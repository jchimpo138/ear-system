#include "DisneyBeacons.h"
#include <Arduino.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEUtils.h>

// =========================================================================
// GLOBALS
// =========================================================================
BLEAdvertising *pAdvertising;

// Current broadcast settings
uint8_t currentMode = 1; // 1=SinglePalette, 2=DualPalette, 3=DirectRGB,
                         // 4=HighContrast, 5=StrobePulse, 6=WavePulse
uint8_t color1Idx = 0;   // Palette index 0-31 (Cyan)
uint8_t color2Idx = 15;  // Palette index 0-31 (Yellow Orange)
uint8_t vibePattern =
    0x0B; // Haptic: 0x0B=Tap, 0x07=Rumble, 0x0A=Alert, 0x01=Tick, 0x09=Wave
uint8_t customR = 200, customG = 0, customB = 255; // Direct RGB values
int intervalMs = 25; // Rapid 25ms advertising interval (_AD_INTERVAL = 0.025s)
int broadcastDurationMs =
    3000; // 3.0s burst broadcast duration (_BROADCAST_SECONDS = 3.0s)
bool burstMode = true;     // True = 3s timed burst, False = continuous repeat
bool broadcasting = false; // Active broadcast flag
uint8_t timingByte =
    0x09; // Default Disney timing: 0b scaler, 0s fade, time val 9 (20s on)
int txCount = 0;

const char *VIBE_NAMES[16] = {
    "0x0 None",         "0x1 Tap (-)",     "0x2 Double (--)",
    "0x3 Triple (---)", "0x4 Pulse (--*)", "0x5 Pattern 5",
    "0x6 Pattern 6",    "0x7 Rumble (#)",  "0x8 Ticks ('''''' )",
    "0x9 Tap (-)",      "0xA Sharp (*)",   "0xB Pulse (%)",
    "0xC None",         "0xD None",        "0xE None",
    "0xF None"};

// =========================================================================
// BLE PAYLOAD BUILDER
// =========================================================================
void sendPayload(const uint8_t *data, size_t len, int durationMs = 0) {
  if (!pAdvertising)
    return;
  pAdvertising->stop();

  BLEAdvertisementData advData;
  advData.setFlags(0x06); // LE General Discoverable Mode

  std::string mfgData = "";
  mfgData += (char)0x83;
  mfgData += (char)0x01;
  for (size_t i = 0; i < len; i++) {
    mfgData += (char)data[i];
  }

  advData.setManufacturerData(mfgData);
  pAdvertising->setAdvertisementData(advData);

  uint16_t bleUnits = (intervalMs * 1000) / 625;
  if (bleUnits < 0x20)
    bleUnits = 0x20;
  pAdvertising->setMinInterval(bleUnits);
  pAdvertising->setMaxInterval(bleUnits);

  pAdvertising->start();
  broadcasting = true;

  if (durationMs > 0) {
    delay(durationMs);
    pAdvertising->stop();
    broadcasting = false;
  }
}

// =========================================================================
// WAKE PING (0xCC03000000)
// =========================================================================
void sendWakePing() {
  const uint8_t pingData[] = {0xCC, 0x03, 0x00, 0x00, 0x00};
  Serial.printf("[%lu ms] 📡 Sending 500ms CC03 Wake Ping @ 25ms interval...\n",
                millis());
  sendPayload(pingData, sizeof(pingData), 500); // 500ms wake burst
}

// =========================================================================
// BROADCAST MODES
// =========================================================================
void broadcastSinglePalette() {
  uint8_t data[] = {0xE1,
                    0x00,
                    0xE9,
                    0x05,
                    0x00,
                    timingByte,
                    0x0E,
                    (uint8_t)(color1Idx & 0x1F),
                    (uint8_t)(0xB0 | (vibePattern & 0x0F))};
  sendPayload(data, sizeof(data));
}

void broadcastDualPalette() {
  uint8_t data[] = {0xE2,
                    0x00,
                    0xE9,
                    0x06,
                    0x00,
                    timingByte,
                    0x0F,
                    (uint8_t)(0x40 | (color1Idx & 0x1F)),
                    (uint8_t)(0x40 | (color2Idx & 0x1F)),
                    (uint8_t)(0xB0 | (vibePattern & 0x0F))};
  sendPayload(data, sizeof(data));
}

void broadcastDirectRGB() {
  uint8_t data[] = {0xE1,
                    0x00,
                    0xE9,
                    0x08,
                    0x00,
                    0x0E,
                    0xD2,
                    0x55,
                    (uint8_t)((customR >> 1) & 0x7E),
                    (uint8_t)((customG >> 1) & 0x7E),
                    (uint8_t)((customB >> 1) & 0x7E),
                    (uint8_t)(0xB0 | (vibePattern & 0x0F))};
  sendPayload(data, sizeof(data));
}

void broadcastHighContrast() {
  const uint8_t data[] = {0xE1, 0x00, 0xE9, 0x0B, 0x0B, 0x0F, 0x0F, 0x5C,
                          0x42, 0x5C, 0xA2, 0xDC, 0x42, 0x32, 0x05};
  sendPayload(data, sizeof(data));
}

void broadcastStrobePulse() {
  uint8_t data[] = {0xE1,
                    0x00,
                    0xE9,
                    0x0E,
                    0x00,
                    timingByte,
                    0x0F,
                    (uint8_t)(0x40 | (color1Idx & 0x1F)),
                    (uint8_t)(0x40 | (color2Idx & 0x1F)),
                    (uint8_t)(0xB0 | (vibePattern & 0x0F))};
  sendPayload(data, sizeof(data));
}

void broadcastWavePulse() {
  uint8_t data[] = {0xE2,
                    0x00,
                    0xE9,
                    0x12,
                    0x00,
                    timingByte,
                    0x0F,
                    (uint8_t)(0x40 | (color1Idx & 0x1F)),
                    (uint8_t)(0x40 | (color2Idx & 0x1F)),
                    (uint8_t)(0xB0 | (vibePattern & 0x0F))};
  sendPayload(data, sizeof(data));
}

void broadcastFiveColorPalette() {
  uint8_t data[] = {0xE1,
                    0x00,
                    0xE9,
                    0x09,
                    0x00,
                    0x0E,
                    0x0F,
                    (uint8_t)(0xA0 | (color1Idx & 0x1F)),       // Top-Left
                    (uint8_t)(0xA0 | ((color1Idx + 4) & 0x1F)), // Bottom-Left
                    (uint8_t)(0xA0 | (color2Idx & 0x1F)),       // Bottom-Right
                    (uint8_t)(0xA0 | ((color2Idx + 4) & 0x1F)), // Top-Right
                    (uint8_t)(0xA0 | 0x00),                     // Center (Cyan)
                    (uint8_t)(0xB0 | (vibePattern & 0x0F))};
  sendPayload(data, sizeof(data));
}

void broadcastRainbowShow() {
  const uint8_t data[] = {0xE1, 0x00, 0xE9, 0x0C, 0x00, 0x0F, 0x0F, 0x5D,
                          0x46, 0x5B, 0xF0, 0x05, 0x32, 0x37, 0x48, 0xB0};
  sendPayload(data, sizeof(data));
}

void broadcastOrangeAlert() {
  const uint8_t data[] = {0xE1, 0x00, 0xE9, 0x0C, 0x00, 0xEF, 0x0F, 0x4F,
                          0x4F, 0x5B, 0xF0, 0xFB, 0x14, 0x37, 0x48, 0x95};
  sendPayload(data, sizeof(data));
}

void broadcastStatueBeacon() {
  // Fab 50 Statue #53 (Mickey Mouse - Magic Kingdom Hub)
  const uint8_t data[] = {0xC4, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, '5',  '3',  0x00};
  sendPayload(data, sizeof(data));
}

void broadcastWandCast() {
  // Starlight Bubble Wand Cast (Cyan Spell)
  const uint8_t data[] = {0xCF, 0x0B, 0x00, 0xC4, 0x20, 0x22, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  sendPayload(data, sizeof(data));
}

// Apply current mode & execute 3.0s burst broadcast strategy
void applyCurrentMode() {
  sendWakePing();
  Serial.printf(
      "[%lu ms] 🚀 Broadcasting Mode %d payload (Interval: %dms) ...\n",
      millis(), currentMode, intervalMs);

  switch (currentMode) {
  case 1:
    broadcastSinglePalette();
    break;
  case 2:
    broadcastDualPalette();
    break;
  case 3:
    broadcastDirectRGB();
    break;
  case 4:
    broadcastHighContrast();
    break;
  case 5:
    broadcastStrobePulse();
    break;
  case 6:
    broadcastWavePulse();
    break;
  case 7:
    broadcastFiveColorPalette();
    break;
  case 8:
    broadcastStatueBeacon();
    break;
  case 9:
    broadcastWandCast();
    break;
  case 10:
    broadcastRainbowShow();
    break;
  case 11:
    broadcastOrangeAlert();
    break;
  }
  txCount++;

  if (burstMode) {
    Serial.printf("[%lu ms] ⏱️ Holding burst for %d ms...\n", millis(),
                  broadcastDurationMs);
    delay(broadcastDurationMs);
    if (pAdvertising)
      pAdvertising->stop();
    broadcasting = false;
    Serial.printf("[%lu ms] ⏹️ 3.0s Burst complete! Advertising stopped.\n",
                  millis());
  }
}

// =========================================================================
// STATUS LINE (compact one-liner for heartbeat)
// =========================================================================
void printStatus() {
  const char *modeNames[] = {"",
                             "Single Palette (E9 05)",
                             "Dual Split Palette (E9 06)",
                             "Direct Custom RGB (E9 08)",
                             "High-Contrast Circle (E9 0B)",
                             "Strobe Pulse (E9 0E)",
                             "Wave Pulse (E9 12)",
                             "5-Color Palette Ring (E9 09)",
                             "Fab 50 Statue #53 (C4 10)",
                             "Starlight Wand Cast (CF 0B)",
                             "Taste the Rainbow (E9 0C)",
                             "Orange Alert Strobe (E9 0C)"};

  // Decode timing for timingByte (Modes 1 & 2)
  bool alwaysOn = (timingByte & 0x80) != 0;
  bool scalerBit = (timingByte & 0x40) != 0;
  uint8_t fadeSec = (timingByte >> 4) & 0x03;
  uint8_t timeVal = timingByte & 0x0F;
  float calculatedOnSec = scalerBit ? (3.1f * (float)timeVal + 0.5f)
                                    : (1.5f * (float)timeVal + 0.5f);

  Serial.println("\n==================================================");
  Serial.printf("[%lu ms] 🎨 PATTERN : Mode %d - %s\n", millis(), currentMode,
                (currentMode <= 11) ? modeNames[currentMode] : "Unknown");

  // Color Breakdown
  if (currentMode == 1 || currentMode == 5 || currentMode == 6) {
    Serial.printf(
        "           🎨 COLOR   : Primary: C1=%d (%s) | Secondary: C2=%d (%s)\n",
        color1Idx, DISNEY_PALETTE_NAMES[color1Idx], color2Idx,
        DISNEY_PALETTE_NAMES[color2Idx]);
  } else if (currentMode == 2) {
    Serial.printf("           🎨 COLOR   : Left Ear: C1=%d (%s) | Right Ear: "
                  "C2=%d (%s)\n",
                  color1Idx, DISNEY_PALETTE_NAMES[color1Idx], color2Idx,
                  DISNEY_PALETTE_NAMES[color2Idx]);
  } else if (currentMode == 3) {
    Serial.printf("           🎨 COLOR   : Direct Custom RGB (%d, %d, %d)\n",
                  customR, customG, customB);
  } else if (currentMode == 7) {
    Serial.printf(
        "           🎨 COLOR   : 5-Color Ring Palette [C1=%s, C2=%s]\n",
        DISNEY_PALETTE_NAMES[color1Idx], DISNEY_PALETTE_NAMES[color2Idx]);
  } else if (currentMode == 8) {
    Serial.printf("           🎨 COLOR   : Statue Gold (255, 215, 0)\n");
  } else if (currentMode == 9) {
    Serial.printf("           🎨 COLOR   : Wand Spell Palette [Idx: %d (%s)]\n",
                  color1Idx, DISNEY_PALETTE_NAMES[color1Idx]);
  } else {
    Serial.printf("           🎨 COLOR   : Park Show Color Microcode\n");
  }

  // Expected On-Time & Fade
  if (currentMode == 1 || currentMode == 2) {
    Serial.printf("           ⏱️ TIME ON : %.1f sec %s | Fade-Out: %d sec | Raw "
                  "Timing: 0x%02X\n",
                  alwaysOn ? 10.0f : calculatedOnSec,
                  alwaysOn ? "(Always-On Default)" : "(Calculated)", fadeSec,
                  timingByte);
  } else if (currentMode == 3 || currentMode == 7) {
    Serial.printf("           ⏱️ TIME ON : 10.0 sec (Zone Hold)\n");
  } else if (currentMode == 5) {
    Serial.printf("           ⏱️ TIME ON : 3.6 sec (Rapid Strobe Pulse)\n");
  } else if (currentMode == 6) {
    Serial.printf("           ⏱️ TIME ON : 5.0 sec (Wave Pulse Gradient)\n");
  } else if (currentMode == 4) {
    Serial.printf("           ⏱️ TIME ON : 4.0 sec (High-Contrast Strobe)\n");
  } else if (currentMode == 8) {
    Serial.printf("           ⏱️ TIME ON : 3.0 sec (Statue Rumble Burst)\n");
  } else if (currentMode == 9) {
    Serial.printf(
        "           ⏱️ TIME ON : 2.5 sec (Wand Spell Sparkle Burst)\n");
  } else if (currentMode == 10 || currentMode == 11) {
    Serial.printf(
        "           ⏱️ TIME ON : 29.0 sec (Park Show Microcode Sequence)\n");
  }

  Serial.printf("           📳 HAPTIC  : 0x%02X (Vibe Pattern)\n", vibePattern);
  Serial.println("==================================================");
}

// =========================================================================
// CONSOLE MENU
// =========================================================================
void printMenu() {
  Serial.println("\n==================================================");
  Serial.println("    DISNEY BLE TRANSMITTER - COM8");
  Serial.println("==================================================");
  Serial.println("  DYNAMIC MODES:");
  Serial.println("   [1] Single Palette Color  (E9 05)");
  Serial.println("   [2] Dual Split Palette    (E9 06)");
  Serial.println("   [3] Direct Custom RGB     (E9 08)");
  Serial.println("   [4] High-Contrast Circle  (E9 0B)");
  Serial.println("   [5] Strobe Pulse          (E9 0E)");
  Serial.println("   [6] Wave Pulse            (E9 12)");
  Serial.println("   [7] 5-Color Ring Palette  (E9 09)");
  Serial.flush();
  delay(10);

  Serial.println("--------------------------------------------------");
  Serial.println("  OFFICIAL PARK SHOW PRESETS:");
  Serial.println("   [8] Fab 50 Statue #53     (C4 10)");
  Serial.println("   [9] Starlight Wand Cast   (CF 0B)");
  Serial.println("   [w] Taste the Rainbow     (E9 0C)");
  Serial.println("   [o] Orange Alert Strobe   (E9 0C)");
  Serial.flush();
  delay(10);

  Serial.println("--------------------------------------------------");
  Serial.println("  ADJUSTMENTS:");
  Serial.printf("   [c] Cycle Color 1  (now: %d - %s)\n", color1Idx,
                DISNEY_PALETTE_NAMES[color1Idx]);
  Serial.printf("   [d] Cycle Color 2  (now: %d - %s)\n", color2Idx,
                DISNEY_PALETTE_NAMES[color2Idx]);
  Serial.printf("   [v] Cycle Haptic   (now: 0x%02X)\n", vibePattern);
  Serial.printf("   [r] Cycle RGB Red   (now: %d)\n", customR);
  Serial.printf("   [g] Cycle RGB Green (now: %d)\n", customG);
  Serial.printf("   [b] Cycle RGB Blue  (now: %d)\n", customB);
  Serial.printf("   [t] Toggle Timing   (now: %d ms)\n", intervalMs);
  Serial.printf("   [f] Cycle Fade-Out  (now: 0x%02X)\n", timingByte);
  Serial.flush();
  delay(10);

  Serial.println("--------------------------------------------------");
  Serial.println("  CONTROL:");
  Serial.println("   [0] Stop Broadcast");
  Serial.println("   [s] Start Broadcast");
  Serial.println("   [m] Show This Menu");
  Serial.println("==================================================\n");
  Serial.flush();
}

// =========================================================================
// RAW TUPLE & HEX PAYLOAD PARSER
// =========================================================================
void parseAndBroadcastRawPayload(String input) {
  input.trim();
  if (input.length() == 0)
    return;

  uint8_t bytes[60];
  int byteCount = 0;

  // Check if input contains decimal values with commas (e.g. "(225, 0, 233,
  // ...)")
  bool hasCommas = input.indexOf(',') >= 0;
  bool isDecimal =
      hasCommas && input.indexOf("0x") < 0 && input.indexOf("0X") < 0;

  if (isDecimal) {
    int startIdx = 0;
    while (startIdx < input.length() && byteCount < 60) {
      int commaIdx = input.indexOf(',', startIdx);
      if (commaIdx < 0)
        commaIdx = input.length();
      String token = input.substring(startIdx, commaIdx);
      token.trim();
      String cleanToken = "";
      for (size_t i = 0; i < token.length(); i++) {
        if (isdigit(token[i]))
          cleanToken += token[i];
      }
      if (cleanToken.length() > 0) {
        bytes[byteCount++] = (uint8_t)cleanToken.toInt();
      }
      startIdx = commaIdx + 1;
    }
  } else {
    // Parse hex string / tuple / C-array
    String clean = "";
    for (size_t i = 0; i < input.length(); i++) {
      char c = input[i];
      if (isxdigit(c)) {
        clean += c;
      }
    }
    for (size_t i = 0; i + 1 < clean.length() && byteCount < 60; i += 2) {
      String hexPair = clean.substring(i, i + 2);
      bytes[byteCount++] = (uint8_t)strtol(hexPair.c_str(), NULL, 16);
    }
  }

  if (byteCount == 0) {
    Serial.println(">> ❌ Could not parse valid bytes from input!");
    return;
  }

  sendWakePing();
  Serial.printf(
      "[%lu ms] 🚀 Broadcasting Custom Raw Payload for 3.0s burst...\n",
      millis());
  sendPayload(bytes, byteCount, 3000); // 3-second burst, then auto-stop!
  if (pAdvertising)
    pAdvertising->stop();
  broadcasting = false;
  txCount++;

  Serial.println("\n==================================================");
  Serial.printf("[%lu ms] ⏹️ 3.0s Burst Complete! Custom Payload Broadcast "
                "Stopped (%d bytes):\n",
                millis(), byteCount);
  Serial.print("           HEX: ");
  for (int b = 0; b < byteCount; b++) {
    Serial.printf("%02X ", bytes[b]);
  }
  Serial.println("\n==================================================");
}

// =========================================================================
// SERIAL INPUT HANDLER
// =========================================================================
void handleInput() {
  while (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() == 0)
      continue;

    // Single character command
    if (input.length() == 1) {
      char cmd = input[0];
      switch (cmd) {
      // --- Broadcast Modes ---
      case '1':
        currentMode = 1;
        applyCurrentMode();
        printStatus();
        break;
      case '2':
        currentMode = 2;
        applyCurrentMode();
        printStatus();
        break;
      case '3':
        currentMode = 3;
        applyCurrentMode();
        printStatus();
        break;
      case '4':
        currentMode = 4;
        applyCurrentMode();
        printStatus();
        break;
      case '5':
        currentMode = 5;
        applyCurrentMode();
        printStatus();
        break;
      case '6':
        currentMode = 6;
        applyCurrentMode();
        printStatus();
        break;
      case '7':
        currentMode = 7;
        applyCurrentMode();
        printStatus();
        break;
      case '8':
        currentMode = 8;
        applyCurrentMode();
        printStatus();
        break;
      case '9':
        currentMode = 9;
        applyCurrentMode();
        printStatus();
        break;
      case 'w':
      case 'W':
        currentMode = 10;
        applyCurrentMode();
        printStatus();
        break;
      case 'o':
      case 'O':
        currentMode = 11;
        applyCurrentMode();
        printStatus();
        break;

      // --- Color 1 Cycle ---
      case 'c':
      case 'C':
        color1Idx = (color1Idx + 1) % 32;
        Serial.printf(">> Color 1 = %d (%s)\n", color1Idx,
                      DISNEY_PALETTE_NAMES[color1Idx]);
        if (broadcasting)
          applyCurrentMode();
        break;

      // --- Color 2 Cycle ---
      case 'd':
      case 'D':
        color2Idx = (color2Idx + 1) % 32;
        Serial.printf(">> Color 2 = %d (%s)\n", color2Idx,
                      DISNEY_PALETTE_NAMES[color2Idx]);
        if (broadcasting)
          applyCurrentMode();
        break;

      // --- Haptic Cycle ---
      case 'v':
      case 'V': {
        vibePattern = (vibePattern + 1) % 16;
        Serial.printf(">> Haptic = 0x%02X (%s)\n", vibePattern,
                      VIBE_NAMES[vibePattern]);
        if (broadcasting)
          applyCurrentMode();
        break;
      }

      // --- Fade-Out / Timing Byte Cycling ---
      case 'f':
      case 'F': {
        const uint8_t timingPresets[] = {0x09, 0x19, 0x29, 0x39,
                                         0x12, 0x32, 0x89};
        static uint8_t ti = 0;
        ti = (ti + 1) % 7;
        timingByte = timingPresets[ti];
        uint8_t fadeSec = (timingByte >> 4) & 0x03;
        bool alwaysOn = (timingByte & 0x80) != 0;
        Serial.printf(">> Timing Byte = 0x%02X (Fade: %ds, AlwaysOn: %s)\n",
                      timingByte, fadeSec, alwaysOn ? "YES" : "NO");
        if (broadcasting && (currentMode == 1 || currentMode == 2))
          applyCurrentMode();
        break;
      }

      // --- RGB Channel Cycling ---
      case 'r':
        customR = (customR + 32) % 256;
        Serial.printf(">> R = %d\n", customR);
        if (broadcasting && currentMode == 3)
          applyCurrentMode();
        break;
      case 'g':
        customG = (customG + 32) % 256;
        Serial.printf(">> G = %d\n", customG);
        if (broadcasting && currentMode == 3)
          applyCurrentMode();
        break;
      case 'b':
        customB = (customB + 32) % 256;
        Serial.printf(">> B = %d\n", customB);
        if (broadcasting && currentMode == 3)
          applyCurrentMode();
        break;

      // --- Timing Toggle ---
      case 't':
      case 'T':
        if (intervalMs == 100)
          intervalMs = 250;
        else if (intervalMs == 250)
          intervalMs = 500;
        else if (intervalMs == 500)
          intervalMs = 1000;
        else
          intervalMs = 100;
        Serial.printf(">> Interval = %d ms\n", intervalMs);
        if (broadcasting)
          applyCurrentMode();
        break;

      // --- Stop ---
      case '0':
        broadcasting = false;
        if (pAdvertising)
          pAdvertising->stop();
        Serial.println(">> BROADCAST STOPPED");
        break;

      // --- Start ---
      case 's':
      case 'S':
        applyCurrentMode();
        Serial.println(">> BROADCAST STARTED");
        printStatus();
        break;

      // --- Menu ---
      case 'm':
      case 'M':
      case '?':
      case 'h':
      case 'H':
        printMenu();
        break;

      default:
        Serial.printf(">> Unknown: '%c' - press 'm' for menu\n", cmd);
        break;
      }
    } else {
      // Multi-character input: parse as raw tuple / hex payload
      parseAndBroadcastRawPayload(input);
    }
  }
}

// =========================================================================
// SETUP & LOOP
// =========================================================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  Serial.setTimeout(50); // Fast response for custom hex string input

  BLEDevice::init("DisneyBeaconTX");
  pAdvertising = BLEDevice::getAdvertising();

  Serial.println("\n==========================================");
  Serial.println("  DISNEY BLE TRANSMITTER - ONLINE");
  Serial.println("==========================================");

  printMenu();
  applyCurrentMode();
  printStatus();
}

void loop() {
  static unsigned long lastHeartbeat = 0;

  handleInput();

  // Slow heartbeat (every 15 seconds) with a clean simple one-liner
  if (millis() - lastHeartbeat >= 15000) {
    lastHeartbeat = millis();
    const char *modeNames[] = {"",          "SinglePalette",    "DualPalette",
                               "DirectRGB", "HighContrast",     "StrobePulse",
                               "WavePulse", "FiveColorPalette", "StatueBeacon",
                               "WandCast",  "RainbowShow",      "OrangeAlert"};
    Serial.printf("💚 [ALIVE] Mode %d (%s) | C1: %d-%s | C2: %d-%s\n",
                  currentMode,
                  (currentMode <= 11) ? modeNames[currentMode] : "Unknown",
                  color1Idx, DISNEY_PALETTE_NAMES[color1Idx], color2Idx,
                  DISNEY_PALETTE_NAMES[color2Idx]);
  }
}