#include "DisneyBeacons.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <bluefruit.h>

// --- Pin Definitions ---
#define LED_PIN PIN_006     // P0.06 (NeoPixel Data)
#define BUTTON_PIN PIN_017  // P0.17 (Manual / Mode Button)
#define HAPTIC_PIN PIN_020  // P0.20 (Haptic Motor Output)
#define BATTERY_PIN PIN_031 // P0.31 (AIN7 / Battery ADC)
#define NUM_LEDS 31
#define LED_BRIGHTNESS 120

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- Haptic Feedback Helper ---
void triggerHapticPattern(uint8_t patternCode) {
  Serial.printf("📳 [HAPTIC] Triggering pattern: 0x%02X on pin P0.20\n", patternCode);
  switch (patternCode) {
    case 0xFF: // 1-Second Full Power Test Pulse
      digitalWrite(HAPTIC_PIN, HIGH);
      delay(1000);
      digitalWrite(HAPTIC_PIN, LOW);
      break;
    case 0x01: // Single Tap (250ms)
      digitalWrite(HAPTIC_PIN, HIGH);
      delay(250);
      digitalWrite(HAPTIC_PIN, LOW);
      break;
    case 0x02: // Double Tap (250ms / 150ms / 250ms)
      digitalWrite(HAPTIC_PIN, HIGH); delay(250);
      digitalWrite(HAPTIC_PIN, LOW);  delay(150);
      digitalWrite(HAPTIC_PIN, HIGH); delay(250);
      digitalWrite(HAPTIC_PIN, LOW);
      break;
    case 0x07: // Heavy Rumble (600ms)
      digitalWrite(HAPTIC_PIN, HIGH);
      delay(600);
      digitalWrite(HAPTIC_PIN, LOW);
      break;
    case 0x0A: // Sharp Pulse
      digitalWrite(HAPTIC_PIN, HIGH); delay(300);
      digitalWrite(HAPTIC_PIN, LOW);  delay(100);
      digitalWrite(HAPTIC_PIN, HIGH); delay(300);
      digitalWrite(HAPTIC_PIN, LOW);
      break;
    case 0x0B: // Standard Pulse
    default:
      digitalWrite(HAPTIC_PIN, HIGH);
      delay(300);
      digitalWrite(HAPTIC_PIN, LOW);
      break;
  }
}

// --- Battery Functions ---
static float filteredBatteryVolts = 0.0f;

float readBatteryVoltage() {
  analogReadResolution(12);
  analogReference(AR_INTERNAL);

  uint32_t totalAdc = 0;
  for (int i = 0; i < 32; i++) {
    totalAdc += analogRead(BATTERY_PIN);
  }
  float rawAdc = totalAdc / 32.0f;
  float instantVolts = (rawAdc / 4095.0f) * 3.60f * 2.235f;

  if (filteredBatteryVolts == 0.0f) {
    filteredBatteryVolts = instantVolts;
  } else {
    filteredBatteryVolts = (filteredBatteryVolts * 0.92f) + (instantVolts * 0.08f);
  }
  return filteredBatteryVolts;
}

int calculateBatteryPercentage(float volts) {
  if (volts >= 4.20f) return 100;
  if (volts <= 3.30f) return 0;
  float pct = 0.0f;
  if (volts >= 3.90f) {
    pct = 75.0f + ((volts - 3.90f) / (4.20f - 3.90f)) * 25.0f;
  } else if (volts >= 3.75f) {
    pct = 50.0f + ((volts - 3.75f) / (3.90f - 3.75f)) * 25.0f;
  } else if (volts >= 3.60f) {
    pct = 25.0f + ((volts - 3.60f) / (3.75f - 3.60f)) * 25.0f;
  } else {
    pct = ((volts - 3.30f) / (3.60f - 3.30f)) * 25.0f;
  }
  int finalPct = (int)(pct + 0.5f);
  return constrain(finalPct, 0, 100);
}

void runBatterySweepAnimation() {
  analogReadResolution(12);
  analogReference(AR_INTERNAL);

  uint32_t totalAdc = 0;
  for (int i = 0; i < 32; i++) {
    totalAdc += analogRead(BATTERY_PIN);
  }
  float rawAdc = totalAdc / 32.0f;
  float vBat = (rawAdc / 4095.0f) * 3.60f * 2.235f;

  int pct = calculateBatteryPercentage(vBat);
  int targetLeds = (pct * NUM_LEDS) / 100;
  if (targetLeds < 5) targetLeds = 5;

  Serial.printf("\n🔋 [BATTERY CALIBRATED] Raw ADC: %.1f | Calibrated Voltage: %.2fV | Pct: %d%% | LEDs: %d\n",
                rawAdc, vBat, pct, targetLeds);

  strip.clear();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.show();

  for (int i = 0; i < targetLeds; i++) {
    float ratio = (float)i / (float)(NUM_LEDS - 1);
    uint16_t hue;
    if (ratio <= 0.5f) {
      hue = (uint16_t)((ratio / 0.5f) * 43690.0f);
    } else {
      hue = 43690 - (uint16_t)(((ratio - 0.5f) / 0.5f) * 21845.0f);
    }
    strip.setPixelColor(i, strip.ColorHSV(hue, 255, 255));
    strip.show();
    delay(30);
  }

  delay(2500);

  for (int b = 255; b >= 0; b -= 15) {
    for (int i = 0; i < targetLeds; i++) {
      float ratio = (float)i / (float)(NUM_LEDS - 1);
      uint16_t hue;
      if (ratio <= 0.5f) {
        hue = (uint16_t)((ratio / 0.5f) * 43690.0f);
      } else {
        hue = 43690 - (uint16_t)(((ratio - 0.5f) / 0.5f) * 21845.0f);
      }
      strip.setPixelColor(i, strip.ColorHSV(hue, 255, b));
    }
    strip.show();
    delay(20);
  }

  strip.clear();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.show();
}

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
volatile uint8_t manualMode = 0; // 0 = Auto/BLE Rainbow, 1-7 = Custom Animations
const uint8_t NUM_MANUAL_MODES = 8;
const char* MODE_NAMES[] = {
    "Auto (Rainbow / BLE)",
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

void setup() {
  Serial.begin(115200);

  // Button & Haptic Pin Setup
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(HAPTIC_PIN, OUTPUT);
  digitalWrite(HAPTIC_PIN, LOW);

  // 📳 Startup Haptic Test: 1-Second Full-Power Pulse
  triggerHapticPattern(0xFF);

  // Onboard LEDs Off
  pinMode(PIN_015, OUTPUT);
  digitalWrite(PIN_015, LOW);
  pinMode(PIN_109, OUTPUT);
  digitalWrite(PIN_109, LOW);

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();

  Serial.println("\n=========================================");
  Serial.println("   NRF52840 DISNEY BLE RECEIVER STARTED  ");
  Serial.println("=========================================");

  // Initialize Bluefruit BLE Central / Scanner
  Bluefruit.begin(0, 1); // 0 Peripherals, 1 Central
  Bluefruit.setTxPower(4);

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  Bluefruit.Scanner.setInterval(6400, 3200);
  Bluefruit.Scanner.filterMSD(
      DISNEY_COMPANY_ID_LE);  // Filter Disney Manufacturer ID
  Bluefruit.Scanner.start(0); // 0 = continuous scan mode with duty cycling

  Serial.println(
      ">>> BLE Scanner initialized & searching for Disney Beacons...");

  // Run battery sweep animation once at startup
  runBatterySweepAnimation();
}

void loop() {
  unsigned long now = millis();
  gHue++;

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
      Serial.println("🔘 [BUTTON] Long Press (>1.2s)! Running Battery Sweep...");
      triggerHapticPattern(0x02); // Double tap haptic pulse
      runBatterySweepAnimation();
    }
  } else {
    if (buttonPressed) {
      buttonPressed = false;
      if (!longPressHandled && (now - pressStartTime < 1200) && (now - pressStartTime > DEBOUNCE_DELAY)) {
        manualMode = (manualMode + 1) % NUM_MANUAL_MODES;
        Serial.printf("🔘 [BUTTON] Short Press! Mode %d: %s\n", manualMode, MODE_NAMES[manualMode]);
        triggerHapticPattern(0x01); // Short single click haptic pulse
      }
    }
  }

  // --- Display Priority ---
  if (disneyDeviceFound) {
    executeDisneyMicrocode();

    if (now - lastShowSyncTime > 4000) {
      disneyDeviceFound = false;
    }
  } else {
    // 2. Manual Animations or Default Rainbow Mode
    switch (manualMode) {
      case 0: // Auto (Idle Ambient Rainbow)
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
  delay(20);
}

// --- Animation Routines ---
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
    else if (showCmd == 0x08 && len >= 10) {
      showR1 = (payload[7] & 0x3F) << 2;
      showG1 = (payload[8] & 0x3F) << 2;
      showB1 = (payload[9] & 0x3F) << 2;
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
    else if (showCmd == 0x09 && len >= 12) {
      for (int s = 0; s < 5; s++) {
        uint8_t cIdx = payload[7 + s] & 0x1F;
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