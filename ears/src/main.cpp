#include <Arduino.h>
#include <Wire.h>
#include <FastLED.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include "DisneyBeacons.h"

// --- Pin Definitions ---
#define LED_PIN         2   // Connected to GP2 (Row 33, Column B)
#define BUTTON_PIN      4   // Connected to GP4 (Row 35, Column B)
#define HAPTIC_PIN      5   // Connected to GP5 (Row 36, Column B -> Row 50)
#define SDA_PIN         8   // I2C Data Pin (GP8)
#define SCL_PIN         9   // I2C Clock Pin (GP9)
#define NUM_PIXELS      32  // Bench testing pixel count (Split: 16 Left, 16 Right)

// --- LIS3DSH Accelerometer Registers ---
#define LIS3DSH_ADDR1       0x1D // SA0 = HIGH (3.3V)
#define LIS3DSH_ADDR2       0x1E // SA0 = LOW (GND)
#define LIS3DSH_WHO_AM_I    0x0F // Expected return: 0x3F
#define LIS3DSH_CTRL_REG4   0x20 // ODR and axis selection
#define LIS3DSH_CTRL_REG5   0x24 // Scale selection
#define LIS3DSH_OUT_X_L     0x28 // Start of output registers

uint8_t lis3dshAddr = 0;
bool lis3dshFound = false;
unsigned long lastMotionTriggerTime = 0;

CRGB leds[NUM_PIXELS];

int animationState = 0; 
bool lastButtonState = HIGH;
uint8_t gHue = 0;       

// --- BLE Scanner Control Variables ---
BLEScan* pBLEScan;
unsigned long lastScanTime = 0;
const unsigned long scanInterval = 4000; 
const int scanDuration = 2;              
bool isScanning = false;

// --- DYNAMIC SHOW INTERCEPT VARIABLES ---
volatile bool disneyDeviceFound = false;
volatile uint8_t receivedCommandType = 0x00; // 0x05 (Single), 0x06 (Dual), 0x08 (Raw RGB)
volatile uint8_t dynamicVibePattern = 0x00; 

// Pass colors across thread contexts using compiler-safe volatile primitives
volatile uint8_t showR1 = 255;
volatile uint8_t showG1 = 215;
volatile uint8_t showB1 = 0;

volatile uint8_t showR2 = 0;
volatile uint8_t showG2 = 0;
volatile uint8_t showB2 = 0;

volatile uint8_t showColors5[5][3]; // 5-slot palette RGB matrix
char statueIdStr[4] = "??";
volatile bool showSyncHold = false;
volatile unsigned long holdStartTime = 0;

volatile float decodedOnTimeSec = 10.0f;
volatile float decodedFadeTimeSec = 0.0f;
volatile bool decodedAlwaysOn = true;
volatile uint8_t rawTimingByte = 0x00;

void parseDisneyTimingByte(uint8_t timingByte) {
    rawTimingByte = timingByte;
    decodedAlwaysOn = (timingByte & 0x80) != 0;
    bool scalerBit = (timingByte & 0x40) != 0;
    uint8_t fadeBits = (timingByte >> 4) & 0x03; // 00b=0s, 01b=1s, 10b=2s, 11b=3s
    decodedFadeTimeSec = (float)fadeBits;
    uint8_t timeVal = timingByte & 0x0F;
    if (scalerBit) {
        decodedOnTimeSec = 3.1f * (float)timeVal + 0.5f;
    } else {
        decodedOnTimeSec = 1.5f * (float)timeVal + 0.5f;
    }
}

// --- Function Prototypes ---
void scanCompleteCB(BLEScanResults scanResults);
void executeDynamicShowSync();
void triggerHeavyDoubleTap();
void runActiveAnimation();
void runIgnitionPulse();
void runMeteorChase();
void runCyberPlasma();
void runLightningStorm();
void runNeonGlitch();
void runHyperDrive();

// Global debounce timer to prevent duplicate triggers from a single 3.0s burst
volatile unsigned long lastShowSyncTime = 0;

// --- EXCLUSIVE SHOW PACKET PARSER CALLBACK ---
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!advertisedDevice.haveManufacturerData()) return;

        // 3.5s Debounce: Ignore duplicate packets from the same broadcast burst
        unsigned long now = millis();
        if (now - lastShowSyncTime < 3500) return;

        std::string mData = advertisedDevice.getManufacturerData();
        int len = mData.length();
        if (len < 5) return;

        // Fix C++ sign-extension compiler bugs
        uint8_t payload[40];
        int copyLen = (len < 40) ? len : 40;
        for (int i = 0; i < copyLen; i++) {
            payload[i] = (uint8_t)mData[i];
        }

        // 1. STARLIGHT BUBBLE WAND CAST DETECTION (CF 0B or payload[2] == 0xCF)
        if ((payload[0] == 0xCF && payload[1] == 0x0B) || (payload[2] == 0xCF && payload[3] == 0x0B)) {
            uint8_t wandColIdx = (copyLen >= 13) ? (payload[12] & 0x1F) : 0;
            CRGB wandCol = DISNEY_PALETTE[wandColIdx];
            showR1 = wandCol.r; showG1 = wandCol.g; showB1 = wandCol.b;
            dynamicVibePattern = 0x01; // Quick tick
            receivedCommandType = 0xC1;
            lastShowSyncTime = now;
            disneyDeviceFound = true;
            Serial.printf("\n✨ [COM7 RX] STARLIGHT BUBBLE WAND CAST DETECTED! Palette Index: %d\n", wandColIdx);
            return;
        }

        // 2. FAB 50 STATUE BEACON DETECTION (0xC4 Header - C4 10 or C4 15)
        if ((payload[0] == 0xC4 && (payload[1] == 0x10 || payload[1] == 0x15)) || 
            (payload[2] == 0xC4 && (payload[3] == 0x10 || payload[3] == 0x15))) {
            int offset = (payload[0] == 0xC4) ? 0 : 2;
            if (copyLen >= (offset + 17)) {
                statueIdStr[0] = (char)payload[offset + 15];
                statueIdStr[1] = (char)payload[offset + 16];
                statueIdStr[2] = '\0';
            } else {
                statueIdStr[0] = '?'; statueIdStr[1] = '?'; statueIdStr[2] = '\0';
            }
            showR1 = 255; showG1 = 215; showB1 = 0; // Gold
            dynamicVibePattern = 0x07; // Continuous rumble
            receivedCommandType = 0xC4;
            lastShowSyncTime = now;
            disneyDeviceFound = true;
            Serial.printf("\n🗿 [COM7 RX] FAB 50 STATUE BEACON DETECTED! Sub-type: C4 %02X | Statue ID: %s\n", payload[offset + 1], statueIdStr);
            return;
        }

        // Validate Disney Company ID (0x0183 or 0x8301)
        uint16_t companyId = (payload[1] << 8) | payload[0];
        if (companyId != DISNEY_COMPANY_ID_LE && companyId != DISNEY_COMPANY_ID_BE) return;

        // Is this an actual SHOW/COMMAND packet? (0xE9)
        if (payload[4] == DISNEY_HEADER_SHOW) {
            uint8_t showCmd = payload[5];
            const DisneyShowCommand* showInfo = getDisneyShowInfo(showCmd);

            Serial.printf("\n[%lu ms] ✨ [COM7 RX] DISNEY BEACON DETECTED! Raw Payload: ", millis());
            for (int i = 0; i < copyLen; i++) {
                Serial.printf("%02X ", payload[i]);
            }
            Serial.println();

            if (showInfo != nullptr) {
                Serial.printf("   📍 Known Match: %s [%s]\n", showInfo->name, showInfo->location);
            }

            // CASE 1: Single Color from Palette (E9 05) - Length >= 10
            if (showCmd == 0x05 && len >= 10) {
                parseDisneyTimingByte(payload[7]);
                uint8_t colorIdx = payload[9] & 0x1F; // Color Byte is at payload[9] (with 0x8301 CID)
                CRGB col = DISNEY_PALETTE[colorIdx];
                showR1 = col.r; showG1 = col.g; showB1 = col.b;
                dynamicVibePattern = (len >= 11) ? (payload[10] & 0x0F) : 0x0B;
                receivedCommandType = 0x05;
                lastShowSyncTime = now;
                disneyDeviceFound = true;
                Serial.printf("   🎨 Mode: Single Palette Color [Idx: %d (%s)] | Vibe: 0x%02X | Timing 0x%02X (AlwaysOn:%s, On:%.1fs, Fade:%.1fs)\n", 
                              colorIdx, DISNEY_PALETTE_NAMES[colorIdx], dynamicVibePattern, payload[7], decodedAlwaysOn ? "YES" : "NO", decodedOnTimeSec, decodedFadeTimeSec);
            } 
            
            // CASE 2: Dual Color Palette Colors (E9 06) - Length >= 11
            else if (showCmd == 0x06 && len >= 11) {
                parseDisneyTimingByte(payload[7]);
                uint8_t innerColorIdx = payload[9] & 0x1F;  // Inner Color Byte (Payload[9])
                uint8_t outerColorIdx = (len >= 12) ? (payload[10] & 0x1F) : innerColorIdx; // Outer Color Byte (Payload[10])
                CRGB col1 = DISNEY_PALETTE[innerColorIdx];
                CRGB col2 = DISNEY_PALETTE[outerColorIdx];
                showR1 = col1.r; showG1 = col1.g; showB1 = col1.b; // Left Ear / Inner
                showR2 = col2.r; showG2 = col2.g; showB2 = col2.b; // Right Ear / Outer
                dynamicVibePattern = (len >= 13) ? (payload[11] & 0x0F) : 0x0B;
                receivedCommandType = 0x06;
                lastShowSyncTime = now;
                disneyDeviceFound = true;
                Serial.printf("   🎨 Mode: Dual Split Palette [Inner: %d (%s), Outer: %d (%s)] | Timing 0x%02X (AlwaysOn:%s, On:%.1fs, Fade:%.1fs)\n", 
                              innerColorIdx, DISNEY_PALETTE_NAMES[innerColorIdx], outerColorIdx, DISNEY_PALETTE_NAMES[outerColorIdx], payload[7], decodedAlwaysOn ? "YES" : "NO", decodedOnTimeSec, decodedFadeTimeSec);
            } 
            
            // CASE 3: Single Direct 6-bit RGB Color (E9 08) - Length >= 13
            else if (showCmd == 0x08 && len >= 13) {
                parseDisneyTimingByte(payload[7]);
                bool isStrobe = (payload[10] & 0x80) || (payload[11] & 0x80) || (payload[12] & 0x80);
                showR1 = (payload[10] & 0x7E) << 1;
                showG1 = (payload[11] & 0x7E) << 1;
                showB1 = (payload[12] & 0x7E) << 1;
                dynamicVibePattern = (len >= 14) ? (payload[13] & 0x0F) : 0x0B;
                receivedCommandType = isStrobe ? 0x0E : 0x08;
                lastShowSyncTime = now;
                disneyDeviceFound = true;
                Serial.printf("   🎨 Mode: Direct 6-bit RGB [%d, %d, %d] (Strobe: %s)\n", showR1, showG1, showB1, isStrobe ? "YES" : "NO");
            } 

            // CASE 3B: 5-Color Ring Palette (E9 09) - Length >= 14
            else if (showCmd == 0x09 && len >= 14) {
                parseDisneyTimingByte(payload[7]);
                uint8_t slots[5] = {
                    (uint8_t)(payload[9] & 0x1F),  // TL
                    (uint8_t)(payload[10] & 0x1F), // BL
                    (uint8_t)(payload[11] & 0x1F), // BR
                    (uint8_t)(payload[12] & 0x1F), // TR
                    (uint8_t)(payload[13] & 0x1F)  // Center
                };
                for (int s = 0; s < 5; s++) {
                    CRGB c = DISNEY_PALETTE[slots[s]];
                    showColors5[s][0] = c.r;
                    showColors5[s][1] = c.g;
                    showColors5[s][2] = c.b;
                }
                dynamicVibePattern = (len >= 15) ? (payload[14] & 0x0F) : 0x0B;
                receivedCommandType = 0x09;
                lastShowSyncTime = now;
                disneyDeviceFound = true;
                Serial.println("   🎨 Mode: 5-Color Palette Ring (0x09)");
            }
            
            // CASE 4: High-Contrast Circle (E9 0B)
            else if (showCmd == 0x0B) {
                showR1 = 0; showG1 = 180; showB1 = 255;  // Electric Blue
                showR2 = 255; showG2 = 255; showB2 = 255; // White
                dynamicVibePattern = 0x0A;
                receivedCommandType = 0x0B;
                lastShowSyncTime = now;
                disneyDeviceFound = true;
                Serial.println("   🎨 Mode: High-Contrast Blue & White Circle (0x0B)");
            }

            // CASE 4B: Official Park Show Microcode (E9 0C)
            else if (showCmd == 0x0C) {
                receivedCommandType = 0x0C;
                uint8_t lastByte = payload[len - 1];
                if (len >= 16 && payload[5] == 0xEF) {
                    dynamicVibePattern = 0x07; // Continuous rumble
                    showR1 = 255; showG1 = 90; showB1 = 0; // Orange Alert
                    Serial.println("   🎨 Mode: Park Show Microcode - Orange Alert Strobe");
                } else if (len >= 16 && lastByte == 0x95) {
                    dynamicVibePattern = 0x0A; // Sharp alert
                    showR1 = 255; showG1 = 255; showB1 = 255; // White Strobe
                    Serial.println("   🎨 Mode: Park Show Microcode - White Lightning Strobe");
                } else {
                    dynamicVibePattern = 0x01; // Quick tick
                    showR1 = 0; showG1 = 255; showB1 = 255; // Rainbow
                    Serial.println("   🎨 Mode: Park Show Microcode - Taste The Rainbow");
                }
                lastShowSyncTime = now;
                disneyDeviceFound = true;
            }

            // CASE 4C: Park Cross-Fade Series (E9 11)
            else if (showCmd == 0x11) {
                receivedCommandType = 0x11;
                dynamicVibePattern = 0x0B;
                lastShowSyncTime = now;
                disneyDeviceFound = true;
                Serial.println("   🎨 Mode: Park Cross-Fade Color Series (0x11)");
            }

            // CASE 5: Strobe Pulse (E9 0E) - Length >= 11
            else if (showCmd == 0x0E && len >= 11) {
                uint8_t colorIdx1 = payload[9] & 0x1F;
                CRGB col1 = DISNEY_PALETTE[colorIdx1];
                showR1 = col1.r; showG1 = col1.g; showB1 = col1.b;

                if (len >= 12) {
                    uint8_t colorIdx2 = payload[10] & 0x1F;
                    CRGB col2 = DISNEY_PALETTE[colorIdx2];
                    showR2 = col2.r; showG2 = col2.g; showB2 = col2.b;
                    dynamicVibePattern = (len >= 13) ? (payload[11] & 0x0F) : 0x0B;
                    Serial.printf("   🎨 Mode: Dual Strobe Pulse [C1: %d (%s), C2: %d (%s)] | Vibe: 0x%02X\n", 
                                  colorIdx1, DISNEY_PALETTE_NAMES[colorIdx1], colorIdx2, DISNEY_PALETTE_NAMES[colorIdx2], dynamicVibePattern);
                } else {
                    showR2 = 0; showG2 = 0; showB2 = 0; // Black / OFF on alternate flash
                    dynamicVibePattern = 0x0B;
                    Serial.printf("   🎨 Mode: Single Strobe Pulse [Idx: %d (%s)] | Vibe: 0x%02X\n", 
                                  colorIdx1, DISNEY_PALETTE_NAMES[colorIdx1], dynamicVibePattern);
                }
                receivedCommandType = 0x0E;
                lastShowSyncTime = now;
                disneyDeviceFound = true;
            }

            // CASE 6: Wave Pulse (E9 12) - Length >= 11
            else if (showCmd == 0x12 && len >= 11) {
                parseDisneyTimingByte(payload[7]);
                uint8_t innerColorIdx = payload[9] & 0x1F;
                uint8_t outerColorIdx = (len >= 13) ? (payload[11] & 0x1F) : innerColorIdx;
                CRGB col1 = DISNEY_PALETTE[innerColorIdx];
                CRGB col2 = DISNEY_PALETTE[outerColorIdx];
                showR1 = col1.r; showG1 = col1.g; showB1 = col1.b;
                showR2 = col2.r; showG2 = col2.g; showB2 = col2.b;
                dynamicVibePattern = (len >= 14) ? (payload[len - 1] & 0x0F) : 0x0B;
                receivedCommandType = 0x12;
                lastShowSyncTime = now;
                disneyDeviceFound = true;
                Serial.printf("   🎨 Mode: Wave Pulse [Idx1: %d (%s), Idx2: %d (%s)] | Timing 0x%02X (AlwaysOn:%s, On:%.1fs, Fade:%.1fs)\n", 
                              innerColorIdx, DISNEY_PALETTE_NAMES[innerColorIdx], outerColorIdx, DISNEY_PALETTE_NAMES[outerColorIdx],
                              payload[7], decodedAlwaysOn ? "YES" : "NO", decodedOnTimeSec, decodedFadeTimeSec);
            }

            // CASE 7: Unknown / Unhandled Show Packet Fallback
            else {
                showR1 = 255; showG1 = 215; showB1 = 0; // Gold
                dynamicVibePattern = 0x0B;
                receivedCommandType = 0x00;
                lastShowSyncTime = now;
                disneyDeviceFound = true;
                Serial.println("   🎨 Mode: Unknown Show Packet - Fallback (Gold)");
            }
        }
        // 3. DISNEY PARADE FLOAT BEACON (CD 07)
        else if (payload[4] == 0xCD && payload[5] == 0x07) {
            receivedCommandType = 0x07;
            dynamicVibePattern = 0x0A;
            lastShowSyncTime = now;
            disneyDeviceFound = true;
            Serial.println("   🎈 Mode: Disney Parade Float Beacon (CD 07)");
        }
    }
};

void scanCompleteCB(BLEScanResults scanResults) {
    isScanning = false;
}

// --- LIS3DSH Driver Helper Functions ---
uint8_t readLIS3DSHReg(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0x00;
    Wire.requestFrom(addr, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0x00;
}

void writeLIS3DSHReg(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

bool initLIS3DSH() {
    Wire.begin(SDA_PIN, SCL_PIN, 400000); // 400kHz I2C clock speed

    uint8_t addrs[2] = { LIS3DSH_ADDR1, LIS3DSH_ADDR2 };
    for (int i = 0; i < 2; i++) {
        uint8_t id = readLIS3DSHReg(addrs[i], LIS3DSH_WHO_AM_I);
        if (id == 0x3F) {
            lis3dshAddr = addrs[i];
            lis3dshFound = true;
            break;
        }
    }

    if (!lis3dshFound) {
        Serial.println("⚠️ [MOTION SENSOR] LIS3DSH not detected on I2C bus (probed 0x1D & 0x1E).");
        return false;
    }

    // CTRL_REG4 (0x20): 100Hz ODR + BDU enabled + X/Y/Z active (0x67)
    writeLIS3DSHReg(lis3dshAddr, LIS3DSH_CTRL_REG4, 0x67);
    // CTRL_REG5 (0x24): Full-scale range +/-2g (0x00)
    writeLIS3DSHReg(lis3dshAddr, LIS3DSH_CTRL_REG5, 0x00);

    Serial.printf("✅ [MOTION SENSOR] LIS3DSH initialized at I2C address 0x%02X! (WHO_AM_I: 0x3F)\n", lis3dshAddr);
    return true;
}

bool readLIS3DSH(float &accelX, float &accelY, float &accelZ) {
    if (!lis3dshFound) return false;

    Wire.beginTransmission(lis3dshAddr);
    Wire.write(LIS3DSH_OUT_X_L);
    if (Wire.endTransmission(false) != 0) return false;

    Wire.requestFrom(lis3dshAddr, (uint8_t)6);
    if (Wire.available() < 6) return false;

    int16_t rawX = (int16_t)(Wire.read() | (Wire.read() << 8));
    int16_t rawY = (int16_t)(Wire.read() | (Wire.read() << 8));
    int16_t rawZ = (int16_t)(Wire.read() | (Wire.read() << 8));

    accelX = rawX * 0.000061f; // Sensitivity for +/-2g scale is 0.061 mg/LSB
    accelY = rawY * 0.000061f;
    accelZ = rawZ * 0.000061f;

    return true;
}

void checkMotionTrigger() {
    if (!lis3dshFound) return;

    unsigned long now = millis();
    if (now - lastMotionTriggerTime < 1500) return; // 1.5s shake debounce cooldown

    float ax, ay, az;
    if (readLIS3DSH(ax, ay, az)) {
        float totalG = sqrt(ax * ax + ay * ay + az * az);

        // Motion Shake threshold: Total acceleration >= 2.2g
        if (totalG >= 2.2f) {
            lastMotionTriggerTime = now;
            animationState = (animationState + 1) % 8; // Advance local animation mode
            
            Serial.printf("💫 [MOTION SHAKE DETECTED!] Force: %.2fg | Switching to Animation Mode: %d\n", totalG, animationState);

            // Haptic tap feedback on shake
            digitalWrite(HAPTIC_PIN, HIGH);
            delay(60);
            digitalWrite(HAPTIC_PIN, LOW);
        }
    }
}

void setup() {
    pinMode(BUTTON_PIN, INPUT_PULLUP); 
    pinMode(HAPTIC_PIN, OUTPUT);       
    digitalWrite(HAPTIC_PIN, LOW); 

    // Non-blocking USB CDC Serial Initialization
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0); // Prevents USB CDC serial buffer from blocking execution
    delay(1000);

    Serial.println("\n==========================================");
    Serial.println("  ESP32-S3 EAR RECEIVER INITIALIZING...");
    Serial.println("==========================================");

    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_PIXELS);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 600); 
    FastLED.setBrightness(255);                     

    // Initialize LIS3DSH I2C Motion Sensor
    initLIS3DSH();

    BLEDevice::init("InteractiveWearable");
    pBLEScan = BLEDevice::getScan(); 
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);   
    pBLEScan->setInterval(40);  // 25ms interval (ultra-fast channel hopping)
    pBLEScan->setWindow(40);    // 25ms window (100% continuous RF duty cycle)

    FastLED.clear();
    FastLED.show();

    Serial.println("✅ BLE Scanner active and listening for Disney 0x0183 beacons!");
    Serial.println("==========================================\n");
}

void loop() {
    bool currentButtonState = digitalRead(BUTTON_PIN);
    unsigned long currentMillis = millis();

    // Check motion sensor shake triggers
    checkMotionTrigger();

    // Rapid 1-second scan cycles with zero gap (clears result cache each cycle
    // so the same transmitter MAC triggers onResult every time)
    if (!isScanning) {
        pBLEScan->clearResults();
        isScanning = true;
        pBLEScan->start(1, scanCompleteCB, false);
    }

    // Auto-timeout & fade-out manager for Zone Hold modes (0x05, 0x06, 0x08, 0x09)
    if (showSyncHold) {
        float timeoutSec = decodedAlwaysOn ? 10.0f : decodedOnTimeSec;
        unsigned long timeoutMs = (unsigned long)(timeoutSec * 1000.0f);
        if (currentMillis - holdStartTime >= timeoutMs) {
            if (decodedFadeTimeSec > 0.0f) {
                Serial.printf("[%lu ms] 🌅 Fading out over %.1f seconds...\n", currentMillis, decodedFadeTimeSec);
                int fadeSteps = (int)(decodedFadeTimeSec * 50.0f); // 50 updates/sec
                if (fadeSteps < 10) fadeSteps = 10;
                for (int s = fadeSteps; s >= 0; s--) {
                    uint8_t b = (uint8_t)((s * 255) / fadeSteps);
                    FastLED.setBrightness(b);
                    FastLED.show();
                    delay(20);
                }
            }
            showSyncHold = false;
            FastLED.clear();
            FastLED.setBrightness(255);
            FastLED.show();
            Serial.printf("[%lu ms] ⏱️ Zone Hold timing complete (On: %.1fs, Fade: %.1fs). LEDs turned OFF.\n", 
                          millis(), decodedOnTimeSec, decodedFadeTimeSec);
        }
    }

    // Intercept execution loop vector
    if (disneyDeviceFound) {
        executeDynamicShowSync();
    }

    // Manual Button Animation Cycling
    if (lastButtonState == HIGH && currentButtonState == LOW) {
        delay(50); // Simple debounce
        triggerHeavyDoubleTap();
        animationState = (animationState + 1) % 8;
        showSyncHold = false; // Always cancel show hold on manual button press
        FastLED.clear();
        FastLED.show();
        Serial.printf("[%lu ms] 🔘 Button Pressed! Switching to Animation State: %d\n", millis(), animationState);
    }
    lastButtonState = currentButtonState;

    runActiveAnimation();
    EVERY_N_MILLISECONDS(15) { gHue++; } 
    FastLED.show(); 

    // Periodic Serial Heartbeat
    static unsigned long lastHeartbeat = 0;
    if (currentMillis - lastHeartbeat > 4000) {
        lastHeartbeat = currentMillis;
        Serial.printf("[%lu ms] 🔍 [COM7 RX] Standing by | Active Anim State: %d | Scanning: %s\n", 
                      millis(), animationState, isScanning ? "YES" : "NO");
    }
}

// --- DISNEY OFFICIAL HAPTIC PATTERN ENGINE (0x0 to 0xF) ---
// Legend: ' = 0.125s (125ms), - = 0.25s (250ms), * = 0.5s (500ms), % = 1.0s (1000ms), # = 2.0s (2000ms)
void playDisneyVibrationPattern(uint8_t vibeCode) {
    uint8_t code = vibeCode & 0x0F;
    switch (code) {
        case 0x00: // 0x0 = No Vibration
        case 0x0C: // 0xC = No Vibration
        case 0x0D: // 0xD = No Vibration
        case 0x0E: // 0xE = No Vibration
        case 0x0F: // 0xF = No Vibration
            digitalWrite(HAPTIC_PIN, LOW);
            break;

        case 0x01: // 0x1 = - (0.25s Tap)
        case 0x09: // 0x9 = - (0.25s Tap)
            digitalWrite(HAPTIC_PIN, HIGH); delay(250); digitalWrite(HAPTIC_PIN, LOW);
            break;

        case 0x02: // 0x2 = -- (Two 0.25s Taps)
            for (int i = 0; i < 2; i++) {
                digitalWrite(HAPTIC_PIN, HIGH); delay(250); digitalWrite(HAPTIC_PIN, LOW);
                if (i < 1) delay(100);
            }
            break;

        case 0x03: // 0x3 = --- (Three 0.25s Taps)
            for (int i = 0; i < 3; i++) {
                digitalWrite(HAPTIC_PIN, HIGH); delay(250); digitalWrite(HAPTIC_PIN, LOW);
                if (i < 2) delay(100);
            }
            break;

        case 0x04: // 0x4 = --* (Two 0.25s Taps, One 0.5s Pulse)
            digitalWrite(HAPTIC_PIN, HIGH); delay(250); digitalWrite(HAPTIC_PIN, LOW); delay(100);
            digitalWrite(HAPTIC_PIN, HIGH); delay(250); digitalWrite(HAPTIC_PIN, LOW); delay(100);
            digitalWrite(HAPTIC_PIN, HIGH); delay(500); digitalWrite(HAPTIC_PIN, LOW);
            break;

        case 0x05: // 0x5 = ----*- (Four 0.25s Taps, One 0.5s Pulse, One 0.25s Tap)
            for (int i = 0; i < 4; i++) {
                digitalWrite(HAPTIC_PIN, HIGH); delay(250); digitalWrite(HAPTIC_PIN, LOW); delay(100);
            }
            digitalWrite(HAPTIC_PIN, HIGH); delay(500); digitalWrite(HAPTIC_PIN, LOW); delay(100);
            digitalWrite(HAPTIC_PIN, HIGH); delay(250); digitalWrite(HAPTIC_PIN, LOW);
            break;

        case 0x06: // 0x6 = ---***--- (Three 0.25s Taps, Three 0.5s Pulses, Three 0.25s Taps)
            for (int i = 0; i < 3; i++) {
                digitalWrite(HAPTIC_PIN, HIGH); delay(250); digitalWrite(HAPTIC_PIN, LOW); delay(100);
            }
            for (int i = 0; i < 3; i++) {
                digitalWrite(HAPTIC_PIN, HIGH); delay(500); digitalWrite(HAPTIC_PIN, LOW); delay(100);
            }
            for (int i = 0; i < 3; i++) {
                digitalWrite(HAPTIC_PIN, HIGH); delay(250); digitalWrite(HAPTIC_PIN, LOW);
                if (i < 2) delay(100);
            }
            break;

        case 0x07: // 0x7 = # (2.0s Heavy Rumble)
            digitalWrite(HAPTIC_PIN, HIGH); delay(2000); digitalWrite(HAPTIC_PIN, LOW);
            break;

        case 0x08: // 0x8 = '''''' (Six 0.125s Ticks)
            for (int i = 0; i < 6; i++) {
                digitalWrite(HAPTIC_PIN, HIGH); delay(125); digitalWrite(HAPTIC_PIN, LOW);
                if (i < 5) delay(50);
            }
            break;

        case 0x0A: // 0xA = * (0.5s Pulse)
            digitalWrite(HAPTIC_PIN, HIGH); delay(500); digitalWrite(HAPTIC_PIN, LOW);
            break;

        case 0x0B: // 0xB = % (1.0s Notification Pulse)
            digitalWrite(HAPTIC_PIN, HIGH); delay(1000); digitalWrite(HAPTIC_PIN, LOW);
            break;
    }
}

// --- PHYSICAL DYNAMIC SHOW-SYNC ENGINE ---
void executeDynamicShowSync() {
    Serial.printf("[%lu ms] 🚀 [COM7 RX] Executing Dynamic Show Sync Effect...\n", millis());

    CRGB activeShowColor1 = CRGB(showR1, showG1, showB1);
    CRGB activeShowColor2 = CRGB(showR2, showG2, showB2);

    // 1. Dynamic Haptic Decoder
    playDisneyVibrationPattern(dynamicVibePattern);
    
    // 2. Spatial Show LED Decoder
    if (receivedCommandType == 0x05) {
        // 🔮 SOLID SINGLE PALETTE HOLD: Keep color lit (do not clear)
        fill_solid(leds, NUM_PIXELS, activeShowColor1);
        FastLED.setBrightness(255);
        FastLED.show();
        // Leaves color on solid!
    } 
    else if (receivedCommandType == 0x06) {
        // 🎭 DUAL SPLIT PALETTE: Keep split colors lit on left vs right ears
        int half = NUM_PIXELS / 2;
        fill_solid(leds, half, activeShowColor1);        // Left Ear
        fill_solid(leds + half, half, activeShowColor2); // Right Ear
        FastLED.setBrightness(255);
        FastLED.show();
        // Leaves split colors on solid!
    } 
    else if (receivedCommandType == 0x08) {
        // 🌬️ DIRECT RGB: 3 cycles of smooth breathing glow (~4 seconds)
        for (int cycle = 0; cycle < 3; cycle++) {
            for (int b = 30; b <= 255; b += 10) {
                fill_solid(leds, NUM_PIXELS, activeShowColor1);
                FastLED.setBrightness(b);
                FastLED.show();
                delay(12);
            }
            for (int b = 255; b >= 30; b -= 10) {
                fill_solid(leds, NUM_PIXELS, activeShowColor1);
                FastLED.setBrightness(b);
                FastLED.show();
                delay(12);
            }
        }
        FastLED.setBrightness(255); 
    } 
    else if (receivedCommandType == 0x0B) {
        // ⚡ HIGH-CONTRAST CIRCLE: Solid Electric Blue outer ring with White center
        int half = NUM_PIXELS / 2; // 16 pixels per ear ring
        for (int i = 0; i < NUM_PIXELS; i++) {
            int posInEar = i % half;
            if (posInEar >= 4 && posInEar < 12) {
                leds[i] = activeShowColor1; // Electric Blue outer circle
            } else {
                leds[i] = activeShowColor2; // Pure White center
            }
        }
        FastLED.setBrightness(255);
        FastLED.show();
    }
    else if (receivedCommandType == 0x09) {
        // 🎨 5-COLOR PALETTE RING: Solid hold across 5 segments
        int segSize = NUM_PIXELS / 5;
        for (int s = 0; s < 5; s++) {
            CRGB c = CRGB(showColors5[s][0], showColors5[s][1], showColors5[s][2]);
            int startIdx = s * segSize;
            int count = (s == 4) ? (NUM_PIXELS - startIdx) : segSize;
            fill_solid(leds + startIdx, count, c);
        }
        FastLED.setBrightness(255);
        FastLED.show();
        // Leaves 5-color ring on solid!
    }
    else if (receivedCommandType == 0xC1) {
        // ✨ STARLIGHT BUBBLE WAND: 4 seconds of magical sparkle
        for (int frame = 0; frame < 40; frame++) {
            fill_solid(leds, NUM_PIXELS, activeShowColor1);
            int sparklePix = random(NUM_PIXELS);
            leds[sparklePix] = CRGB::White;
            int sparklePix2 = random(NUM_PIXELS);
            leds[sparklePix2] = CRGB::White;
            FastLED.show();
            delay(80);
        }
    }
    else if (receivedCommandType == 0xC4) {
        // 🗿 FAB 50 STATUE: 5 seconds of golden magical swirl
        CRGB goldCol = CRGB(255, 215, 0);
        for (int swirl = 0; swirl < 80; swirl++) {
            fadeToBlackBy(leds, NUM_PIXELS, 64);
            int p1 = swirl % NUM_PIXELS;
            int p2 = (swirl + (NUM_PIXELS / 2)) % NUM_PIXELS;
            leds[p1] = goldCol;
            leds[p2] = CRGB::White;
            FastLED.show();
            delay(60);
        }
    }
    else if (receivedCommandType == 0x0C) {
        // 🌈 PARK SHOW MICROCODE: Full 29s show spectrum, Orange Alert, or White Strobe
        if (showR1 == 255 && showG1 == 90 && showB1 == 0) {
            // Orange Alert
            for (int f = 0; f < 30; f++) {
                fill_solid(leds, NUM_PIXELS, CRGB(255, 90, 0));
                FastLED.show(); delay(80);
                fill_solid(leds, NUM_PIXELS, CRGB::Black);
                FastLED.show(); delay(80);
            }
        } else if (showR1 == 255 && showG1 == 255 && showB1 == 255) {
            // White Lightning Strobe
            for (int f = 0; f < 35; f++) {
                fill_solid(leds, NUM_PIXELS, CRGB::White);
                FastLED.setBrightness(255);
                FastLED.show(); delay(50);
                fill_solid(leds, NUM_PIXELS, CRGB::Black);
                FastLED.show(); delay(50);
            }
        } else {
            unsigned long startTime = millis();
            uint8_t hue = 0;
            while (millis() - startTime < 29000) {
                fill_rainbow(leds, NUM_PIXELS, hue, 7);
                FastLED.show();
                delay(15);
                hue += 2;
            }
        }
    }
    else if (receivedCommandType == 0x11) {
        // 🌊 PARK CROSS-FADE: 6 seconds of smooth color cross-fading
        CRGB colors[4] = { CRGB(0, 0, 255), CRGB(255, 180, 0), CRGB(255, 0, 0), CRGB(0, 255, 255) };
        for (int c = 0; c < 3; c++) {
            for (int step = 0; step <= 64; step++) {
                CRGB col = blend(colors[c], colors[c+1], step * 4);
                fill_solid(leds, NUM_PIXELS, col);
                FastLED.show();
                delay(30);
            }
        }
    }
    else if (receivedCommandType == 0x07) {
        // 🎈 DISNEY PARADE FLOAT BEACON: 4 seconds of pulsing float colors
        for (int p = 0; p < 12; p++) {
            fill_solid(leds, NUM_PIXELS, CRGB(255, 0, 255)); // Magenta
            FastLED.show(); delay(100);
            fill_solid(leds, NUM_PIXELS, CRGB(0, 255, 255)); // Cyan
            FastLED.show(); delay(100);
            fill_solid(leds, NUM_PIXELS, CRGB(255, 220, 0)); // Yellow
            FastLED.show(); delay(100);
        }
    }
    else if (receivedCommandType == 0x0E) {
        // ⚡ STROBE PULSE: 4 seconds of rapid alternating strobe
        CRGB primaryCol = (activeShowColor1 == CRGB(0, 0, 0)) ? CRGB(180, 0, 255) : activeShowColor1;
        CRGB secondaryCol = (activeShowColor2 == primaryCol || activeShowColor2 == CRGB(0, 0, 0)) 
                            ? CRGB::Black : activeShowColor2;
        for (int f = 0; f < 30; f++) {
            fill_solid(leds, NUM_PIXELS, primaryCol);
            FastLED.setBrightness(255);
            FastLED.show(); 
            delay(65);
            fill_solid(leds, NUM_PIXELS, secondaryCol);
            FastLED.show(); 
            delay(65);
        }
    }
    else if (receivedCommandType == 0x12) {
        // 🌊 WAVE PULSE: MagicBand+ animation replica (Dynamic timing byte sync)
        // Color 1 (Purple) base with Color 2 (Red) spinning ring & center pulse cutout
        int half = NUM_PIXELS / 2; // 16 pixels per ear
        unsigned long animStart = millis();
        unsigned long targetDurationMs = (unsigned long)(decodedOnTimeSec * 1000.0f);
        if (targetDurationMs < 2000) targetDurationMs = 14000; // Default safety fallback (14.0s)
        
        int step = 0;
        while (millis() - animStart < targetDurationMs) {
            int spinPos = step % half;
            
            // Pulse Beat (every 10 steps): Color 1 turns OFF while Color 2 flashes
            if (step % 10 == 0) {
                fill_solid(leds, NUM_PIXELS, CRGB::Black); // Color 1 (Purple) turns off
                // Center LEDs of each ear flash Color 2 (Red)
                for (int e = 0; e < 2; e++) {
                    int offset = e * half;
                    for (int c = 5; c < 11; c++) {
                        leds[offset + c] = activeShowColor2;
                    }
                }
                FastLED.show();
                delay(90);
            } else {
                // Normal frame: Color 1 base with Color 2 chaser
                fill_solid(leds, NUM_PIXELS, activeShowColor1);
                
                // Color 2 dot spinning around both ear rings
                leds[spinPos] = activeShowColor2;
                leds[(spinPos + 1) % half] = activeShowColor2;
                leds[half + spinPos] = activeShowColor2;
                leds[half + ((spinPos + 1) % half)] = activeShowColor2;
                
                FastLED.show();
                delay(35);
            }
            step++;
        }
    }
    else {
        fill_solid(leds, NUM_PIXELS, activeShowColor1);
        FastLED.show();
    }
    
    // Only hold colors for Zone & High-Contrast Circle modes (0x05, 0x06, 0x08, 0x09, 0x0B)
    if (receivedCommandType == 0x05 || receivedCommandType == 0x06 || 
        receivedCommandType == 0x08 || receivedCommandType == 0x09 ||
        receivedCommandType == 0x0B) {
        showSyncHold = true;
        holdStartTime = millis(); // Record start time for 10s auto-timeout
    } else {
        showSyncHold = false;
        FastLED.clear();
        FastLED.show();
    }
    
    lastShowSyncTime = millis(); // Refresh 3.5s debounce cooldown AFTER show animation finishes
    disneyDeviceFound = false; // Release intercept lock
}

void triggerHeavyDoubleTap() {
    digitalWrite(HAPTIC_PIN, HIGH); delay(120); digitalWrite(HAPTIC_PIN, LOW);  
    delay(100);                                     
    digitalWrite(HAPTIC_PIN, HIGH); delay(120); digitalWrite(HAPTIC_PIN, LOW);  
}

void runActiveAnimation() {
    switch (animationState) {
        case 1: showSyncHold = false; fill_rainbow(leds, NUM_PIXELS, gHue, 8); break;
        case 2: showSyncHold = false; runIgnitionPulse(); break;
        case 3: showSyncHold = false; runMeteorChase();   break;
        case 4: showSyncHold = false; runCyberPlasma();   break;
        case 5: showSyncHold = false; runLightningStorm();break; 
        case 6: showSyncHold = false; runNeonGlitch();    break; 
        case 7: showSyncHold = false; runHyperDrive();    break; 
        default: 
            if (!showSyncHold) {
                FastLED.clear(); 
            }
            break; 
    }
}

void runIgnitionPulse() {
    uint8_t pulseIntensity = beatsin8(45, 130, 255); 
    for (int i = 0; i < NUM_PIXELS; i++) {
        leds[i] = CHSV(beatsin8(18, 0, 24, 0, i * 6), 255, pulseIntensity);
    }
}

void runMeteorChase() {
    fadeToBlackBy(leds, NUM_PIXELS, 45); 
    leds[beatsin8(25, 0, NUM_PIXELS - 1)] = CRGB::Cyan; 
}

void runCyberPlasma() {
    uint8_t waveA = beatsin8(9, 0, 255); uint8_t waveB = beatsin8(14, 0, 255);
    for (int i = 0; i < NUM_PIXELS; i++) {
        leds[i] = CHSV((waveA + waveB + (i * 4)) / 2, 255, 255);
    }
}

void runLightningStorm() {
    fadeToBlackBy(leds, NUM_PIXELS, 90); 
    if (random8() < 12) { leds[random16(NUM_PIXELS)] = CRGB::AliceBlue; }
    if (random8() < 2)  { fill_solid(leds, NUM_PIXELS, CRGB::DeepSkyBlue); }
}

void runNeonGlitch() {
    fadeToBlackBy(leds, NUM_PIXELS, 75); 
    if (random8() < 60) {
        int pos = random16(NUM_PIXELS); uint8_t variant = random8(3); 
        if (variant == 0) leds[pos] = CRGB::Magenta;
        else if (variant == 1) leds[pos] = CRGB::Lime;
        else leds[pos] = CRGB::Turquoise;
    }
}

void runHyperDrive() {
    fadeToBlackBy(leds, NUM_PIXELS, 110); 
    uint8_t leadHead = beatsin8(120, 0, NUM_PIXELS - 1);
    leds[leadHead] = CHSV(gHue * 3, 255, 255); 
    leds[(leadHead + (NUM_PIXELS / 2)) % NUM_PIXELS] = CHSV((gHue * 3) + 128, 255, 255); 
}