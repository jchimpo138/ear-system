#include "DisneyBeacons.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <LSM6DS3.h>
#include <bluefruit.h>

// Bring-up test: poll the onboard LSM6DS3TR-C accelerometer/gyro and print
// readings, before committing to interrupt-driven motion wake. Takes over
// setup()/loop() entirely -- highest-priority test mode, checked before
// FET1_TOGGLE_TEST. The Seeed fork of this library already special-cases
// this exact board (TARGET_SEEED_XIAO_NRF52840_SENSE): it redirects Wire to
// the internal Wire1 bus and auto-powers PIN_LSM6DS3TR_C_POWER inside
// begin(), so no manual pin/bus setup is needed here.
#define IMU_POLL_TEST 0

// --- Pin Definitions (Seeed XIAO nRF52840 Sense) ---
#define FET1_GATE_PIN D0 // 5V LED rail switch (SSM3J328R gate via S8050 level-shifter)
#define LED_PIN D2       // P0.28 (NeoPixel/SK6812 Data)

// Bring-up test: rail-gated color cycle so the rail can be watched with a
// meter. The rail is only powered while actively pushing pixel data (see
// loop()) -- this is the zero-idle-drain behavior FET1 exists for.
// Flip back to 0 to build the full production app instead.
//
// Pixel output goes through Adafruit_NeoPixel, not FastLED: FastLED's nRF52
// clockless driver is confirmed broken on this exact board (hard-crashes on
// .show(), confirmed via bisection here and documented upstream --
// https://github.com/FastLED/FastLED/issues/1648,
// https://github.com/FastLED/FastLED/issues/2061). Adafruit_NeoPixel is the
// community-confirmed working driver for this board.
#define FET1_TOGGLE_TEST 0

// Each of these is independently wireable -- flip one on once its hardware
// is actually connected, without needing the others wired too.
#define HAS_BUTTON 1 // D3, momentary switch to GND (INPUT_PULLUP)
#define HAS_HAPTIC 1 // D4, pre-built vibration motor driver module
#define HAS_BATTERY_ADC 1

#if HAS_BUTTON
#define BUTTON_PIN D3
#endif
#if HAS_HAPTIC
#define HAPTIC_PIN D4
#endif
#if HAS_BATTERY_ADC
// The XIAO Sense has a built-in battery-sense circuit -- no external divider
// needed. PIN_VBAT (P0.31, AIN7) and VBAT_ENABLE (P0.14) are already defined
// by the board variant.
//
// VBAT_ENABLE is held permanently LOW (divider always connected) rather than
// toggled around reads. The real hazard -- P0.31 getting pulled up through
// the 1M resistor into the nRF52840's internal ESD clamp (~3.6V, right at
// the GPIO absolute max) when VBAT_ENABLE sits HIGH -- depends only on BAT+
// voltage, not on which charger put it there. This board's onboard BQ25101
// charger (~CHG on P0.17) only engages when USB is plugged into the XIAO's
// own port; the actual product charges through the IP5310 instead (see
// CLAUDE.md), which never touches that pin at all -- so a charging-state
// gate is blind to the real-world charging path and can't actually protect
// against it. Holding the divider connected permanently costs ~2.3uA
// continuously, negligible next to the 18650's own self-discharge.
// https://wiki.seeedstudio.com/XIAO_BLE/ (FAQ Q3)
#endif

#define NUM_LEDS 31
#define LED_BRIGHTNESS 120

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRBW + NEO_KHZ800);

#if IMU_POLL_TEST
LSM6DS3 myIMU(I2C_MODE, 0x6A); // per the library's own example for this board
#endif

// Shared with loop()'s rail-gating logic -- anything that toggles
// FET1_GATE_PIN outside of loop() (e.g. runBatterySweepAnimation()) must
// keep this in sync, or loop() and reality disagree about the rail state.
bool railOn = false;

// --- Haptic Feedback Helper (non-blocking) ---
#if HAS_HAPTIC
// Motor is on the raw battery rail (~3.0-4.2V), rated for 3.0V/100mA. 80%
// PWM duty brings the sustained average down near its rated point (e.g.
// ~3.0V average at a 3.75V resting battery) instead of running it at full
// rail voltage continuously. A brief full-power kick-start is still used
// first, since the motor's ~2.3V start-voltage spec is too close to what
// 80% duty alone would deliver at a low, near-depleted battery.
#define HAPTIC_DUTY_PERCENT 80
#define HAPTIC_KICKSTART_MS 40
// Longest documented pattern is Complex SOS (0x6): 3x250 + 3x500 + 3x250,
// each pulse is 2 steps (kick+sustain) plus a gap step between every pulse
// and between each 3-pulse group -- 9 pulses (18 steps) + 8 internal gaps
// + 2 inter-group gaps = 28 steps. Rounded up for headroom.
#define HAPTIC_MAX_STEPS 32

// Every pattern used to be a chain of hapticPulse() calls, each blocking
// via delay() for its full duration (up to 1000ms for the boot test pulse)
// -- since executeDisneyMicrocode()/manual animations only render from
// loop(), the strip visibly froze for however long a haptic buzz lasted.
// Replaced with a step sequence (kick, sustain, gap, kick, sustain...)
// advanced against millis() in updateHaptic(), called every loop() pass
// exactly like updateScanBoost() -- motor timing is unaffected, rendering
// just keeps running underneath it now.
struct HapticStep {
  uint8_t duty;         // 0 = motor off, used for the double-tap/sharp-pulse gap
  unsigned long durationMs;
};
HapticStep hapticSteps[HAPTIC_MAX_STEPS];
uint8_t hapticStepCount = 0;
uint8_t hapticStepIndex = 0;
unsigned long hapticStepStart = 0;
bool hapticActive = false;

uint8_t appendHapticPulse(uint8_t slot, unsigned long durationMs) {
  uint8_t sustainDuty = (uint8_t)((HAPTIC_DUTY_PERCENT * 255) / 100);
  unsigned long kick = min(durationMs, (unsigned long)HAPTIC_KICKSTART_MS);
  hapticSteps[slot].duty = 255; // full-power kick to overcome static friction
  hapticSteps[slot].durationMs = kick;
  slot++;
  if (durationMs > kick) {
    hapticSteps[slot].duty = sustainDuty;
    hapticSteps[slot].durationMs = durationMs - kick;
    slot++;
  }
  return slot;
}

uint8_t appendHapticGap(uint8_t slot, unsigned long durationMs) {
  hapticSteps[slot].duty = 0;
  hapticSteps[slot].durationMs = durationMs;
  return slot + 1;
}

// `count` identical pulses separated by `gapMs` gaps (no trailing gap after
// the last one) -- the repeated shape behind Tap/Double Tap/Triple Tap/
// Ticks below.
uint8_t appendHapticPulses(uint8_t slot, uint8_t count, unsigned long pulseMs,
                            unsigned long gapMs) {
  for (uint8_t n = 0; n < count; n++) {
    if (n > 0) slot = appendHapticGap(slot, gapMs);
    slot = appendHapticPulse(slot, pulseMs);
  }
  return slot;
}

// Pattern table matches PROTOCOL.md sec. 5 (Haptic Vibration Engine) --
// codes are the low nibble of a real show packet's VibeByte (0xB0 |
// VibeCode). Previously only a handful of ad-hoc patterns existed here
// (button-feedback only, never driven by real packets) and several didn't
// match the documented timing at all (0x07 was 600ms vs. the documented
// 2.0s, 0x0A was two 300ms pulses vs. one 500ms pulse, 0x0B was a bare
// 300ms default vs. the documented 1000ms). Rebuilt against the actual
// table so real show-triggered haptics feel like the documented patterns,
// not whatever happened to be convenient for local button testing.
void triggerHapticPattern(uint8_t patternCode) {
  Serial.printf("📳 [HAPTIC] Triggering pattern: 0x%02X on D4 (P0.04)\n", patternCode);
  uint8_t slot = 0;
  if (patternCode == 0xFF) {
    // Brief full-power boot self-test pulse -- not a real protocol vibe
    // code (those only occupy the low nibble, 0x0-0xF); kept as a distinct
    // sentinel so it can never collide with a real "Reserved" code (0xC-0xF)
    // decoded from a live packet. Short on purpose: this fires on every
    // power-up just to confirm the motor is alive, not to be felt as a
    // real notification.
    slot = appendHapticPulse(slot, 750);
  } else {
    switch (patternCode & 0x0F) {
      case 0x1: case 0x9: // Tap / Single Tap (0x9 is a documented alias of 0x1)
        slot = appendHapticPulses(slot, 1, 250, 0);
        break;
      case 0x2: // Double Tap: 2x250ms taps, 100ms gap
        slot = appendHapticPulses(slot, 2, 250, 100);
        break;
      case 0x3: // Triple Tap: 3x250ms taps, 100ms gap
        slot = appendHapticPulses(slot, 3, 250, 100);
        break;
      case 0x4: // Pulse Combo: 2x250ms taps + 1x500ms pulse
        slot = appendHapticPulses(slot, 2, 250, 100);
        slot = appendHapticGap(slot, 100);
        slot = appendHapticPulse(slot, 500);
        break;
      case 0x5: // Fast Pattern: 4x250ms taps + 1x500ms + 1x250ms tap
        slot = appendHapticPulses(slot, 4, 250, 100);
        slot = appendHapticGap(slot, 100);
        slot = appendHapticPulse(slot, 500);
        slot = appendHapticGap(slot, 100);
        slot = appendHapticPulse(slot, 250);
        break;
      case 0x6: // Complex SOS: 3x250ms + 3x500ms + 3x250ms
        slot = appendHapticPulses(slot, 3, 250, 100);
        slot = appendHapticGap(slot, 100);
        slot = appendHapticPulses(slot, 3, 500, 100);
        slot = appendHapticGap(slot, 100);
        slot = appendHapticPulses(slot, 3, 250, 100);
        break;
      case 0x7: // Heavy Rumble: continuous 2.0s heavy vibration
        slot = appendHapticPulse(slot, 2000);
        break;
      case 0x8: // Ticks: 6x125ms rapid ticks, 50ms gap
        slot = appendHapticPulses(slot, 6, 125, 50);
        break;
      case 0xA: // Sharp Pulse: 1x500ms medium pulse
        slot = appendHapticPulse(slot, 500);
        break;
      case 0xB: // Notification Pulse: 1x1000ms long pulse
        slot = appendHapticPulse(slot, 1000);
        break;
      case 0x0: // None
      default:  // 0xC-0xF Reserved -- also off
        break;
    }
  }

  if (slot == 0) {
    // "None"/Reserved code, or nothing queued -- cut off any previous
    // pattern still mid-run instead of leaving it to finish on its own.
    hapticActive = false;
    analogWrite(HAPTIC_PIN, 0);
    return;
  }

  // A new trigger always overrides whatever pattern is currently mid-run
  // (e.g. a fresh button press while a previous buzz is still finishing).
  hapticStepCount = slot;
  hapticStepIndex = 0;
  hapticStepStart = millis();
  hapticActive = true;
  analogWrite(HAPTIC_PIN, hapticSteps[0].duty);
}

// Real show packets encode the vibe code in the last payload byte as
// 0xB0 | VibeCode (PROTOCOL.md sec. 5) -- but only for the commands that
// document a VibeByte field at all (E9 05/06/08/09/0E/11/12/14 and the
// generic-show fallback). E9 0C's baked animations (Rainbow/Blink White/
// Orange Blink) repurpose their trailing byte(s) as signature/disambiguation
// data instead, not a real vibe code (confirmed: those bytes are 0x95/0xB0/
// 0x95, and PROTOCOL.md's own E9 0C section never lists a VibeByte) -- so
// callers must not invoke this for 0x0C. The 0xF0 top-nibble check is a
// belt-and-suspenders guard against decoding some other field as a bogus
// pattern on a command whose real vibe-byte position isn't actually
// confirmed yet.
void triggerHapticFromShowByte(uint8_t vibeByte) {
  if ((vibeByte & 0xF0) != 0xB0) return;
  triggerHapticPattern(vibeByte & 0x0F);
}

void updateHaptic(unsigned long now) {
  if (!hapticActive)
    return;
  if (now - hapticStepStart >= hapticSteps[hapticStepIndex].durationMs) {
    hapticStepIndex++;
    hapticStepStart = now;
    if (hapticStepIndex >= hapticStepCount) {
      analogWrite(HAPTIC_PIN, 0);
      hapticActive = false;
      return;
    }
    analogWrite(HAPTIC_PIN, hapticSteps[hapticStepIndex].duty);
  }
}
#endif // HAS_HAPTIC

// --- Battery Functions ---
#if HAS_BATTERY_ADC
static float filteredBatteryVolts = 0.0f;

// VBAT_ENABLE is held permanently LOW (see the note above HAS_BATTERY_ADC) --
// setup() does that once at boot, so reading is always just a direct sample.
float readBatteryVoltage() {
  analogReadResolution(12);
  analogReference(AR_INTERNAL_2_4); // XIAO Sense's documented VBAT reference

  uint32_t totalAdc = 0;
  for (int i = 0; i < 32; i++) {
    totalAdc += analogRead(PIN_VBAT);
  }
  float rawAdc = totalAdc / 32.0f;
  float adcVoltage = (rawAdc / 4095.0f) * 2.4f;
  float instantVolts = adcVoltage * (1510.0f / 510.0f); // undo the onboard divider

  if (filteredBatteryVolts == 0.0f) {
    filteredBatteryVolts = instantVolts;
  } else {
    filteredBatteryVolts = (filteredBatteryVolts * 0.92f) + (instantVolts * 0.08f);
  }
  return filteredBatteryVolts;
}

int calculateBatteryPercentage(float volts) {
  // Breakpoints read off the Amprius INR18650/40 (SA110) 1A discharge curve
  // (Mooch test data) for the actual cell used in this build, not a generic
  // Li-ion approximation.
  if (volts >= 4.20f) return 100;
  if (volts <= 3.00f) return 0;
  float pct = 0.0f;
  if (volts >= 3.95f) {
    pct = 75.0f + ((volts - 3.95f) / (4.20f - 3.95f)) * 25.0f;
  } else if (volts >= 3.70f) {
    pct = 50.0f + ((volts - 3.70f) / (3.95f - 3.70f)) * 25.0f;
  } else if (volts >= 3.40f) {
    pct = 25.0f + ((volts - 3.40f) / (3.70f - 3.40f)) * 25.0f;
  } else {
    pct = ((volts - 3.00f) / (3.40f - 3.00f)) * 25.0f;
  }
  int finalPct = (int)(pct + 0.5f);
  return constrain(finalPct, 0, 100);
}

// --- Low Battery Warning (vibration + red flash) ---
// Previously the only way to see battery state was a deliberate long-press
// (runBatterySweepAnimation()) or the one-time boot sweep -- nothing warned
// passively during normal wear. For a permanently-installed, non-swappable
// single 18650 worn for a multi-hour event, that meant the only real signal
// of a dying battery was the LEDs going dark mid-show. This samples the
// battery every LOW_BATTERY_CHECK_INTERVAL_MS and, if it's at or below
// LOW_BATTERY_THRESHOLD_PCT, fires a buzz + a few red flashes -- no more
// often than LOW_BATTERY_WARN_COOLDOWN_MS apart, so it nags periodically
// instead of on every single check while low.
#define LOW_BATTERY_THRESHOLD_PCT 15
#define LOW_BATTERY_CHECK_INTERVAL_MS (240UL * 1000UL)
#define LOW_BATTERY_WARN_COOLDOWN_MS (480UL * 1000UL)
#define LOW_BATTERY_FLASH_COUNT 3
#define LOW_BATTERY_FLASH_ON_MS 150
#define LOW_BATTERY_FLASH_OFF_MS 150

unsigned long lastBatteryCheckTime = 0;
unsigned long lastLowBatteryWarnTime = 0;
bool lowBatteryFlashActive = false;
uint8_t lowBatteryFlashStep = 0; // counts ON/OFF half-cycles
unsigned long lowBatteryFlashStepStart = 0;

void startLowBatteryWarning(float volts, int pct) {
  Serial.printf("🔋⚠️  [BATTERY] LOW: %.2fV (%d%%) -- warning!\n", volts, pct);
#if HAS_HAPTIC
  triggerHapticPattern(0x03); // Triple Tap -- distinct from any show-driven pattern
#endif
  lowBatteryFlashActive = true;
  lowBatteryFlashStep = 0;
  lowBatteryFlashStepStart = millis();
}

// Called every loop() pass (like updateScanBoost()/updateHaptic()) to
// sample the battery and decide whether to (re-)trigger the warning.
void updateLowBatteryMonitor(unsigned long now) {
  if (now - lastBatteryCheckTime < LOW_BATTERY_CHECK_INTERVAL_MS)
    return;
  lastBatteryCheckTime = now;

  float vBat = readBatteryVoltage();
  int pct = calculateBatteryPercentage(vBat);
  if (pct <= LOW_BATTERY_THRESHOLD_PCT &&
      (lastLowBatteryWarnTime == 0 ||
       now - lastLowBatteryWarnTime >= LOW_BATTERY_WARN_COOLDOWN_MS)) {
    lastLowBatteryWarnTime = now;
    startLowBatteryWarning(vBat, pct);
  }
}

// Advances the flash step timer -- kept separate from the actual
// pixel-writing (renderLowBatteryFlash(), called from loop()'s render
// dispatch) so the warning can preempt whatever's currently on the strip
// without the two pieces needing to know about each other's internals.
void updateLowBatteryFlash(unsigned long now) {
  if (!lowBatteryFlashActive)
    return;
  unsigned long stepMs = (lowBatteryFlashStep % 2 == 0) ? LOW_BATTERY_FLASH_ON_MS
                                                          : LOW_BATTERY_FLASH_OFF_MS;
  if (now - lowBatteryFlashStepStart >= stepMs) {
    lowBatteryFlashStep++;
    lowBatteryFlashStepStart = now;
    if (lowBatteryFlashStep >= LOW_BATTERY_FLASH_COUNT * 2) {
      lowBatteryFlashActive = false;
    }
  }
}

void renderLowBatteryFlash() {
  bool on = (lowBatteryFlashStep % 2 == 0);
  if (on) {
    for (int i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, strip.Color(255, 0, 0)); // red -- universal low-battery warning color
    }
  } else {
    strip.clear();
  }
  strip.show();
}

uint16_t batteryGaugeHue(int i) {
  float ratio = (float)i / (float)(NUM_LEDS - 1);
  if (ratio <= 0.5f) {
    return (uint16_t)((ratio / 0.5f) * 43690.0f);
  }
  return 43690 - (uint16_t)(((ratio - 0.5f) / 0.5f) * 21845.0f);
}

void runBatterySweepAnimation() {
  // Reads (via VBAT_ENABLE/PIN_VBAT) don't need the LED rail, but the
  // display that follows does -- self-contained gating since this can be
  // called from setup() (no display shown yet) or from loop()'s button
  // handler (display state varies).
  float vBat = readBatteryVoltage();

  int pct = calculateBatteryPercentage(vBat);
  int targetLeds = (pct * NUM_LEDS) / 100;
  if (targetLeds < 5) targetLeds = 5;

  Serial.printf("\n🔋 [BATTERY] Voltage: %.2fV | Pct: %d%% | LEDs: %d\n",
                vBat, pct, targetLeds);

  digitalWrite(FET1_GATE_PIN, HIGH);
  railOn = true;
  delay(20); // let the boost rail + SK6812 ICs settle before clocking data

  strip.clear();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.show();

  // Fixed 100% reference marker at the very end of the strip (solid, on
  // the SK6812's dedicated white channel so it reads as distinct from the
  // rainbow-gradient level bar) -- shows how much headroom is left to full.
  strip.setPixelColor(NUM_LEDS - 1, strip.Color(0, 0, 0, 255));

  for (int i = 0; i < targetLeds; i++) {
    strip.setPixelColor(i, strip.ColorHSV(batteryGaugeHue(i), 255, 255));
    strip.show();
    delay(30);
  }

  // Flash the top (current level) LED a few times so the exact gauge
  // reading is easy to spot instead of blending into the solid bar.
  int topLed = targetLeds - 1;
  uint16_t topHue = batteryGaugeHue(topLed);
  for (int flash = 0; flash < 5; flash++) {
    strip.setPixelColor(topLed, strip.ColorHSV(topHue, 255, 255));
    strip.show();
    delay(250);
    strip.setPixelColor(topLed, 0);
    strip.show();
    delay(250);
  }
  strip.setPixelColor(topLed, strip.ColorHSV(topHue, 255, 255));
  strip.show();

  for (int b = 255; b >= 0; b -= 15) {
    for (int i = 0; i < targetLeds; i++) {
      strip.setPixelColor(i, strip.ColorHSV(batteryGaugeHue(i), 255, b));
    }
    strip.show();
    delay(20);
  }

  strip.clear();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.show();

  digitalWrite(FET1_GATE_PIN, LOW);
  railOn = false;
}
#endif // HAS_BATTERY_ADC

// --- Disney Beacon Global State ---
volatile bool disneyDeviceFound = false;
volatile unsigned long lastShowSyncTime = 0;
volatile uint8_t showR1 = 255;
volatile uint8_t showG1 = 0;
volatile uint8_t showB1 = 0;

volatile uint8_t showR2 = 0;
volatile uint8_t showG2 = 0;
volatile uint8_t showB2 = 255;
volatile bool isDualColor = false;

// How long the current show holds before expiring, per its own Timing Byte
// (see PROTOCOL.md "Timing Byte Specification") instead of one fixed
// cutoff for every command -- this is what makes ears hold-time match a
// real MagicBand+'s, which varies per show (observed ~24-29s for a
// TIME_VAL=15/scaler=0 command, not a blanket few seconds).
volatile unsigned long currentShowDurationMs = 4000;
volatile bool currentShowAlwaysOn = false;

// Decodes a Disney show Timing Byte into (duration, always-on).
// Bit7 ALWAYS_ON, Bit6 SCALER, Bits5-4 FADE_CODE (unused here), Bits3-0 TIME_VAL.
// Duration = 3.1*TIME_VAL + 5.5 (scaler=1) or 1.5*TIME_VAL + 6.5 (scaler=0) seconds.
//
// ALWAYS_ON does NOT mean literally forever on real hardware -- confirmed
// live (E9 0E, TIME_VAL=3, ALWAYS_ON set) that the real band actually
// turns off after ~24s, not indefinitely. Only one data point exists so
// the real formula for ALWAYS_ON's duration isn't known; applying a fixed
// 24s cap instead of the previous true-forever behavior, which would have
// left a show running until a new command arrived (unlike the real band).
#define ALWAYS_ON_CAP_MS 24000
void applyShowTiming(uint8_t timingByte) {
  bool alwaysOnFlag = (timingByte & 0x80) != 0;
  bool scaler = (timingByte & 0x40) != 0;
  uint8_t timeVal = timingByte & 0x0F;
  float durationSec = scaler ? (3.1f * timeVal + 5.5f) : (1.5f * timeVal + 6.5f);
  currentShowAlwaysOn = false; // never treat a show as literally infinite
  currentShowDurationMs = alwaysOnFlag ? ALWAYS_ON_CAP_MS
                                        : (unsigned long)(durationSec * 1000.0f);
}

// --- CC 03 Wake-Ping Scan Boost ---
// Per PROTOCOL.md: park infrastructure sends a 500ms "CC 03 00 00 00" wake
// ping (25ms advertising interval) before the real 3000ms show payload
// burst -- ~3.5s total. Our idle scan is deliberately lazy (~4% duty, see
// setup()) to save battery, so on its own it might miss the actual show
// command. On a wake ping, temporarily switch to a near-continuous scan for
// long enough to cover that whole burst, then drop back to the lazy default.
#define SCAN_INTERVAL_LAZY 1600   // 1000ms
#define SCAN_WINDOW_LAZY   64     // 40ms  (~4% duty)
#define SCAN_INTERVAL_BOOST 32    // 20ms
#define SCAN_WINDOW_BOOST   32    // 20ms  (100% duty, matches the 25ms TX interval)
#define SCAN_BOOST_DURATION_MS 4000 // covers the 500ms wake + 3000ms payload with margin

volatile bool scanBoosted = false;
volatile unsigned long scanBoostUntil = 0;

void enterBoostedScan() {
  if (scanBoosted) {
    scanBoostUntil = millis() + SCAN_BOOST_DURATION_MS; // extend on repeat wake pings
    return;
  }
  Serial.println("📡 [SCAN] CC03 wake ping detected -- boosting scan rate");
  Bluefruit.Scanner.stop();
  Bluefruit.Scanner.setInterval(SCAN_INTERVAL_BOOST, SCAN_WINDOW_BOOST);
  Bluefruit.Scanner.start(0);
  scanBoosted = true;
  scanBoostUntil = millis() + SCAN_BOOST_DURATION_MS;
}

void updateScanBoost(unsigned long now) {
  if (scanBoosted && now > scanBoostUntil) {
    Serial.println("📡 [SCAN] Boost window expired -- back to lazy scan");
    Bluefruit.Scanner.stop();
    Bluefruit.Scanner.setInterval(SCAN_INTERVAL_LAZY, SCAN_WINDOW_LAZY);
    Bluefruit.Scanner.start(0);
    scanBoosted = false;
  }
}

// Function declarations
void scan_callback(ble_gap_evt_adv_report_t *report);
void parseDisneyPacket(const uint8_t *payload, uint16_t len);

// --- Animation Functions ---
void runIgnitionPulse(uint8_t gHue);
void runMeteorChase();
void runCyberPlasma();
void runLightningStorm();
void runNeonGlitch();
void runHyperDrive(uint8_t gHue);

// Manual Button Control State
volatile uint8_t manualMode = 0; // 0 = Off, 1-7 = Custom Animations (BLE shows override either way)
const uint8_t NUM_MANUAL_MODES = 8;
const char* MODE_NAMES[] = {
    "Off",
    "Rainbow Wave",
    "Ignition Pulse",
    "Meteor Chase",
    "Cyber Plasma",
    "Lightning Storm",
    "Neon Glitch",
    "HyperDrive"
};

bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 50;
uint8_t gHue = 0;

// --- Disney Microcode Animation Drivers ---
volatile uint8_t receivedCommandType = 0;
volatile uint8_t showColors5[5][3]; // 5-slot palette RGB matrix
volatile uint8_t showColors5Count = 5; // how many of the 5 slots are actually valid (0x0C can have fewer)
volatile unsigned long showBlinkIntervalMs = 50; // per-command blink/strobe rate, see receivedCommandType==0x0E render

// --- Family 3 (Park Infrastructure) E9 10 decode state ---
// Bench-confirmed against a real MagicBand+ (see PROTOCOL.md sec. 6.1): 5
// independently-addressable LED slots (Center, NE, SE, SW, NW on the real
// band), each payload byte packing (mode<<5)|paletteIndex. Kept separate
// from showColors5 above since that one belongs to the unrelated E9 09
// (Family 1) handler.
volatile uint8_t showColorsPark5[5][3];
volatile uint8_t parkPatternId = 0; // payload[12] -- only 0x30/0x31 confirmed as a rotating chase
volatile bool parkSpin = false;        // computed at parse time, see parseDisneyPacket()
volatile bool parkSpinReverse = false;

// Real SK6812 white channel instead of faking white via R=G=B on the RGB
// LEDs -- the 3-arg Color() always encodes W=0, so driving all 3 color
// LEDs to fake white burns more current and looks cooler/dimmer than the
// dedicated white diode. Covers both a literal (255,255,255) source (e.g.
// E9 0C's baked "Blink White") and DISNEY_PALETTE's White/White 2 entries
// (255,200,180 -- a warm-white RGB approximation), which is what any
// palette-driven show color actually resolves to when the real band picks
// White. Previously only the E9 0E/0C strobe branch checked for this, and
// only the literal-255 case, so every other palette-driven render (E9 09,
// 0x10/0xFE park zones, dual-color holds, etc.) missed real White entirely.
uint32_t neoColor(uint8_t r, uint8_t g, uint8_t b) {
  bool isWhite = (r == 255 && g == 255 && b == 255) ||
                 (r == 255 && g == 200 && b == 180);
  return isWhite ? strip.Color(0, 0, 0, 255) : strip.Color(r, g, b);
}

#if !FET1_TOGGLE_TEST
void executeDisneyMicrocode() {
  if (receivedCommandType == 0xC1) {
    // ✨ STARLIGHT BUBBLE WAND: continuous sparkle for the show's real
    // duration (governed by loop()'s Timing-Byte expiry, not a hardcoded
    // frame count). Elapsed-time-throttled instead of delay()-blocked --
    // called every loop() pass but only renders a new frame every 80ms, so
    // BLE scanning/button/haptic/rail-gating keep running in between.
    const unsigned long FRAME_INTERVAL_MS = 80;
    static unsigned long lastFrame = 0;
    if (millis() - lastFrame >= FRAME_INTERVAL_MS) {
      lastFrame = millis();
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, neoColor(showR1, showG1, showB1));
      }
      // Real SK6812 white channel (4-arg Color()) instead of faking white
      // via R=G=B=255 -- same fix already applied to the E9 0B/0C/0E white
      // renders tonight. Bumped from 2 to 4 sparkle pixels since 2 of 31
      // was proportionally too subtle to read as "sparkle" against the
      // solid-color fill.
      for (int s = 0; s < 4; s++) {
        strip.setPixelColor(random(NUM_LEDS), strip.Color(0, 0, 0, 255));
      }
      strip.show();
    }
  } else if (receivedCommandType == 0xC4) {
    // 🗿 FAB 50 STATUE: golden swirl with rhythmic sparkles and a two-beat
    // breathing pulse, adapted from the Adafruit reference renderer's own
    // statue animation (research/BLE_Beacon_Ears, _state_statue_beacon())
    // -- their richer version syncs sparkle probability to the pulse peaks
    // and has a proper fade-in/fade-out envelope, versus our previous
    // simple fading-trail swirl. Their design uses a separate center pixel
    // per physical "zone" (their dual-jewel hardware); dropped here since
    // our linear strip has no equivalent -- the whole strip acts as their
    // "outer ring."
    const unsigned long FRAME_INTERVAL_MS = 30;
    static unsigned long lastFrame = 0;
    if (millis() - lastFrame >= FRAME_INTERVAL_MS) {
      lastFrame = millis();
      float t = (millis() - lastShowSyncTime) / 1000.0f;

      // Fade-in over the first 0.3s, fade-out over the last 0.3s of the
      // 4.0s show duration (matches currentShowDurationMs for this command).
      float fadeEnvelope;
      if (t < 0.3f) fadeEnvelope = t / 0.3f;
      else if (t > 3.7f) fadeEnvelope = max(0.0f, (4.0f - t) / 0.3f);
      else fadeEnvelope = 1.0f;

      // Two-beat pulse: dim at t=0/2/4, peak at t=1/3. Range kept to
      // 0.5-1.0 so the swirl never fully disappears between peaks.
      float pulse = 0.75f + 0.25f * sinf(2.0f * PI * t / 2.0f - PI / 2.0f);
      float envelope = fadeEnvelope * pulse;

      // ~1.5 revolutions/sec, continuous (no reset between beats).
      float phase = t * 1.5f;
      float headPos = fmodf(phase * NUM_LEDS, (float)NUM_LEDS);
      if (headPos < 0) headPos += NUM_LEDS;

      const float goldBright[3] = {255, 180, 30};
      const float goldDim[3] = {120, 80, 10};
      const float sparkleWhite[3] = {255, 255, 200};

      for (int i = 0; i < NUM_LEDS; i++) {
        // Sparkle probability rises slightly on pulse peaks so sparkles
        // cluster rhythmically with the beat instead of feeling random.
        float sparklePhase = t * 12.0f + i * 1.7f;
        if (sinf(sparklePhase) > 0.88f - 0.06f * pulse) {
          strip.setPixelColor(i, strip.Color((uint8_t)(sparkleWhite[0] * envelope),
                                              (uint8_t)(sparkleWhite[1] * envelope),
                                              (uint8_t)(sparkleWhite[2] * envelope)));
          continue;
        }
        float distance = fmodf(headPos - i, (float)NUM_LEDS);
        if (distance < 0) distance += NUM_LEDS;
        if (distance < 1.0f) {
          strip.setPixelColor(i, strip.Color((uint8_t)(goldBright[0] * envelope),
                                              (uint8_t)(goldBright[1] * envelope),
                                              (uint8_t)(goldBright[2] * envelope)));
        } else if (distance < 3.0f) {
          float fade = 1.0f - (distance / 3.0f);
          strip.setPixelColor(i, strip.Color((uint8_t)(goldDim[0] * envelope * fade),
                                              (uint8_t)(goldDim[1] * envelope * fade),
                                              (uint8_t)(goldDim[2] * envelope * fade)));
        } else {
          strip.setPixelColor(i, 0);
        }
      }
      strip.show();
    }
  } else if (receivedCommandType == 0x09) {
    // 🎨 5-COLOR PALETTE RING: 5 distinct color segments
    int segSize = NUM_LEDS / 5;
    for (int s = 0; s < 5; s++) {
      uint32_t c = neoColor(showColors5[s][0], showColors5[s][1], showColors5[s][2]);
      int startIdx = s * segSize;
      int count = (s == 4) ? (NUM_LEDS - startIdx) : segSize;
      for (int i = startIdx; i < startIdx + count; i++) {
        strip.setPixelColor(i, c);
      }
    }
    strip.show();
  } else if (receivedCommandType == 0xFB) {
    // ⚪ E9 0B HIGH-CONTRAST CIRCLE: a 6-pixel-wide cluster of white dots
    // chasing around the ring together, dark otherwise -- confirmed live
    // against a real MagicBand+ (started as a single dot, tuned to a
    // 6-wide cluster + faster speed per direct comparison against the real
    // band's animation). Uses the SK6812's real white channel, same as the
    // E9 0C/0E white fix earlier.
    const unsigned long STEP_MS = 40;
    const int DOT_COUNT = 6;
    int pos = (int)((millis() / STEP_MS) % NUM_LEDS);
    strip.clear();
    for (int d = 0; d < DOT_COUNT; d++) {
      int i = (pos + d) % NUM_LEDS;
      strip.setPixelColor(i, strip.Color(0, 0, 0, 255));
    }
    strip.show();
  } else if (receivedCommandType == 0xFA) {
    // 🎡 E9 0E 5-COLOR WHEEL: real decoded colors (showColors5) rotating
    // in discrete jumps, same mechanism as E9 0C's "Taste the Rainbow"
    // (0xFC below) -- confirmed via the Adafruit reference renderer using
    // the identical approach for its generic "animation" bucket (E9 0B/
    // 0E/0F), just fed real payload colors instead of a hardcoded list.
    static const float ROTATE_PERIOD_S = 2.0f;
    float phase = (float)millis() / 1000.0f / ROTATE_PERIOD_S;
    int n = showColors5Count > 0 ? showColors5Count : 1;
    for (int i = 0; i < NUM_LEDS; i++) {
      float angleFrac = (float)i / NUM_LEDS;
      float colorPhase = phase + angleFrac;
      int slot = ((int)(colorPhase * n)) % n;
      if (slot < 0) slot += n;
      strip.setPixelColor(i, neoColor(showColors5[slot][0], showColors5[slot][1],
                                       showColors5[slot][2]));
    }
    strip.show();
  } else if (receivedCommandType == 0x0E) {
    // ⚡ STROBE/BLINK PULSE: alternate showR1/G1/B1/black continuously for
    // the show's real duration -- same non-blocking throttle pattern. Uses
    // the current show color and showBlinkIntervalMs (set in
    // parseDisneyPacket() per command) rather than a single hardcoded rate
    // so this same branch can be reused for E9 0C's "Blink White" and
    // "Orange Blink" baked animations, which are visually just this
    // strobe/blink in a different color and speed -- confirmed live that
    // the real band's "Blink White" is a much slower ~0.5s on/off blink.
    // (Genuine E9 0E no longer uses this branch -- see 0xFA above.)
    static unsigned long lastFrame = 0;
    static bool strobeOn = false;
    if (millis() - lastFrame >= showBlinkIntervalMs) {
      lastFrame = millis();
      strobeOn = !strobeOn;
      if (strobeOn) {
        // Pure/palette white uses the SK6812's dedicated white channel via
        // neoColor() (matching the battery gauge's reference marker)
        // instead of faking it with R=G=B -- the 3-arg form always encodes
        // W=0, so that would drive all 3 color LEDs instead of the
        // cleaner/more efficient real white LED. Any other color (e.g.
        // Orange Blink) isn't representable on the white channel, so it
        // still goes through R/G/B as before.
        uint32_t c = neoColor(showR1, showG1, showB1);
        for (int i = 0; i < NUM_LEDS; i++) {
          strip.setPixelColor(i, c);
        }
      } else {
        strip.clear();
      }
      strip.show();
    }
  } else if (receivedCommandType == 0xFC) {
    // 🌈 E9 0C KNOWN "TASTE THE RAINBOW" ANIMATION: firmware-baked program
    // on the real band, not real palette data. Matches the real band's
    // actual look (per user observation) better with discrete color-band
    // jumps than a smooth HSV blend -- adapted from the Adafruit reference
    // renderer's own render() for this exact signature (research/
    // BLE_Beacon_Ears), which uses 6 hardcoded colors and int()-truncated
    // slot indices rather than a continuous hue sweep. Their version splits
    // center-vs-outer-ring across two physical NeoPixel jewels; adapted
    // here to our single linear strip using their outer-ring formula
    // (spatial position + time both feed the same discrete slot index).
    static const uint8_t RAINBOW6[6][3] = {
        {255, 0, 0}, {255, 90, 0}, {255, 220, 0}, {0, 255, 0}, {0, 120, 255}, {180, 0, 255}};
    const float ROTATE_PERIOD_S = 2.0f; // matches the reference renderer's own pacing
    float phase = (float)millis() / 1000.0f / ROTATE_PERIOD_S;
    for (int i = 0; i < NUM_LEDS; i++) {
      float angleFrac = (float)i / NUM_LEDS;
      float colorPhase = phase + angleFrac;
      int slot = ((int)(colorPhase * 6.0f)) % 6;
      if (slot < 0) slot += 6; // defensive: millis()-derived phase is always >=0, but keep % well-defined
      strip.setPixelColor(i, strip.Color(RAINBOW6[slot][0], RAINBOW6[slot][1], RAINBOW6[slot][2]));
    }
    strip.show();
  } else if (receivedCommandType == 0x0C) {
    // 🎨 E9 0C GENERIC PALETTE CYCLE: real decoded colors (showColors5[0..
    // showColors5Count-1]) rotating around the strip over time, for any
    // E9 0C capture that isn't the specific known-baked signature above.
    const unsigned long STEP_MS = 400;
    int n = showColors5Count > 0 ? showColors5Count : 1;
    int phase = (int)(millis() / STEP_MS);
    int segSize = NUM_LEDS / n;
    for (int s = 0; s < n; s++) {
      int srcSlot = (s + phase) % n;
      uint32_t c = neoColor(showColors5[srcSlot][0], showColors5[srcSlot][1],
                            showColors5[srcSlot][2]);
      int startIdx = s * segSize;
      int count = (s == n - 1) ? (NUM_LEDS - startIdx) : segSize;
      for (int i = startIdx; i < startIdx + count; i++) {
        strip.setPixelColor(i, c);
      }
    }
    strip.show();
  } else if (receivedCommandType == 0x12) {
    // 🌊 WAVE PULSE: MagicBand+ replica spinning chaser and center beat.
    // Previously capped at its own hardcoded 6s internal while-loop --
    // removed in favor of running for as long as loop() keeps calling this
    // branch (i.e. the real decoded Timing Byte duration), same as every
    // other show handler.
    const unsigned long BEAT_INTERVAL_MS = 90;
    const unsigned long SPIN_INTERVAL_MS = 35;
    static unsigned long lastFrame = 0;
    static int step = 0;
    int half = NUM_LEDS / 2;
    unsigned long frameInterval = (step % 10 == 0) ? BEAT_INTERVAL_MS : SPIN_INTERVAL_MS;
    if (millis() - lastFrame >= frameInterval) {
      lastFrame = millis();
      int spinPos = step % half;
      if (step % 10 == 0) {
        strip.clear();
        for (int e = 0; e < 2; e++) {
          int offset = e * half;
          for (int c = 4; c < 10; c++) {
            strip.setPixelColor(offset + c, neoColor(showR2, showG2, showB2));
          }
        }
      } else {
        for (int i = 0; i < NUM_LEDS; i++) {
          strip.setPixelColor(i, neoColor(showR1, showG1, showB1));
        }
        strip.setPixelColor(spinPos, neoColor(showR2, showG2, showB2));
        strip.setPixelColor((spinPos + 1) % half, neoColor(showR2, showG2, showB2));
        strip.setPixelColor(half + spinPos, neoColor(showR2, showG2, showB2));
        strip.setPixelColor(half + ((spinPos + 1) % half), neoColor(showR2, showG2, showB2));
      }
      strip.show();
      step++;
    }
  } else if (receivedCommandType == 0x11) {
    // 🌈 PALETTE CROSS-FADE (Center Opposite Outer Ring, per emcot.txt's
    // name for E9 11): confirmed against a real MagicBand+ -- two zones
    // hold opposite colors and continuously swap back and forth (~1s per
    // direction, smooth not a hard cut) for the whole show duration, not a
    // single one-shot uniform blend across the strip like a plain
    // cross-fade would be. Mapped onto our linear strip as two halves,
    // matching the existing half-split convention used by 0x06/0x12.
    // Recomputed fresh every call from millis() -- no blocking loop.
    const float CROSSFADE_PERIOD_MS = 2000.0f; // ~1s each direction
    float phase = sinf(2.0f * PI * (float)millis() / CROSSFADE_PERIOD_MS -
                        PI / 2.0f);
    float blendA = (phase + 1.0f) / 2.0f; // 0..1, smooth continuous oscillation

    // Steepen the curve: hold each zone near-fully-saturated for the outer
    // ~30% of each half-cycle and compress the actual color swap into the
    // middle ~40%. A raw sine still spends a lot of perceptual time in the
    // muddy, low-contrast midpoint blend (where zoneA/zoneB nearly match)
    // -- that's what was reading as "barely any change" on the strip even
    // though the underlying numbers were correct. This keeps the same ~1s
    // per-direction timing but makes the flip read as a clear swap.
    float steep = (blendA - 0.3f) / 0.4f;
    if (steep < 0.0f) steep = 0.0f;
    if (steep > 1.0f) steep = 1.0f;
    blendA = steep * steep * (3.0f - 2.0f * steep); // smoothstep

    // Boost output intensity for this effect specifically (independent of
    // the global LED_BRIGHTNESS dimmer cap used elsewhere) since Disney
    // show colors need to read as vivid/saturated to be noticeable at a
    // glance, not just technically-correct.
    const float XFADE_GAIN = 1.7f;
    auto boost = [XFADE_GAIN](uint8_t v) -> uint8_t {
      float g = v * XFADE_GAIN;
      return (uint8_t)(g > 255.0f ? 255.0f : g);
    };

    uint8_t rA = boost(showR1 + (int16_t)((showR2 - showR1) * blendA));
    uint8_t gA = boost(showG1 + (int16_t)((showG2 - showG1) * blendA));
    uint8_t bA = boost(showB1 + (int16_t)((showB2 - showB1) * blendA));
    uint8_t rB = boost(showR2 + (int16_t)((showR1 - showR2) * blendA));
    uint8_t gB = boost(showG2 + (int16_t)((showG1 - showG2) * blendA));
    uint8_t bB = boost(showB2 + (int16_t)((showB1 - showB2) * blendA));

    int half = NUM_LEDS / 2;
    for (int i = 0; i < half; i++) {
      strip.setPixelColor(i, neoColor(rA, gA, bA));
    }
    for (int i = half; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, neoColor(rB, gB, bB));
    }
    strip.show();
  } else if (receivedCommandType == 0x14) {
    // ⚡ E9 14 FAST FLICKER: same two-color (Center/NE-equivalent) model as
    // E9 10/13's background+chaser render, but bench-confirmed much faster
    // and less predictable than their steady sweeping chaser -- rendered
    // as a fast random flicker between showR1/G1/B1 and showR2/G2/B2
    // across 5 equal zones (same zone convention as 0x09/0x10/0x13) rather
    // than a moving gap, since that read as visibly wrong for this command.
    {
      const unsigned long FRAME_INTERVAL_MS = 90;
      static unsigned long lastFrame = 0;
      static uint8_t zoneState = 0; // bit per zone: 1 = show color2 this frame
      if (millis() - lastFrame >= FRAME_INTERVAL_MS) {
        lastFrame = millis();
        zoneState = random(32);
      }
      int segSize = NUM_LEDS / 5;
      for (int s = 0; s < 5; s++) {
        bool useColor2 = (zoneState >> s) & 0x01;
        uint32_t c = useColor2 ? neoColor(showR2, showG2, showB2)
                                : neoColor(showR1, showG1, showB1);
        int startIdx = s * segSize;
        int count = (s == 4) ? (NUM_LEDS - startIdx) : segSize;
        for (int i = startIdx; i < startIdx + count; i++) {
          strip.setPixelColor(i, c);
        }
      }
      strip.show();
    }
  } else if (receivedCommandType == 0x10) {
    // 🎡 FAMILY 3 PARK SHOW (E9 10 "Alternating Colors"): 5 independently
    // colored zones split across the strip -- the real MagicBand+ has 5
    // discrete physical LEDs (4-LED ring + 1 center), which our linear
    // strip has no direct 1:1 layout for, so this just divides NUM_LEDS
    // into 5 equal zones, same convention already used for 0x09's 5-color
    // ring. parkSpin/parkSpinReverse are decided in parseDisneyPacket():
    // pattern IDs 0x30/0x31 are bench-confirmed to trigger this rotation
    // (direction flipped by the low bit) when top3 is 5-7; for top3 0-4 the
    // real trigger condition isn't confirmed, so it defaults to spinning
    // since that's what the one real-world example of that mode does.
    int segSize = NUM_LEDS / 5;
    int rotateOffset = 0;
    if (parkSpin) {
      const unsigned long STEP_MS = 500; // ~1 zone-hop every half second
      int step = (int)(millis() / STEP_MS);
      rotateOffset = parkSpinReverse ? -step : step;
    }
    for (int s = 0; s < 5; s++) {
      int srcSlot = ((s + rotateOffset) % 5 + 5) % 5;
      uint32_t c = neoColor(showColorsPark5[srcSlot][0], showColorsPark5[srcSlot][1],
                            showColorsPark5[srcSlot][2]);
      int startIdx = s * segSize;
      int count = (s == 4) ? (NUM_LEDS - startIdx) : segSize;
      for (int i = startIdx; i < startIdx + count; i++) {
        strip.setPixelColor(i, c);
      }
    }
    strip.show();
  } else if (receivedCommandType == 0xFE) {
    // 🔴 FAMILY 3 PARK SHOW, top3 0-4 (background + chaser): Center's color
    // fills the strip and NE's color sweeps around it as a moving ~1/5-strip
    // chaser -- confirmed on a real band for both E9 10 (whose real capture
    // has NE=Off, which is why that case reads as a dark gap sweeping a lit
    // background) and E9 13 (whose captures have real NE colors, producing
    // a colored chaser instead). Same two-parameter model for both opcodes,
    // just different NE values in each real-world example.
    const unsigned long STEP_MS = 40;
    const int GAP_WIDTH = NUM_LEDS / 5;
    int pos = (int)((millis() / STEP_MS) % NUM_LEDS);
    uint32_t chaserColor = neoColor(showColorsPark5[1][0], showColorsPark5[1][1],
                                     showColorsPark5[1][2]);
    for (int i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, neoColor(showR1, showG1, showB1));
    }
    for (int d = 0; d < GAP_WIDTH; d++) {
      int i = (pos + d) % NUM_LEDS;
      strip.setPixelColor(i, chaserColor);
    }
    strip.show();
  } else {
    // Standard Hold Display (0x05, 0x06, 0x08, 0x0B, and the Family 3
    // hash-derived fallback for undecoded opcodes)
    if (isDualColor) {
      int half = NUM_LEDS / 2;
      for (int i = 0; i < half; i++) {
        strip.setPixelColor(i, neoColor(showR1, showG1, showB1));
      }
      for (int i = half; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, neoColor(showR2, showG2, showB2));
      }
    } else {
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, neoColor(showR1, showG1, showB1));
      }
    }
    strip.show();
  }
}
#endif // !FET1_TOGGLE_TEST

void setup() {
  Serial.begin(115200);

#if IMU_POLL_TEST
  delay(2000); // give USB serial time to connect
  Serial.println("\n=== IMU polling bring-up test ===");
  if (myIMU.begin() != 0) {
    Serial.println("IMU init FAILED");
  } else {
    Serial.println("IMU init OK");
  }
  return;
#endif

  // FET1 gate: default OFF. The rail is only powered while actively pushing
  // pixel data -- holding it off otherwise is the whole point of FET1
  // (true zero-drain idle vs. a boost converter + LED ICs idling at "black").
  pinMode(FET1_GATE_PIN, OUTPUT);
  digitalWrite(FET1_GATE_PIN, LOW);

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();

#if FET1_TOGGLE_TEST
  Serial.println("\n=== FET1 + NeoPixel rail-gated bring-up test ===");
#else
#if HAS_BUTTON
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif
#if HAS_HAPTIC
  pinMode(HAPTIC_PIN, OUTPUT);
  digitalWrite(HAPTIC_PIN, LOW);

  // 📳 Startup Haptic Test: brief full-power pulse. triggerHapticPattern()
  // only sets the motor's initial duty -- updateHaptic() is what actually
  // steps it down and cuts it off, and that only runs from loop(). Since
  // runBatterySweepAnimation() right below is itself a blocking multi-
  // second delay()-driven sequence that never reaches loop(), the motor
  // was staying pinned at full power for that whole animation instead of
  // stopping after 150ms -- drain the sequencer here first so it actually
  // finishes before the (also blocking) battery sweep starts.
  triggerHapticPattern(0xFF);
  while (hapticActive) {
    updateHaptic(millis());
    delay(5);
  }
#endif
#if HAS_BATTERY_ADC
  // Held permanently LOW -- see the note above HAS_BATTERY_ADC.
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);
#endif

  // Rail stays off at boot -- loop() only powers it while something is
  // actually being displayed (BLE show or a selected manual mode).

  Serial.println("\n=========================================");
  Serial.println("   NRF52840 DISNEY BLE RECEIVER STARTED  ");
  Serial.println("=========================================");

  // Initialize Bluefruit BLE Central / Scanner
  Bluefruit.begin(0, 1); // 0 Peripherals, 1 Central
  Bluefruit.setTxPower(4);

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  // Units are 0.625ms per the BLE GAP scan parameter spec. Lazy default is
  // ~4% duty cycle to save battery; a CC03 wake ping (see enterBoostedScan())
  // temporarily switches to SCAN_*_BOOST for reliable payload capture.
  Bluefruit.Scanner.setInterval(SCAN_INTERVAL_LAZY, SCAN_WINDOW_LAZY);
  Bluefruit.Scanner.filterMSD(
      DISNEY_COMPANY_ID_LE);  // Filter Disney Manufacturer ID
  Bluefruit.Scanner.start(0); // 0 = continuous scan mode with duty cycling

  Serial.println(
      ">>> BLE Scanner initialized & searching for Disney Beacons...");

#if HAS_BATTERY_ADC
  // Run battery sweep animation once at startup
  runBatterySweepAnimation();
#endif
#endif // FET1_TOGGLE_TEST
}

#if IMU_POLL_TEST
void loop() {
  float ax = myIMU.readFloatAccelX();
  float ay = myIMU.readFloatAccelY();
  float az = myIMU.readFloatAccelZ();
  float magnitude = sqrtf(ax * ax + ay * ay + az * az);
  Serial.printf("Accel: X=%.3f Y=%.3f Z=%.3f g | |a|=%.3f g\n", ax, ay, az, magnitude);
  delay(200);
}
#elif FET1_TOGGLE_TEST
void showColorOnRail(const char *name, uint32_t color) {
  Serial.printf("Rail ON  -> %s (5V rail should read ~5.15V)\n", name);
  digitalWrite(FET1_GATE_PIN, HIGH);
  delay(20); // let the boost rail + SK6812 ICs settle before clocking data
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
  delay(2000);

  Serial.println("Rail OFF (SK6812s unpowered -> true 0 drain, not just dark)");
  digitalWrite(FET1_GATE_PIN, LOW);
  delay(2000);
}

void loop() {
  showColorOnRail("RED", strip.Color(255, 0, 0));
  showColorOnRail("GREEN", strip.Color(0, 255, 0));
  showColorOnRail("BLUE", strip.Color(0, 0, 255));
  showColorOnRail("WHITE (4th SK6812 channel)", strip.Color(0, 0, 0, 255));
}
#else
void loop() {
  unsigned long now = millis();
  gHue++;

  updateScanBoost(now);

#if HAS_HAPTIC
  updateHaptic(now);
#endif

#if HAS_BATTERY_ADC
  updateLowBatteryMonitor(now);
  updateLowBatteryFlash(now);
#endif

#if HAS_BUTTON
  // --- Button Read & Debounce (Short-press cycles modes, Long-press > 1.2s triggers Battery Sweep) ---
  bool reading = digitalRead(BUTTON_PIN);
  static unsigned long pressStartTime = 0;
  static bool buttonPressed = false;
  static bool longPressHandled = false;

  if (reading == LOW) {
    if (!buttonPressed) {
      buttonPressed = true;
      pressStartTime = now;
      longPressHandled = false;
    } else if (!longPressHandled && (now - pressStartTime > 1200)) {
      longPressHandled = true;
#if HAS_BATTERY_ADC
      Serial.println("🔘 [BUTTON] Long Press (>1.2s)! Running Battery Sweep...");
#if HAS_HAPTIC
      triggerHapticPattern(0x02); // Double tap haptic pulse
#endif
      runBatterySweepAnimation();
#endif
    }
  } else {
    if (buttonPressed) {
      buttonPressed = false;
      if (!longPressHandled && (now - pressStartTime < 1200) && (now - pressStartTime > DEBOUNCE_DELAY)) {
        manualMode = (manualMode + 1) % NUM_MANUAL_MODES;
        Serial.printf("🔘 [BUTTON] Short Press! Mode %d: %s\n", manualMode, MODE_NAMES[manualMode]);
#if HAS_HAPTIC
        triggerHapticPattern(0x01); // Short single click haptic pulse
#endif
      }
    }
  }
#endif // HAS_BUTTON

  // A show holds for its own decoded Timing Byte duration (ALWAYS_ON shows
  // never expire on their own -- only a new command changes them), matching
  // real MagicBand+ behavior instead of one fixed cutoff for every show.
  if (disneyDeviceFound && !currentShowAlwaysOn &&
      (now - lastShowSyncTime > currentShowDurationMs)) {
    disneyDeviceFound = false;
  }

  // --- Rail Gating: FET1 stays off unless something is actually being shown ---
  // railOn is a global (declared near the top) since runBatterySweepAnimation()
  // also toggles FET1_GATE_PIN directly and must keep this in sync.
  bool wantDisplay = disneyDeviceFound || (manualMode != 0);
#if HAS_BATTERY_ADC
  wantDisplay = wantDisplay || lowBatteryFlashActive;
#endif

  if (wantDisplay && !railOn) {
    digitalWrite(FET1_GATE_PIN, HIGH);
    delay(20); // let the boost rail + SK6812 ICs settle before clocking data
    railOn = true;
  } else if (!wantDisplay && railOn) {
    digitalWrite(FET1_GATE_PIN, LOW);
    railOn = false;
  }

  if (wantDisplay) {
#if HAS_BATTERY_ADC
    if (lowBatteryFlashActive) {
      renderLowBatteryFlash();
    } else
#endif
    if (disneyDeviceFound) {
      executeDisneyMicrocode();
    } else {
      // Manual Animations
      switch (manualMode) {
        case 1: // Rainbow Wave
          {
            uint16_t hue = (now / 10) % 65536;
            for (int i = 0; i < NUM_LEDS; i++) {
              uint32_t color = strip.ColorHSV(hue + (i * 65536 / NUM_LEDS), 255, 120);
              strip.setPixelColor(i, color);
            }
            strip.show();
          }
          break;
        case 2: // Ignition Pulse
          runIgnitionPulse(gHue);
          break;
        case 3: // Meteor Chase
          runMeteorChase();
          break;
        case 4: // Cyber Plasma
          runCyberPlasma();
          break;
        case 5: // Lightning Storm
          runLightningStorm();
          break;
        case 6: // Neon Glitch
          runNeonGlitch();
          break;
        case 7: // HyperDrive
          runHyperDrive(gHue);
          break;
      }
    }
  }
  delay(20);
}
#endif // FET1_TOGGLE_TEST

// --- Animation Routines ---
#if !FET1_TOGGLE_TEST

// RGBW-safe pixel dim for the fade-trail animations below. This strip is
// NEO_GRBW, so strip.getPixelColor() packs all 4 channels into the 32-bit
// return (W in the top byte, same layout strip.Color(r,g,b,w) writes) --
// extracting only R/G/B here and writing back via the 3-arg Color() would
// silently zero any white component every single frame instead of letting
// it decay with the rest of the trail. Needed now that several of these
// animations light the real white diode for flare/spark/flash highlights.
void dimPixel(int i, float factor) {
  uint32_t c = strip.getPixelColor(i);
  uint8_t w = (uint8_t)(((c >> 24) & 0xFF) * factor);
  uint8_t r = (uint8_t)(((c >> 16) & 0xFF) * factor);
  uint8_t g = (uint8_t)(((c >> 8) & 0xFF) * factor);
  uint8_t b = (uint8_t)((c & 0xFF) * factor);
  strip.setPixelColor(i, strip.Color(r, g, b, w));
}

void runIgnitionPulse(uint8_t gHue) {
  uint8_t pulseIntensity = beatsin8(45, 130, 255);
  // White-hot flare: as the pulse nears its peak, blend the flame hue
  // toward the real white diode instead of just cranking RGB brighter --
  // a true white core reads like a hot flame tip in a way saturated
  // orange/red alone can't, and it's cheaper current-wise than faking
  // white via all 3 color LEDs.
  uint8_t whiteAmt = (pulseIntensity > 210)
                          ? (uint8_t)(((uint32_t)(pulseIntensity - 210) * 255) / 45)
                          : 0;
  uint8_t sat = 255 - (whiteAmt / 3); // desaturate slightly as it whitens
  for (int i = 0; i < NUM_LEDS; i++) {
    uint16_t h = (beatsin8(18, 0, 24, 0, i * 6) * 65536) / 255;
    uint32_t c = strip.ColorHSV(h, sat, pulseIntensity);
    c = (c & 0x00FFFFFF) | ((uint32_t)whiteAmt << 24);
    strip.setPixelColor(i, c);
  }
  strip.show();
}

void runMeteorChase() {
  for (int i = 0; i < NUM_LEDS; i++) {
    dimPixel(i, 0.82f);
  }
  uint8_t head = beatsin8(25, 0, NUM_LEDS - 1);
  // White-hot nucleus with a cyan tint, instead of pure RGB cyan -- a
  // comet's head reads brighter/hotter with the real white diode lit
  // underneath the color than R/G/B alone can fake.
  strip.setPixelColor(head, strip.Color(0, 80, 80, 255));
  strip.show();
}

void runCyberPlasma() {
  uint8_t waveA = beatsin8(9, 0, 255);
  uint8_t waveB = beatsin8(14, 0, 255);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t val = (waveA + waveB + (i * 4)) / 2;
    uint16_t h = (val * 65536) / 255;
    strip.setPixelColor(i, strip.ColorHSV(h, 255, 200));
  }
  // Electrical white sparks crackling through the plasma field -- real
  // white diode layered on top of the existing hue (OR'd into the W byte)
  // rather than overwriting it, so the spark reads as "hot" rather than
  // just replacing the plasma color outright.
  if (random(100) < 25) {
    int pos = random(NUM_LEDS);
    uint32_t c = strip.getPixelColor(pos);
    strip.setPixelColor(pos, (c & 0x00FFFFFF) | ((uint32_t)255 << 24));
  }
  strip.show();
}

void runLightningStorm() {
  for (int i = 0; i < NUM_LEDS; i++) {
    dimPixel(i, 0.65f);
  }
  if (random(100) < 12) {
    // Lightning core is white-hot -- real white diode instead of the old
    // (240,248,255) RGB approximation.
    strip.setPixelColor(random(NUM_LEDS), strip.Color(0, 0, 0, 255));
  }
  if (random(100) < 2) {
    for (int i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, strip.Color(0, 191, 255));
    }
  }
  strip.show();
}

void runNeonGlitch() {
  for (int i = 0; i < NUM_LEDS; i++) {
    dimPixel(i, 0.70f);
  }
  if (random(100) < 60) {
    int pos = random(NUM_LEDS);
    uint8_t variant = random(4);
    if (variant == 0)
      strip.setPixelColor(pos, strip.Color(255, 0, 255));
    else if (variant == 1)
      strip.setPixelColor(pos, strip.Color(0, 255, 0));
    else if (variant == 2)
      strip.setPixelColor(pos, strip.Color(64, 224, 208));
    else
      // White noise glitch -- real white diode, closer to "digital
      // static" than any RGB color could read.
      strip.setPixelColor(pos, strip.Color(0, 0, 0, 255));
  }
  strip.show();
}

void runHyperDrive(uint8_t gHue) {
  for (int i = 0; i < NUM_LEDS; i++) {
    dimPixel(i, 0.55f);
  }
  uint8_t leadHead = beatsin8(120, 0, NUM_LEDS - 1);
  uint16_t h1 = ((gHue * 3) % 256) * 65536 / 255;
  uint16_t h2 = (((gHue * 3) + 128) % 256) * 65536 / 255;
  strip.setPixelColor(leadHead, strip.ColorHSV(h1, 255, 255));
  strip.setPixelColor((leadHead + (NUM_LEDS / 2)) % NUM_LEDS, strip.ColorHSV(h2, 255, 255));
  // Occasional warp-flash: real white diode punch on both streak heads,
  // standing in for a hyperspace jump -- brief and rare so it reads as a
  // flash, not a steady-state color.
  if (random(100) < 4) {
    strip.setPixelColor(leadHead, strip.Color(0, 0, 0, 255));
    strip.setPixelColor((leadHead + (NUM_LEDS / 2)) % NUM_LEDS, strip.Color(0, 0, 0, 255));
  }
  strip.show();
}
#endif // !FET1_TOGGLE_TEST

// BLE Advertisement Packet Callback
void scan_callback(ble_gap_evt_adv_report_t *report) {
  uint8_t len = report->data.len;
  uint8_t const *buffer = report->data.p_data;

  uint8_t index = 0;
  while (index < len) {
    uint8_t field_len = buffer[index];
    if (field_len == 0)
      break;
    uint8_t field_type = buffer[index + 1];

    if (field_type == BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA) {
      uint8_t const *mData = &buffer[index + 2];
      uint8_t mDataLen = field_len - 1;
      parseDisneyPacket(mData, mDataLen);
      break;
    }
    index += field_len + 1;
  }

  Bluefruit.Scanner.resume();
}

void parseDisneyPacket(const uint8_t *payload, uint16_t len) {
  if (len < 4)
    return;

  // CC 03 - Wake Ping (PROTOCOL.md): checked ahead of the debounce gate below
  // so a wake ping is never suppressed by an unrelated prior show command's
  // cooldown -- the real payload arrives ~500ms later and needs the boosted
  // scan rate to be active in time to catch it.
  if (payload[0] == 0x83 && payload[1] == 0x01 && payload[2] == 0xCC &&
      payload[3] == 0x03) {
    enterBoostedScan();
    return;
  }

  unsigned long now = millis();
  static unsigned long lastPacketRxTime = 0;
  if (now - lastPacketRxTime < 3500) {
    return;
  }

  // 1. STARLIGHT BUBBLE WAND CAST DETECTION (0xCF 0x0B)
  bool wandAtOffset0 = (payload[0] == 0xCF && payload[1] == 0x0B);
  bool wandAtOffset2 = (payload[2] == 0xCF && payload[3] == 0x0B);
  if (wandAtOffset0 || wandAtOffset2) {
    lastPacketRxTime = now;
    receivedCommandType = 0xC1;
    // Color byte sits at local offset 12 of the 13-byte CF 0B signature
    // (PROTOCOL.md) -- but that's relative to wherever CF actually starts,
    // which shifts by 2 depending on which branch matched above. This was
    // previously hardcoded to payload[12] unconditionally, which is only
    // correct for the offset-0 case; our own transmitter's real broadcast
    // is CID-prefixed (matches offset-2), so the real color byte is at
    // payload[14] -- reading payload[12] instead landed 2 bytes into the
    // all-zero rolling-code section, silently ignoring whatever color was
    // actually set regardless of the transmitter's own color adjustment.
    int colorOffset = wandAtOffset2 ? 14 : 12;
    uint8_t wandColIdx = (len > (unsigned)colorOffset) ? (payload[colorOffset] & 0x1F) : 0;
    CRGB wandCol = DISNEY_PALETTE[wandColIdx];
    showR1 = wandCol.r;
    showG1 = wandCol.g;
    showB1 = wandCol.b;
    isDualColor = false;
    lastShowSyncTime = now;
    disneyDeviceFound = true;
    // No Timing Byte on this command type -- fixed default duration, set
    // explicitly so it doesn't inherit a stale value from a prior E9 show.
    currentShowDurationMs = 4000;
    currentShowAlwaysOn = false;
    Serial.printf(
        "\n✨ [BLE RX] STARLIGHT BUBBLE WAND CAST! Palette Index: %d (%s)\n",
        wandColIdx, DISNEY_PALETTE_NAMES[wandColIdx]);
    return;
  }

  // 2. FAB 50 STATUE BEACON DETECTION (0xC4)
  if ((payload[0] == 0xC4 && (payload[1] == 0x10 || payload[1] == 0x15)) ||
      (payload[2] == 0xC4 && (payload[3] == 0x10 || payload[3] == 0x15))) {
    lastPacketRxTime = now;
    receivedCommandType = 0xC4;
    showR1 = 255;
    showG1 = 215;
    showB1 = 0; // Gold
    isDualColor = false;
    lastShowSyncTime = now;
    disneyDeviceFound = true;
    // No Timing Byte on this command type either -- same fixed default.
    currentShowDurationMs = 4000;
    currentShowAlwaysOn = false;
    Serial.println("\n🗿 [BLE RX] FAB 50 STATUE BEACON DETECTED! (Gold)");
    return;
  }

  // Validate Disney Company ID (0x0183 or 0x8301)
  uint16_t companyId = (payload[1] << 8) | payload[0];
  if (companyId != DISNEY_COMPANY_ID_LE && companyId != DISNEY_COMPANY_ID_BE)
    return;

  // Family 3 (PROTOCOL.md sec. 6): Direct Park Infrastructure opcodes,
  // broadcast as [0x83][0x01][0xE9|0xEA][Opcode]... with no [E1|E2][0x00]
  // wearable wrapper -- so the show header sits 2 bytes earlier than the
  // Family 1 wearable layout checked below, and can also be 0xEA (a header
  // value Family 1 never uses). E9 10 is decoded below (see PROTOCOL.md
  // sec. 6.1); everything else (E9 04, E9 13, EA 14, E9 08 short-form,
  // etc.) still isn't, and falls back to a hash-derived color.
  if (len >= 4 && (payload[2] == 0xE9 || payload[2] == 0xEA)) {
    lastPacketRxTime = now;
    uint8_t opcode = payload[3];
    Serial.printf("\n🏗️  [BLE RX] FAMILY 3 (Park Infrastructure) packet! "
                  "Header=0x%02X Opcode=0x%02X len=%d\n",
                  payload[2], opcode, len);

    // E9 10 ("Alternating Colors" per community naming) is the one Family 3
    // opcode bench-confirmed end-to-end against a real MagicBand+ (see
    // PROTOCOL.md sec. 6.1): 5 independently-addressable LED slots at
    // payload[7..11] (Center/NE/SE/SW/NW on the real band), each byte
    // packing (mode<<5)|paletteIndex -- same two-field split PROTOCOL.md
    // documents for E9 05's LED-position mask, just carrying a different
    // meaning in the top 3 bits here. Timing Byte sits at payload[5] (2
    // bytes earlier than the Family 1 offset since there's no E1/E2 00
    // wrapper). payload[12] is an "extra animation ID" byte -- only 0x30
    // and 0x31 are confirmed to trigger a 5-zone rotating chase (direction
    // flips on the low bit); everything else, including the real captured
    // default 0x82, renders as a static per-slot fill of the real decoded
    // colors.
    // The 5-slot/PatternID model above is only bench-confirmed when byte 7's
    // top3 (mode) is 5, 6, or 7 -- that's what every rotation/static test
    // this session actually varied. Top3 0-4 (which includes mode 2, the
    // real captured default's own mode) showed real, reproducible motion
    // that this model doesn't explain: isolating individual bytes never
    // reproduced it. Watching the real band directly (not just a photo)
    // showed only Center's own color (Red Orange) moving -- no Cyan/Pink
    // from the other slots ever appeared -- so this isn't the same 5-color
    // chase as mode 5-7; it reads as a single dot of just Center's color
    // in motion. Rendered as that: NE/SE/SW/NW's bytes are decoded (in
    // case a future session figures out what they're for) but not used by
    // this specific render path.
    // E9 13 (unwrapped) confirmed to share this exact same byte layout and
    // Timing Byte formula -- see PROTOCOL.md sec. 6.1's E9 13 addendum --
    // so it reuses this same decode/render path rather than duplicating it.
    if ((opcode == 0x10 || opcode == 0x13) && len >= 13) {
      applyShowTiming(payload[5]);
      for (int s = 0; s < 5; s++) {
        uint8_t idx = payload[7 + s] & 0x1F;
        CRGB col = DISNEY_PALETTE[idx];
        showColorsPark5[s][0] = col.r;
        showColorsPark5[s][1] = col.g;
        showColorsPark5[s][2] = col.b;
      }
      uint8_t centerTop3 = (payload[7] >> 5) & 0x07;
      bool isRainbowFamily = (centerTop3 >= 5 && centerTop3 <= 7);
      parkPatternId = payload[12];
      if (isRainbowFamily) {
        // Bench-confirmed: only 0x30/0x31 spin here, direction from the low bit.
        parkSpin = (parkPatternId == 0x30 || parkPatternId == 0x31);
        parkSpinReverse = (parkPatternId & 0x01) != 0;
        receivedCommandType = 0x10;
        Serial.printf("   🎡 [E9 %02X] top3=%d pattern=0x%02X spin=%s timing=%lums\n",
                      opcode, centerTop3, parkPatternId, parkSpin ? "true" : "false",
                      currentShowDurationMs);
      } else {
        // Top3 0-4: single moving dot of just Center's real color.
        showR1 = showColorsPark5[0][0];
        showG1 = showColorsPark5[0][1];
        showB1 = showColorsPark5[0][2];
        receivedCommandType = 0xFE; // single-dot chase renderer
        Serial.printf("   🎡 [E9 %02X] top3=%d single-color chase, color=%s "
                      "timing=%lums\n",
                      opcode, centerTop3, DISNEY_PALETTE_NAMES[payload[7] & 0x1F],
                      currentShowDurationMs);
      }
      lastShowSyncTime = now;
      disneyDeviceFound = true;
    } else {
      // Not yet decoded (E9 04, EA 14, E9 08 short-form, etc.) -- render a
      // deterministic hash-derived color instead of doing nothing,
      // same philosophy as Adafruit's own reference renderer for these
      // exact opcodes (see research/BLE_Beacon_Ears -- their
      // _show_command_generic_path()): different captures still look
      // different from each other on the strip, even though we can't yet
      // decode their internal structure.
      uint16_t seed = 0;
      for (uint16_t i = 0; i < len; i++) {
        seed = (seed * 31 + payload[i]) & 0xFFFF;
      }
      uint8_t idx = seed % 32;
      if (idx == 29) idx = (idx + 1) % 32; // never land on "Off" (black)
      CRGB col = DISNEY_PALETTE[idx];
      showR1 = col.r;
      showG1 = col.g;
      showB1 = col.b;
      isDualColor = false;
      currentShowDurationMs = 4000;
      currentShowAlwaysOn = false;
      receivedCommandType = 0xFF; // falls through to the generic hold-display renderer
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.printf("   🌀 [Family 3 fallback] opcode=0x%02X hue=%s\n", opcode,
                    DISNEY_PALETTE_NAMES[idx]);
    }
    return;
  }

  // Check for 0xE9 Show Command Header
  if (len >= 6 && payload[4] == DISNEY_HEADER_SHOW) {
    lastPacketRxTime = now;
    uint8_t showCmd = payload[5];
    receivedCommandType = showCmd;
    const DisneyShowCommand *showInfo = getDisneyShowInfo(showCmd);

    // Timing Byte sits at the same offset (payload[7], right after the 00
    // spacer at payload[6]) across every E9 show command per PROTOCOL.md --
    // decode it once here rather than per-command, so hold time actually
    // matches what the packet specifies instead of one fixed cutoff for
    // every show.
    if (len >= 8) {
      applyShowTiming(payload[7]);
      Serial.printf("⏱️  [TIMING] byte=0x%02X -> %lums, alwaysOn=%s\n",
                    payload[7], currentShowDurationMs,
                    currentShowAlwaysOn ? "true" : "false");
    }

    Serial.printf("\n[%lu ms] ✨ [BLE RX] DISNEY SHOW PACKET! Cmd: 0x%02X",
                  millis(), showCmd);
    if (showInfo) {
      Serial.printf(" (%s - %s)", showInfo->name, showInfo->location);
    }
    Serial.println();

    // 0x05: Single Color Palette Hold
    if (showCmd == 0x05 && len >= 10) {
      uint8_t colorIdx = payload[9] & 0x1F;
      CRGB col = DISNEY_PALETTE[colorIdx];
      showR1 = col.r;
      showG1 = col.g;
      showB1 = col.b;
      isDualColor = false;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.printf("   🎨 Single Color Index: %d (%s)\n", colorIdx,
                    DISNEY_PALETTE_NAMES[colorIdx]);
#if HAS_HAPTIC
      triggerHapticFromShowByte(payload[len - 1]);
#endif
    }
    // 0x06: Dual Split Palette
    else if (showCmd == 0x06 && len >= 11) {
      uint8_t color1Idx = payload[9] & 0x1F;
      uint8_t color2Idx = payload[10] & 0x1F;
      CRGB col1 = DISNEY_PALETTE[color1Idx];
      CRGB col2 = DISNEY_PALETTE[color2Idx];
      showR1 = col1.r;
      showG1 = col1.g;
      showB1 = col1.b;
      showR2 = col2.r;
      showG2 = col2.g;
      showB2 = col2.b;
      isDualColor = true;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.printf("   🎨 Dual Colors: %s / %s\n",
                    DISNEY_PALETTE_NAMES[color1Idx],
                    DISNEY_PALETTE_NAMES[color2Idx]);
#if HAS_HAPTIC
      triggerHapticFromShowByte(payload[len - 1]);
#endif
    }
    // 0x08: Raw RGB Color Command
    // Payload has a 2-byte D2 55 sub-header before the color bytes (see
    // PROTOCOL.md / emcot.txt raw example 8301E100E908000ED2557C7C7CB0),
    // which our old offsets (7,8,9) didn't account for -- they were reading
    // the timing byte and the D2/55 signature bytes as if they were R/G/B.
    // Real color bytes are at 10/11/12. Also corrected the 6-bit unpack:
    // encoding is Byte = (Channel6bit & 0x3F) << 1, so the 8-bit value is
    // (Byte & 0x7E) << 1, not (Byte & 0x3F) << 2 (off by one bit position).
    else if (showCmd == 0x08 && len >= 13) {
      showR1 = (payload[10] & 0x7E) << 1;
      showG1 = (payload[11] & 0x7E) << 1;
      showB1 = (payload[12] & 0x7E) << 1;
      isDualColor = false;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.printf("   🎨 Raw RGB: (%d, %d, %d)\n", showR1, showG1, showB1);
#if HAS_HAPTIC
      triggerHapticFromShowByte(payload[len - 1]);
#endif
    }
    // 0x0B: High-Contrast Circle (Mode 4 on Transmitter)
    else if (showCmd == 0x0B) {
      // Previous "Electric Blue outer, White inner" behavior was never
      // bench-tested -- PROTOCOL.md's documented example for this command
      // has different (also untested) tail bytes than what our own
      // transmitter actually sends (which matches the real captured
      // example in research/emcot.txt exactly). Confirmed live against a
      // real MagicBand+: a single white LED chasing around the ring, dark
      // otherwise -- not a static two-zone fill at all.
      receivedCommandType = 0xFB; // single white dot chase, dark otherwise
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.println("   🎨 [Mode 4] High-Contrast Circle: white dot chase!");
    }
    // 0x09: 5-Color Palette Ring (Mode 7 on Transmitter)
    // Same 2-byte sub-header issue as 0x08 (see note above): the 5 palette
    // slots start at offset 9, not 7 -- the old offset read the timing byte
    // and 0x0F signature as the first two "colors" and missed the last two
    // slots entirely. Order is Center, NE, SE, SW, NW -- bench-confirmed
    // against a real MagicBand+ (see PROTOCOL.md's E9 09 section), which
    // corrected the community wiki's unverified "TL, BL, BR, TR, Center"
    // labeling. Not that it matters functionally here, since these 5 bytes
    // just fill 5 sequential zones of the linear strip with no attempt at
    // real spatial correspondence (see CLAUDE.md's Fiber Optic Subsystem
    // section for the planned future upgrade to real quadrant positions).
    else if (showCmd == 0x09 && len >= 14) {
      for (int s = 0; s < 5; s++) {
        uint8_t cIdx = payload[9 + s] & 0x1F;
        CRGB col = DISNEY_PALETTE[cIdx];
        showColors5[s][0] = col.r;
        showColors5[s][1] = col.g;
        showColors5[s][2] = col.b;
      }
      showColors5Count = 5;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.println("   🎨 [Mode 7] 5-Color Palette Ring!");
#if HAS_HAPTIC
      triggerHapticFromShowByte(payload[len - 1]);
#endif
    }
    // 0x0E: PROTOCOL.md's documented "Strobe Pulse" 2-color layout turned
    // out to be wrong -- 5 real captured examples all decode cleanly as
    // the same 5-slot Center/NE/SE/SW/NW palette used by E9 09/10/0C
    // (offsets 9-13 here), matching their reported visuals well (e.g.
    // Center=Off + ring=Cyan -> "outer ring flashes blue"). Rendered with
    // the same discrete color-wheel rotation adapted from the Adafruit
    // reference renderer's generic "animation" handler (research/
    // BLE_Beacon_Ears, _state_animation()) -- same mechanism as E9 0C's
    // "Taste the Rainbow" fix, fed real decoded colors instead of a
    // hardcoded rainbow list.
    else if (showCmd == 0x0E && len >= 14) {
      for (int s = 0; s < 5; s++) {
        uint8_t idx = payload[9 + s] & 0x1F;
        CRGB col = DISNEY_PALETTE[idx];
        showColors5[s][0] = col.r;
        showColors5[s][1] = col.g;
        showColors5[s][2] = col.b;
      }
      showColors5Count = 5;
      receivedCommandType = 0xFA; // 5-slot color-wheel rotation
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.println("   🎨 [Mode 8] E9 0E: 5-color wheel rotation");
#if HAS_HAPTIC
      triggerHapticFromShowByte(payload[len - 1]);
#endif
    }
    // 0x0C: "Animation Codes" -- per emcot.txt/research/BLE_Beacon_Ears,
    // several known real captures of this opcode ("Taste the Rainbow",
    // "Blink White", "Orange Blink") are firmware-baked animation programs
    // where the bytes are opaque program IDs, NOT real palette data --
    // decoding "Taste the Rainbow"'s actual bytes via the normal
    // (mode<<5)|color formula gives a scrambled non-rainbow sequence
    // (confirmed: Off/Lavender/White/Gold/Purple/Lime/Cyan/Pink), which is
    // why treating 0x0C uniformly as a white strobe (the old behavior
    // here) looked wrong on real hardware. Detect known signatures and
    // route to the right specific render instead; anything else falls
    // back to a genuine multi-slot palette decode + rotation -- same
    // philosophy as the Adafruit reference renderer's own two-tier
    // approach for this exact opcode.
    else if (showCmd == 0x0C) {
      // Rainbow and Blink White share the exact same 12-byte prefix in
      // their real captures -- Adafruit's own reference renderer
      // disambiguates them by the payload's last byte (0x95 = Blink White,
      // anything else including the real 0xB0 = Rainbow); same approach
      // here.
      static const uint8_t RAINBOW_SIG[12] = {0xE1, 0x00, 0xE9, 0x0C, 0x00, 0x0F,
                                               0x0F, 0x5D, 0x46, 0x5B, 0xF0, 0x05};
      static const uint8_t ORANGE_BLINK_SIG[12] = {0xE1, 0x00, 0xE9, 0x0C, 0x00, 0xEF,
                                                    0x0F, 0x4F, 0x4F, 0x5B, 0xF0, 0xFB};
      bool matchesRainbowPrefix = (len >= 14) && (memcmp(payload + 2, RAINBOW_SIG, 12) == 0);
      bool matchesOrangeBlink = (len >= 14) && (memcmp(payload + 2, ORANGE_BLINK_SIG, 12) == 0);
      if (matchesRainbowPrefix && len >= 1 && payload[len - 1] == 0x95) {
        showR1 = 255; showG1 = 255; showB1 = 255; // White
        isDualColor = false;
        // Confirmed live: the real band's Blink White is a much slower
        // ~0.5s on/off blink, not the fast strobe genuine E9 0E uses.
        showBlinkIntervalMs = 500;
        receivedCommandType = 0x0E; // reuse the existing strobe renderer
        Serial.println("   ⚡ [E9 0C] Blink White (known animation signature)");
      } else if (matchesRainbowPrefix) {
        receivedCommandType = 0xFC; // known baked "Taste the Rainbow" animation
        Serial.println("   🌈 [E9 0C] Taste the Rainbow (known animation signature)");
      } else if (matchesOrangeBlink) {
        showR1 = 255; showG1 = 90; showB1 = 0; // Orange
        isDualColor = false;
        showBlinkIntervalMs = 500;
        // The real captured Timing Byte (0xEF) has ALWAYS_ON set -- on the
        // real band this is an alert-style animation meant to hold
        // indefinitely, but that's not useful for a wearable prop, so
        // override it with a fixed shutoff instead of respecting the
        // packet's own always-on flag.
        currentShowAlwaysOn = false;
        currentShowDurationMs = 5000;
        receivedCommandType = 0x0E; // reuse the existing strobe renderer
        Serial.println("   ⚡ [E9 0C] Orange Blink (known animation signature, capped at 5s)");
      } else {
        // Generic fallback: decode whatever slot bytes are present (up to
        // 5, matching showColors5's capacity) as real palette data and
        // rotate through them -- reasonable for the "5 Palette Color
        // Cycle"-style captures, an approximation for anything else.
        // Clamped to >=0: showCmd==0x0C is reachable with len as low as 6
        // (the outer gate only requires len>=6, and this generic branch has
        // no length check of its own since the signature checks above it
        // already handle the len<14 case implicitly by just not matching).
        // A short packet made (int)len-9 negative, and assigning that to
        // showColors5Count (a uint8_t) wrapped it to a huge value (e.g. -3
        // -> 253) -- the render path then read showColors5[srcSlot] with
        // srcSlot up to 252 against a real 5-slot array, an out-of-bounds
        // read into whatever memory happens to follow it.
        int slotCount = max(0, min(5, (int)len - 9));
        for (int s = 0; s < slotCount; s++) {
          uint8_t idx = payload[9 + s] & 0x1F;
          CRGB col = DISNEY_PALETTE[idx];
          showColors5[s][0] = col.r;
          showColors5[s][1] = col.g;
          showColors5[s][2] = col.b;
        }
        showColors5Count = slotCount;
        receivedCommandType = 0x0C;
        Serial.printf("   🎨 [E9 0C] Generic %d-slot palette cycle\n", slotCount);
      }
      lastShowSyncTime = now;
      disneyDeviceFound = true;
    }
    // 0x12: Wave Pulse (Mode 6 on Transmitter)
    else if (showCmd == 0x12) {
      // Transmitter's broadcastWavePulse() sends color1 duplicated at
      // data[7]/[8] (0xA0|c1) and color2 duplicated at data[9]/[10]
      // (0xA0|c2) -- landing at payload[9]/[10] and payload[11]/[12] once
      // the 2-byte company-ID prefix (payload[0..1]) is accounted for,
      // same +2 offset convention as every other showCmd branch (0x05,
      // 0x06, 0x14). This previously read payload[7] (actually the timing
      // byte) for c1 and payload[9] (actually color1's first copy) for c2,
      // so the fill color was timing-byte garbage and color2 was never
      // used at all -- confirmed live: primary showed as an unrelated
      // color and secondary never appeared.
      uint8_t c1 = (len >= 10) ? (payload[9] & 0x1F) : 0x01;  // Color 1 (Purple)
      uint8_t c2 = (len >= 12) ? (payload[11] & 0x1F) : 0x15; // Color 2 (Red)
      CRGB col1 = DISNEY_PALETTE[c1];
      CRGB col2 = DISNEY_PALETTE[c2];
      showR1 = col1.r; showG1 = col1.g; showB1 = col1.b;
      showR2 = col2.r; showG2 = col2.g; showB2 = col2.b;
      isDualColor = true;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.printf("   🎨 [Mode 6] Wave Pulse! Colors: %s / %s\n",
                    DISNEY_PALETTE_NAMES[c1], DISNEY_PALETTE_NAMES[c2]);
#if HAS_HAPTIC
      triggerHapticFromShowByte(payload[len - 1]);
#endif
    }
    // 0x11: Park Cross-Fade (e.g. Cyan to Pink) -- per emcot.txt "2 Palette
    // Colors follow the 0F" marker byte at payload[8]; color1/color2 land
    // at payload[9]/[10], same &0x1F palette-index convention as everywhere
    // else. Actual blending happens in executeDisneyMicrocode() each frame,
    // driven by elapsed time vs. currentShowDurationMs -- not a blocking
    // delay()-loop, so it doesn't stall the button/BLE/scan-boost logic.
    else if (showCmd == 0x11 && len >= 11) {
      uint8_t c1 = payload[9] & 0x1F;
      uint8_t c2 = payload[10] & 0x1F;
      CRGB col1 = DISNEY_PALETTE[c1];
      CRGB col2 = DISNEY_PALETTE[c2];
      showR1 = col1.r; showG1 = col1.g; showB1 = col1.b;
      showR2 = col2.r; showG2 = col2.g; showB2 = col2.b;
      isDualColor = false;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.printf("   🎨 [Cross-Fade] %s -> %s over %lums\n",
                    DISNEY_PALETTE_NAMES[c1], DISNEY_PALETTE_NAMES[c2],
                    currentShowDurationMs);
#if HAS_HAPTIC
      triggerHapticFromShowByte(payload[len - 1]);
#endif
    }
    // 0x14: same Center/NE two-color model as the unwrapped E9 10/13
    // "background+chaser" render (see PROTOCOL.md sec. 6), same byte
    // offsets as every other wrapped command (Timing at payload[7],
    // colors starting at payload[9]) -- but bench-confirmed to animate
    // much faster and less predictably than E9 10/13's steady sweeping
    // chaser, closer to a fast random flicker between the two colors than
    // a moving gap. Rendered as that instead of forcing it through the
    // same deterministic-sweep renderer, which looked visibly wrong for
    // this command.
    else if (showCmd == 0x14 && len >= 11) {
      uint8_t c1 = payload[9] & 0x1F;
      uint8_t c2 = payload[10] & 0x1F;
      CRGB col1 = DISNEY_PALETTE[c1];
      CRGB col2 = DISNEY_PALETTE[c2];
      showR1 = col1.r; showG1 = col1.g; showB1 = col1.b;
      showR2 = col2.r; showG2 = col2.g; showB2 = col2.b;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.printf("   🎨 [E9 14] Flicker: %s / %s over %lums\n",
                    DISNEY_PALETTE_NAMES[c1], DISNEY_PALETTE_NAMES[c2],
                    currentShowDurationMs);
#if HAS_HAPTIC
      triggerHapticFromShowByte(payload[len - 1]);
#endif
    }
    // Fallback: Any other valid show command
    else {
      uint8_t c1 = (len >= 10) ? (payload[9] & 0x1F) : 0;
      CRGB col = DISNEY_PALETTE[c1];
      showR1 = col.r; showG1 = col.g; showB1 = col.b;
      isDualColor = false;
      // Explicitly reset to the generic hold-display renderer rather than
      // leaving receivedCommandType at the raw showCmd value: a wrapped
      // packet whose showCmd happens to literally be 0x10 or 0x13 (both
      // real, confirmed opcodes -- just ones this wrapped-format dispatch
      // doesn't have its own handler for) would otherwise fall through to
      // the Family 3 E9 10/13 render branches instead, which expect
      // showColorsPark5 to be populated and would render stale/wrong data.
      receivedCommandType = 0xFF;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.printf("   🎨 Generic Show Command 0x%02X Activated!\n", showCmd);
#if HAS_HAPTIC
      triggerHapticFromShowByte(payload[len - 1]);
#endif
    }
  } else if (len >= 4 && payload[2] == 0xCD && payload[3] == 0x07) {
    // CD 07: a "parade/show command not in our protocol docs" per
    // research/BLE_Beacon_Ears's own comment (its magicband_protocol.py
    // has the same func-code entry but also can't decode it). Narrowly
    // scoped to this exact header -- an earlier version of this fallback
    // caught *any* non-E9 packet and fired constantly on ordinary
    // background traffic from a real band just being nearby (e.g. idle/
    // presence broadcasts), which isn't what we want; CD 07 specifically
    // is framed as an actual show event, not routine idle chatter. Same
    // hash-derived-color fallback as the Family 3 undecoded-opcode case.
    lastPacketRxTime = now; // missing from an earlier version of this fix -- without it, this path bypassed the debounce other branches get
    uint16_t seed = 0;
    for (uint16_t i = 0; i < len; i++) {
      seed = (seed * 31 + payload[i]) & 0xFFFF;
    }
    uint8_t idx = seed % 32;
    if (idx == 29) idx = (idx + 1) % 32; // never land on "Off" (black)
    CRGB col = DISNEY_PALETTE[idx];
    showR1 = col.r;
    showG1 = col.g;
    showB1 = col.b;
    isDualColor = false;
    currentShowDurationMs = 4000;
    currentShowAlwaysOn = false;
    receivedCommandType = 0xFF; // generic hold-display renderer
    lastShowSyncTime = now;
    disneyDeviceFound = true;
    Serial.printf("   🌀 [CD 07 Parade Command] hue=%s\n", DISNEY_PALETTE_NAMES[idx]);
  }
}