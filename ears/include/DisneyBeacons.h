#ifndef DISNEY_BEACONS_H
#define DISNEY_BEACONS_H

#include <Arduino.h>
#include <FastLED.h>

// =========================================================================
// 1. DISNEY MANUFACTURER & BEACON CONSTANTS
// =========================================================================
#define DISNEY_COMPANY_ID_LE  0x0183 // Little-endian (0x83, 0x01)
#define DISNEY_COMPANY_ID_BE  0x8301 // Big-endian fallback
#define DISNEY_HEADER_SHOW    0xE9   // Standard Live Show / Zone Command

// =========================================================================
// 2. OFFICIAL DISNEY 5-BIT COLOR PALETTE (32 INDEXES - NEOPIXEL CALIBRATED)
// =========================================================================
const CRGB DISNEY_PALETTE[32] = {
    CRGB(80, 255, 255),   // 0x00 Cyan (Red channel boosted for NeoPixel balance)
    CRGB(180, 0, 255),    // 0x01 Purple
    CRGB(0, 0, 255),      // 0x02 Blue
    CRGB(0, 20, 120),     // 0x03 Midnight Blue
    CRGB(40, 120, 255),   // 0x04 Blue 2
    CRGB(200, 80, 255),   // 0x05 Bright Purple
    CRGB(200, 180, 255),  // 0x06 Lavender
    CRGB(120, 0, 255),    // 0x07 Deep Purple
    CRGB(255, 60, 180),   // 0x08 Pink
    CRGB(255, 70, 170),   // 0x09 Pink 2
    CRGB(255, 80, 160),   // 0x0A Pink 3
    CRGB(255, 90, 150),   // 0x0B Pink 4
    CRGB(255, 110, 150),  // 0x0C Pink 5
    CRGB(255, 130, 160),  // 0x0D Pink 6
    CRGB(255, 160, 170),  // 0x0E Pink 7
    CRGB(255, 180, 0),    // 0x0F Yellow Orange
    CRGB(255, 220, 0),    // 0x10 Off Yellow
    CRGB(255, 140, 20),   // 0x11 Yellow Orange 2
    CRGB(180, 255, 0),    // 0x12 Lime
    CRGB(255, 90, 0),     // 0x13 Orange
    CRGB(255, 40, 0),     // 0x14 Red Orange
    CRGB(255, 0, 0),      // 0x15 Red
    CRGB(60, 255, 255),   // 0x16 Cyan 2
    CRGB(40, 240, 255),   // 0x17 Cyan 3
    CRGB(20, 200, 255),   // 0x18 Cyan 4
    CRGB(0, 255, 0),      // 0x19 Green
    CRGB(80, 255, 40),    // 0x1A Lime Green
    CRGB(255, 200, 180),  // 0x1B White (Warm white)
    CRGB(255, 200, 180),  // 0x1C White 2
    CRGB(0, 0, 0),        // 0x1D Off (Black)
    CRGB(255, 140, 60),   // 0x1E Unique
    CRGB(255, 0, 255)     // 0x1F Magenta / Random
};

const char* const DISNEY_PALETTE_NAMES[32] = {
    "Cyan", "Purple", "Blue", "Midnight Blue",
    "Blue 2", "Bright Purple", "Lavender", "Deep Purple",
    "Pink", "Pink 2", "Pink 3", "Pink 4",
    "Pink 5", "Pink 6", "Pink 7", "Yellow Orange",
    "Off Yellow", "Yellow Orange 2", "Lime", "Orange",
    "Red Orange", "Red", "Cyan 2", "Cyan 3",
    "Cyan 4", "Green", "Lime Green", "White",
    "White 2", "Off", "Unique", "Magenta"
};

// =========================================================================
// 3. KNOWN DISNEY SHOW COMMAND STRUCT & LOOKUP TABLE
// =========================================================================
struct DisneyShowCommand {
    uint8_t cmdByte;          // e.g., 0x05, 0x06, 0x08, 0x09, 0x0B
    const char* name;         // Descriptive name
    const char* location;     // Park / Show reference
    uint8_t defaultVibe;      // Haptic vibration code
};

const DisneyShowCommand KNOWN_DISNEY_SHOWS[] = {
    { 0x05, "Single Palette Hold",  "Generic Zone / Ambient",     0x0B },
    { 0x06, "Dual Split Palette",   "Parade / Nighttime Show",    0x0B },
    { 0x08, "Direct 6-bit RGB",     "Fantasmic / Happily Ever",   0x07 },
    { 0x09, "5-Color Palette",      "5-LED Individual Ring",      0x0B },
    { 0x0B, "High-Contrast Circle", "Main Street / Entry Portal", 0x0A },
    { 0x0E, "Strobe Pulse",         "Coaster / Action Sequence",  0x01 },
    { 0x12, "Wave Pulse",           "World of Color / Water Show",0x09 },
    { 0xC1, "Starlight Wand Cast",  "Bubble Wand Magic Spell",    0x01 },
    { 0xC4, "Fab 50 Statue Beacon", "Magic Kingdom Hub Statues",  0x07 }
};

const size_t NUM_KNOWN_SHOWS = sizeof(KNOWN_DISNEY_SHOWS) / sizeof(DisneyShowCommand);

// Helper function to look up show metadata by command byte
inline const DisneyShowCommand* getDisneyShowInfo(uint8_t cmd) {
    for (size_t i = 0; i < NUM_KNOWN_SHOWS; i++) {
        if (KNOWN_DISNEY_SHOWS[i].cmdByte == cmd) {
            return &KNOWN_DISNEY_SHOWS[i];
        }
    }
    return nullptr;
}

#endif // DISNEY_BEACONS_H