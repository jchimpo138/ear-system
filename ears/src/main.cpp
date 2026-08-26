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

// --- Haptic Feedback Helper ---
#if HAS_HAPTIC
// Motor is on the raw battery rail (~3.0-4.2V), rated for 3.0V/100mA. 80%
// PWM duty brings the sustained average down near its rated point (e.g.
// ~3.0V average at a 3.75V resting battery) instead of running it at full
// rail voltage continuously. A brief full-power kick-start is still used
// first, since the motor's ~2.3V start-voltage spec is too close to what
// 80% duty alone would deliver at a low, near-depleted battery.
#define HAPTIC_DUTY_PERCENT 80
#define HAPTIC_KICKSTART_MS 40

void hapticPulse(unsigned long durationMs) {
  uint8_t sustainDuty = (uint8_t)((HAPTIC_DUTY_PERCENT * 255) / 100);
  unsigned long kick = min(durationMs, (unsigned long)HAPTIC_KICKSTART_MS);

  analogWrite(HAPTIC_PIN, 255); // full-power kick to overcome static friction
  delay(kick);
  if (durationMs > kick) {
    analogWrite(HAPTIC_PIN, sustainDuty);
    delay(durationMs - kick);
  }
  analogWrite(HAPTIC_PIN, 0);
}

void triggerHapticPattern(uint8_t patternCode) {
  Serial.printf("📳 [HAPTIC] Triggering pattern: 0x%02X on D4 (P0.04)\n", patternCode);
  switch (patternCode) {
    case 0xFF: // 1-Second Full Power Test Pulse
      hapticPulse(1000);
      break;
    case 0x01: // Single Tap (250ms)
      hapticPulse(250);
      break;
    case 0x02: // Double Tap (250ms / 150ms / 250ms)
      hapticPulse(250);
      delay(150);
      hapticPulse(250);
      break;
    case 0x07: // Heavy Rumble (600ms)
      hapticPulse(600);
      break;
    case 0x0A: // Sharp Pulse
      hapticPulse(300);
      delay(100);
      hapticPulse(300);
      break;
    case 0x0B: // Standard Pulse
    default:
      hapticPulse(300);
      break;
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
void applyShowTiming(uint8_t timingByte) {
  currentShowAlwaysOn = (timingByte & 0x80) != 0;
  bool scaler = (timingByte & 0x40) != 0;
  uint8_t timeVal = timingByte & 0x0F;
  float durationSec = scaler ? (3.1f * timeVal + 5.5f) : (1.5f * timeVal + 6.5f);
  currentShowDurationMs = (unsigned long)(durationSec * 1000.0f);
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

#if !FET1_TOGGLE_TEST
void executeDisneyMicrocode() {
  if (receivedCommandType == 0xC1) {
    // ✨ STARLIGHT BUBBLE WAND: 4 seconds of magical sparkle
    for (int frame = 0; frame < 40; frame++) {
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, strip.Color(showR1, showG1, showB1));
      }
      strip.setPixelColor(random(NUM_LEDS), strip.Color(255, 255, 255));
      strip.setPixelColor(random(NUM_LEDS), strip.Color(255, 255, 255));
      strip.show();
      delay(80);
    }
  } else if (receivedCommandType == 0xC4) {
    // 🗿 FAB 50 STATUE: 5 seconds of golden magical swirl
    for (int swirl = 0; swirl < 80; swirl++) {
      for (int i = 0; i < NUM_LEDS; i++) {
        uint32_t c = strip.getPixelColor(i);
        uint8_t r = ((c >> 16) & 0xFF) * 0.75;
        uint8_t g = ((c >> 8) & 0xFF) * 0.75;
        uint8_t b = (c & 0xFF) * 0.75;
        strip.setPixelColor(i, strip.Color(r, g, b));
      }
      int p1 = swirl % NUM_LEDS;
      int p2 = (swirl + (NUM_LEDS / 2)) % NUM_LEDS;
      strip.setPixelColor(p1, strip.Color(255, 215, 0)); // Gold
      strip.setPixelColor(p2, strip.Color(255, 255, 255)); // White
      strip.show();
      delay(60);
    }
  } else if (receivedCommandType == 0x09) {
    // 🎨 5-COLOR PALETTE RING: 5 distinct color segments
    int segSize = NUM_LEDS / 5;
    for (int s = 0; s < 5; s++) {
      uint32_t c = strip.Color(showColors5[s][0], showColors5[s][1], showColors5[s][2]);
      int startIdx = s * segSize;
      int count = (s == 4) ? (NUM_LEDS - startIdx) : segSize;
      for (int i = startIdx; i < startIdx + count; i++) {
        strip.setPixelColor(i, c);
      }
    }
    strip.show();
  } else if (receivedCommandType == 0x0E || receivedCommandType == 0x0C) {
    // ⚡ STROBE PULSE: 4 seconds of rapid alternating strobe
    for (int f = 0; f < 35; f++) {
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, strip.Color(255, 255, 255)); // White Strobe
      }
      strip.show();
      delay(50);
      strip.clear();
      strip.show();
      delay(50);
    }
  } else if (receivedCommandType == 0x12) {
    // 🌊 WAVE PULSE: MagicBand+ replica spinning chaser and center beat
    int half = NUM_LEDS / 2;
    unsigned long animStart = millis();
    int step = 0;
    while (millis() - animStart < 6000) {
      int spinPos = step % half;
      if (step % 10 == 0) {
        strip.clear();
        for (int e = 0; e < 2; e++) {
          int offset = e * half;
          for (int c = 4; c < 10; c++) {
            strip.setPixelColor(offset + c, strip.Color(showR2, showG2, showB2));
          }
        }
        strip.show();
        delay(90);
      } else {
        for (int i = 0; i < NUM_LEDS; i++) {
          strip.setPixelColor(i, strip.Color(showR1, showG1, showB1));
        }
        strip.setPixelColor(spinPos, strip.Color(showR2, showG2, showB2));
        strip.setPixelColor((spinPos + 1) % half, strip.Color(showR2, showG2, showB2));
        strip.setPixelColor(half + spinPos, strip.Color(showR2, showG2, showB2));
        strip.setPixelColor(half + ((spinPos + 1) % half), strip.Color(showR2, showG2, showB2));
        strip.show();
        delay(35);
      }
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
      strip.setPixelColor(i, strip.Color(rA, gA, bA));
    }
    for (int i = half; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, strip.Color(rB, gB, bB));
    }
    strip.show();
  } else {
    // Standard Hold Display (0x05, 0x06, 0x08, 0x0B)
    if (isDualColor) {
      int half = NUM_LEDS / 2;
      for (int i = 0; i < half; i++) {
        strip.setPixelColor(i, strip.Color(showR1, showG1, showB1));
      }
      for (int i = half; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, strip.Color(showR2, showG2, showB2));
      }
    } else {
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, strip.Color(showR1, showG1, showB1));
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

  // 📳 Startup Haptic Test: 1-Second Full-Power Pulse
  triggerHapticPattern(0xFF);
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

  if (wantDisplay && !railOn) {
    digitalWrite(FET1_GATE_PIN, HIGH);
    delay(20); // let the boost rail + SK6812 ICs settle before clocking data
    railOn = true;
  } else if (!wantDisplay && railOn) {
    digitalWrite(FET1_GATE_PIN, LOW);
    railOn = false;
  }

  if (wantDisplay) {
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
void runIgnitionPulse(uint8_t gHue) {
  uint8_t pulseIntensity = beatsin8(45, 130, 255);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint16_t h = (beatsin8(18, 0, 24, 0, i * 6) * 65536) / 255;
    strip.setPixelColor(i, strip.ColorHSV(h, 255, pulseIntensity));
  }
  strip.show();
}

void runMeteorChase() {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t c = strip.getPixelColor(i);
    uint8_t r = ((c >> 16) & 0xFF) * 0.82;
    uint8_t g = ((c >> 8) & 0xFF) * 0.82;
    uint8_t b = (c & 0xFF) * 0.82;
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  uint8_t head = beatsin8(25, 0, NUM_LEDS - 1);
  strip.setPixelColor(head, strip.Color(0, 255, 255));
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
  strip.show();
}

void runLightningStorm() {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t c = strip.getPixelColor(i);
    uint8_t r = ((c >> 16) & 0xFF) * 0.65;
    uint8_t g = ((c >> 8) & 0xFF) * 0.65;
    uint8_t b = (c & 0xFF) * 0.65;
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  if (random(100) < 12) {
    strip.setPixelColor(random(NUM_LEDS), strip.Color(240, 248, 255));
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
    uint32_t c = strip.getPixelColor(i);
    uint8_t r = ((c >> 16) & 0xFF) * 0.70;
    uint8_t g = ((c >> 8) & 0xFF) * 0.70;
    uint8_t b = (c & 0xFF) * 0.70;
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  if (random(100) < 60) {
    int pos = random(NUM_LEDS);
    uint8_t variant = random(3);
    if (variant == 0)
      strip.setPixelColor(pos, strip.Color(255, 0, 255));
    else if (variant == 1)
      strip.setPixelColor(pos, strip.Color(0, 255, 0));
    else
      strip.setPixelColor(pos, strip.Color(64, 224, 208));
  }
  strip.show();
}

void runHyperDrive(uint8_t gHue) {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t c = strip.getPixelColor(i);
    uint8_t r = ((c >> 16) & 0xFF) * 0.55;
    uint8_t g = ((c >> 8) & 0xFF) * 0.55;
    uint8_t b = (c & 0xFF) * 0.55;
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  uint8_t leadHead = beatsin8(120, 0, NUM_LEDS - 1);
  uint16_t h1 = ((gHue * 3) % 256) * 65536 / 255;
  uint16_t h2 = (((gHue * 3) + 128) % 256) * 65536 / 255;
  strip.setPixelColor(leadHead, strip.ColorHSV(h1, 255, 255));
  strip.setPixelColor((leadHead + (NUM_LEDS / 2)) % NUM_LEDS, strip.ColorHSV(h2, 255, 255));
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
  if ((payload[0] == 0xCF && payload[1] == 0x0B) ||
      (payload[2] == 0xCF && payload[3] == 0x0B)) {
    lastPacketRxTime = now;
    receivedCommandType = 0xC1;
    uint8_t wandColIdx = (len >= 13) ? (payload[12] & 0x1F) : 0;
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
    }
    // 0x0B: High-Contrast Circle (Mode 4 on Transmitter)
    else if (showCmd == 0x0B) {
      showR1 = 0;   showG1 = 128; showB1 = 255; // Electric Blue outer
      showR2 = 255; showG2 = 255; showB2 = 255; // White inner
      isDualColor = true;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.println("   🎨 [Mode 4] High-Contrast Electric Blue & White Circle!");
    }
    // 0x09: 5-Color Palette Ring (Mode 7 on Transmitter)
    // Same 2-byte sub-header issue as 0x08 (see note above): the 5 palette
    // slots (TL,BL,BR,TR,C) start at offset 9, not 7 -- the old offset read
    // the timing byte and 0x0F signature as the first two "colors" and
    // missed the last two slots (TR, center) entirely.
    else if (showCmd == 0x09 && len >= 14) {
      for (int s = 0; s < 5; s++) {
        uint8_t cIdx = payload[9 + s] & 0x1F;
        CRGB col = DISNEY_PALETTE[cIdx];
        showColors5[s][0] = col.r;
        showColors5[s][1] = col.g;
        showColors5[s][2] = col.b;
      }
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.println("   🎨 [Mode 7] 5-Color Palette Ring!");
    }
    // 0x0C / 0x0E: Strobe Pulse / Rainbow Spectrum (Mode 5 & Mode 8 on Transmitter)
    else if (showCmd == 0x0C || showCmd == 0x0E) {
      showR1 = 255; showG1 = 255; showB1 = 255; // White / Strobe
      isDualColor = false;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.println("   🎨 [Mode 5/8] Strobe / Show Pulse!");
    }
    // 0x12: Wave Pulse (Mode 6 on Transmitter)
    else if (showCmd == 0x12) {
      uint8_t c1 = (len >= 8) ? (payload[7] & 0x1F) : 0x01;  // Color 1 (Purple)
      uint8_t c2 = (len >= 10) ? (payload[9] & 0x1F) : 0x15; // Color 2 (Red)
      CRGB col1 = DISNEY_PALETTE[c1];
      CRGB col2 = DISNEY_PALETTE[c2];
      showR1 = col1.r; showG1 = col1.g; showB1 = col1.b;
      showR2 = col2.r; showG2 = col2.g; showB2 = col2.b;
      isDualColor = true;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.printf("   🎨 [Mode 6] Wave Pulse! Colors: %s / %s\n",
                    DISNEY_PALETTE_NAMES[c1], DISNEY_PALETTE_NAMES[c2]);
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
    }
    // Fallback: Any other valid show command
    else {
      uint8_t c1 = (len >= 10) ? (payload[9] & 0x1F) : 0;
      CRGB col = DISNEY_PALETTE[c1];
      showR1 = col.r; showG1 = col.g; showB1 = col.b;
      isDualColor = false;
      lastShowSyncTime = now;
      disneyDeviceFound = true;
      Serial.printf("   🎨 Generic Show Command 0x%02X Activated!\n", showCmd);
    }
  }
}