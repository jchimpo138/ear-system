# Disney BLE System Protocol Reference

> **Overview:** Reverse-engineered and verified Bluetooth Low Energy (BLE) packet specification for Disney interactive wearables (MagicBand+, Interactive Ear Wearables, Starlight Bubble Wands, and Fab 50 Statues).

---

## Table of Contents

1. [BLE Advertisement Structure](#1-ble-advertisement-structure)
2. [Manufacturer Payload Architecture](#2-manufacturer-payload-architecture)
3. [Timing Byte Specification](#3-timing-byte-specification)
4. [Color Encoding & Palette Table](#4-color-encoding--palette-table)
   - [5-Bit Color Palette Table](#5-bit-color-palette-table-32-indexes)
   - [LED Position Masking](#led-position-masking-e9-05)
   - [6-Bit Packed Direct RGB Format](#6-bit-packed-direct-rgb-format-e9-08)
5. [Haptic Vibration Engine](#5-haptic-vibration-engine)
6. [Command Specifications](#6-command-specifications)
   - [E9 05 — Single Palette Color](#e9-05--single-palette-color)
   - [E9 06 — Dual Split Palette](#e9-06--dual-split-palette)
   - [E9 08 — Direct 6-bit Custom RGB](#e9-08--direct-6-bit-custom-rgb)
   - [E9 09 — 5-Color Ring Palette](#e9-09--5-color-ring-palette)
   - [E9 0B — High-Contrast Circle](#e9-0b--high-contrast-circle)
   - [E9 0C — Park Show Microcode](#e9-0c--park-show-microcode)
   - [E9 0E — Strobe Pulse](#e9-0e--strobe-pulse)
   - [E9 11 — Park Cross-Fade Series](#e9-11--park-cross-fade-series)
   - [E9 12 — Wave Pulse](#e9-12--wave-pulse)
   - [C4 10 / C4 15 — Fab 50 Statue Beacons](#c4-10--c4-15--fab-50-statue-beacons)
   - [CF 0B — Starlight Bubble Wand Cast](#cf-0b--starlight-bubble-wand-cast)
   - [CC 03 — Wake Ping](#cc-03--wake-ping)
   - [Direct E9/EA Park Infrastructure Opcodes](#direct-e9ea-park-infrastructure-opcodes)
7. [Custom Sub-Protocol (`AA 42`)](#7-custom-sub-protocol-aa-42)
8. [Transmission & Timing Strategy](#8-transmission--timing-strategy)
9. [Hardware Mapping](#9-hardware-mapping)

---

## 1. BLE Advertisement Structure

Disney interactive devices utilize **non-connectable BLE advertising frames** (GAP Advertising Flags `0x06`: LE General Discoverable Mode, BR/EDR Not Supported).

All show controls, beacon pings, and interactive commands are embedded in the **Manufacturer Specific Data** field (AD Type `0xFF`).

```
GAP Advertisement PDU
├── AD Length (1 byte)
├── AD Type (1 byte): 0xFF (Manufacturer Specific Data)
└── Manufacturer Data
    ├── Company ID (2 bytes, Little-Endian): 0x83 0x01 (Disney CID 0x0183)
    └── Payload Bytes (N bytes)
```

> **Note on Company ID:** In the raw BLE stream, the Disney Company Identifier `0x0183` is transmitted LSB first (`0x83 0x01`). On some BLE stack parsers, this is interpreted as `0x8301` depending on byte endianness. Both are supported in firmware.

---

## 2. Manufacturer Payload Architecture

Payload bytes follow the `0x83 0x01` Company ID. Disney protocols fall into three primary payload families:

```
Family 1: Guest & Show Commands (E1 / E2 Header)
[0x83] [0x01] [0xE1|0xE2] [0x00] [0xE9] [Cmd] ... [Timing] [Color(s)] [Vibe]

Family 2: Non-Show Beacons (Statues, Wands, Pings)
[0x83] [0x01] [0xC4|0xCF|0xCC] [SubCmd] ...

Family 3: Direct Park Infrastructure (Raw E9 / EA)
[0x83] [0x01] [0xE9|0xEA] [Opcode] ...
```

---

## 3. Timing Byte Specification

The **Timing Byte** controls dynamic effect duration, scaler multipliers, and fade-out behavior.

```
Bit Layout (8 bits):
 Bit 7        Bit 6        Bits 5–4       Bits 3–0
┌────────────┬────────────┬──────────────┬────────────────┐
│ ALWAYS_ON  │   SCALER   │  FADE_CODE   │    TIME_VAL    │
└────────────┴────────────┴──────────────┴────────────────┘
```

| Field | Bits | Description |
|---|---|---|
| **ALWAYS_ON** | 7 | `1` = Force permanent on/latch state (overrides timer). `0` = Timed hold. |
| **SCALER** | 6 | `0` = Standard scaler multiplier. `1` = Long-duration multiplier. |
| **FADE_CODE** | 5–4 | Fade-out duration upon expiry:<br>`00` = 0s (instant off)<br>`01` = 1s smooth fade<br>`10` = 2s smooth fade<br>`11` = 3s smooth fade |
| **TIME_VAL** | 3–0 | 4-bit integer duration multiplier (0–15). |

### Duration Formulas

$$\text{Duration (seconds)} = \begin{cases} 3.1 \times \text{TIME\_VAL} + 5.5 & \text{if SCALER} = 1 \\ 1.5 \times \text{TIME\_VAL} + 6.5 & \text{if SCALER} = 0 \end{cases}$$

---

## 4. Color Encoding & Palette Table

### 5-Bit Color Palette Table (32 Indexes)

Values are calibrated for WS2812B NeoPixels at standard brightness levels.

| Index | Hex | Name | RGB Color | Visual Profile |
|---|---|---|---|---|
| 0 | `0x00` | Cyan | `(80, 255, 255)` | Red-boosted Cyan |
| 1 | `0x01` | Purple | `(180, 0, 255)` | Deep Violet |
| 2 | `0x02` | Blue | `(0, 0, 255)` | Pure Blue |
| 3 | `0x03` | Midnight Blue | `(0, 20, 120)` | Deep Midnight |
| 4 | `0x04` | Blue 2 | `(40, 120, 255)` | Electric Azure |
| 5 | `0x05` | Bright Purple | `(200, 80, 255)` | Bright Magenta-Purple |
| 6 | `0x06` | Lavender | `(200, 180, 255)` | Soft Lavender |
| 7 | `0x07` | Deep Purple | `(120, 0, 255)` | Indigo Purple |
| 8 | `0x08` | Pink | `(255, 60, 180)` | Hot Pink |
| 9 | `0x09` | Pink 2 | `(255, 70, 170)` | Bubblegum Pink |
| 10 | `0x0A` | Pink 3 | `(255, 80, 160)` | Rose Pink |
| 11 | `0x0B` | Pink 4 | `(255, 90, 150)` | Coral Pink |
| 12 | `0x0C` | Pink 5 | `(255, 110, 150)` | Soft Rose |
| 13 | `0x0D` | Pink 6 | `(255, 130, 160)` | Blush Pink |
| 14 | `0x0E` | Pink 7 | `(255, 160, 170)` | Light Pink |
| 15 | `0x0F` | Yellow Orange | `(255, 180, 0)` | Amber Orange |
| 16 | `0x10` | Off Yellow | `(255, 220, 0)` | Warm Gold |
| 17 | `0x11` | Yellow Orange 2 | `(255, 140, 20)` | Deep Amber |
| 18 | `0x12` | Lime | `(180, 255, 0)` | Chartreuse Lime |
| 19 | `0x13` | Orange | `(255, 90, 0)` | Bright Orange |
| 20 | `0x14` | Red Orange | `(255, 40, 0)` | Flame Orange |
| 21 | `0x15` | Red | `(255, 0, 0)` | Pure Red |
| 22 | `0x16` | Cyan 2 | `(60, 255, 255)` | Light Cyan |
| 23 | `0x17` | Cyan 3 | `(40, 240, 255)` | Sky Cyan |
| 24 | `0x18` | Cyan 4 | `(20, 200, 255)` | Ice Blue |
| 25 | `0x19` | Green | `(0, 255, 0)` | Pure Green |
| 26 | `0x1A` | Lime Green | `(80, 255, 40)` | Bright Lime Green |
| 27 | `0x1B` | White | `(255, 200, 180)` | Warm White |
| 28 | `0x1C` | White 2 | `(255, 200, 180)` | Pure White |
| 29 | `0x1D` | Off | `(0, 0, 0)` | Off (Black) |
| 30 | `0x1E` | Unique | `(255, 140, 60)` | Terracotta |
| 31 | `0x1F` | Magenta / Random | `(255, 0, 255)` | Pure Magenta |

---

### LED Position Masking (E9 05)

In `E9 05` Single Palette commands, the color byte packs a 3-bit LED location mask in bits `[7:5]` and the 5-bit palette index in bits `[4:0]`:

`ColorByte = ((Mask & 0x07) << 5) | (PaletteIdx & 0x1F)`

| Mask Bitfield | Binary | Selected LED Position |
|---|---|---|
| `0` | `0b000` | All LEDs |
| `1` | `0b001` | Top-Right LED |
| `2` | `0b010` | Bottom-Right LED |
| `3` | `0b011` | Bottom-Left LED |
| `4` | `0b100` | Top-Left LED |
| `5–7` | `0b101–111` | All LEDs (Fallback) |

---

### 6-Bit Packed Direct RGB Format (E9 08)

Direct RGB commands transmit 6-bit color channels (0–63) packed into 7-bit safe byte representations:

- **Encoding:** `Byte = (Channel_6Bit & 0x3F) << 1`
- **Decoding:** `Channel_8Bit = (Byte & 0x7E) << 1`

---

## 5. Haptic Vibration Engine

Vibration codes are contained in the lower nibble of the vibration control byte (`0xB0 | Code` or `(Code << 4) | 0x0B`).

| Code | Hex | Pattern Name | Timing / Pulse Profile |
|---|---|---|---|
| 0 | `0x0` | None | Off |
| 1 | `0x1` | Tap | 1 × 250ms tap |
| 2 | `0x2` | Double Tap | 2 × 250ms taps (100ms gap) |
| 3 | `0x3` | Triple Tap | 3 × 250ms taps (100ms gap) |
| 4 | `0x4` | Pulse Combo | 2 × 250ms taps + 1 × 500ms pulse |
| 5 | `0x5` | Fast Pattern | 4 × 250ms taps + 1 × 500ms + 1 × 250ms tap |
| 6 | `0x6` | Complex SOS | 3 × 250ms + 3 × 500ms + 3 × 250ms |
| 7 | `0x7` | Heavy Rumble | Continuous 2.0s heavy vibration |
| 8 | `0x8` | Ticks | 6 × 125ms rapid ticks (50ms gap) |
| 9 | `0x9` | Single Tap | 1 × 250ms tap (alias of 0x1) |
| 10 | `0xA` | Sharp Pulse | 1 × 500ms medium pulse |
| 11 | `0xB` | Notification Pulse | 1 × 1000ms long pulse |
| 12–15 | `0xC–0xF` | Reserved / None | Off |

---

## 6. Command Specifications

Byte layouts below reflect the **Manufacturer Data payload** (excluding the 2-byte `0x83 0x01` CID).

---

### E9 05 — Single Palette Color

```
Offset:  [0]  [1]  [2]  [3]  [4]    [5]    [6]      [7]       [8]
Byte:    E1   00   E9   05   00   Timing   0E   ColorByte  VibeByte

Details:
- ColorByte: (Mask << 5) | (PaletteIdx & 0x1F)
- VibeByte:  0xB0 | (VibeCode & 0x0F)
```

---

### E9 06 — Dual Split Palette

```
Offset:  [0]  [1]  [2]  [3]  [4]    [5]    [6]       [7]          [8]        [9]
Byte:    E2   00   E9   06   00   Timing   0F   (0x40|Inner) (0x40|Outer) VibeByte

Details:
- Inner/Left Byte:  0x40 | (InnerIdx & 0x1F)
- Outer/Right Byte: 0x40 | (OuterIdx & 0x1F)
- VibeByte:         0xB0 | (VibeCode & 0x0F)
```

---

### E9 08 — Direct 6-bit Custom RGB

```
Offset:  [0]  [1]  [2]  [3]  [4]    [5]    [6]   [7]    [8]      [9]      [10]     [11]
Byte:    E1   00   E9   08   00   Timing   D2    55   RedByte GreenByte BlueByte VibeByte

Details:
- RedByte:   (R_6bit & 0x3F) << 1
- GreenByte: (G_6bit & 0x3F) << 1
- BlueByte:  (B_6bit & 0x3F) << 1
```

---

### E9 09 — 5-Color Ring Palette

```
Offset:  [0]  [1]  [2]  [3]  [4]    [5]    [6]    [7]    [8]    [9]    [10]   [11]   [12]
Byte:    E1   00   E9   09   00   Timing   0F     TL     BL     BR     TR     C   VibeByte

Details:
- TL (Top-Left):     0xA0 | (Idx & 0x1F)
- BL (Bottom-Left):  0xA0 | (Idx & 0x1F)
- BR (Bottom-Right): 0xA0 | (Idx & 0x1F)
- TR (Top-Right):    0xA0 | (Idx & 0x1F)
- C  (Center):       0xA0 | (Idx & 0x1F)
```

---

### E9 0B — High-Contrast Circle

```
Full Payload: E1 00 E9 0B 0B 0F 0F 5C 42 5C A2 DC 42 32 05

Details:
- Renders Electric Blue outer ring with White center fill.
```

---

### E9 0C — Park Show Microcode

```
Blink White (Lightning Strobe):
Payload: E1 00 E9 0C 00 0F 0F 5D 46 5B F0 05 32 37 48 95

Taste the Rainbow:
Payload: E1 00 E9 0C 00 0F 0F 5D 46 5B F0 05 32 37 48 B0

Orange Alert Strobe:
Payload: E1 00 E9 0C 00 EF 0F 4F 4F 5B F0 FB 14 37 48 95
(Timing 0xEF has Always-On bit set; runs continuously)
```

---

### E9 0E — Strobe Pulse

```
Offset:  [0]  [1]  [2]  [3]  [4]    [5]    [6]       [7]          [8]        [9]
Byte:    E1   00   E9   0E   00   Timing   0F   (0x40|Color1) (0x40|Color2) VibeByte
```

---

### E9 11 — Park Cross-Fade Series

```
Example (Cyan to Pink):
Payload: E1 00 E9 11 00 6F 0F 56 48 58 F4 48 82 D1 46 02 08 D0 65 00 B0
```

---

### E9 12 — Wave Pulse

```
Offset:  [0]  [1]  [2]  [3]  [4]    [5]    [6]       [7]          [8]        [9]
Byte:    E2   00   E9   12   00   Timing   0F   (0x40|Inner) (0x40|Outer) VibeByte
```

---

### C4 10 / C4 15 — Fab 50 Statue Beacons

Emitted by Fab 50 Golden Statues in Disney Parks.

```
Layout (18-byte C4 10 variant):
[0] 0xC4
[1] 0x10
[2..14] Padding / Metadata
[15..16] ASCII 2-digit Statue ID (e.g. "53" = Mickey Mouse)
[17] 0x00
```

---

### CF 0B — Starlight Bubble Wand Cast

Emitted by Disney Starlight Bubble Wands when casting spells.

```
Layout (13 bytes):
[0..5]   Fixed Signature: CF 0B 00 C4 20 22
[6..11]  Rolling Code (Anti-replay security)
[12]     Color Byte: 5-bit Palette Index (bits [4:0])
```

---

### CC 03 — Wake Ping

Sent prior to primary payload bursts to wake receiver radios from low-power scan sleep.

```
Payload: CC 03 00 00 00
Duration: 500 ms @ 25 ms interval
```

---

### Direct E9/EA Park Infrastructure Opcodes

Captured from Walt Disney World park infrastructure (Spaceship Earth, HarmonioUS, etc.):

- **`E9 04`**: Park Show Sync Base
- **`E9 08`** *(Short Form Direct 5-Slot)*: `E9 08 ... 0F [C0] [C1] [C2] [C3] [C4]`
- **`E9 10`**, **`E9 13`**, **`EA 14`**: Long-format park show sequences (contains `f4 48 82` signature).

---

## 7. Custom Sub-Protocol (`AA 42`)

Custom control messages for receiver unit configuration:

| Command Payload | Function |
|---|---|
| `AA 42 01` | **Ears Battery Status:** Displays receiver battery level on LED jewels. |
| `AA 42 03` | **Ears Brightness Cycle:** Toggles brightness (Dim / Medium / Bright). |
| `AA 42 04` | **Find Me Stroller Beacon:** 30s high-visibility stroller locator animation. |
| `AA 42 05` | **Ears Statue Preview:** Triggers golden statue swirl animation on demand. |

---

## 8. Transmission & Timing Strategy

To ensure 100% latch reliability across hardware:

1. **Wake Phase:** 500ms burst of `CC 03 00 00 00` at 25ms interval.
2. **Payload Phase:** 3000ms burst of primary command payload at 25ms interval.
3. **Receiver Cooldown:** 3.5s local debounce gate to prevent repeated trigger on a single burst.
4. **Scan Cycle:** Continuous 1-second BLE scans with 100% RF duty cycle (25ms window / 25ms interval).

---

## 9. Hardware Mapping

| Device | Board | Role | Primary Pins / Output |
|---|---|---|---|
| **Transmitter** | ESP32-S3 DevKitC-1 | BLE Advertiser (`DisneyBeaconTX`) | USB CDC Serial (`COM8`) |
| **Ear Receiver** | ESP32-S3 DevKitC-1 | BLE Scanner + Driver (`InteractiveWearable`) | GP2 (WS2812B NeoPixels, 32 LEDs)<br>GP4 (Button, INPUT_PULLUP)<br>GP5 (Haptic Motor Driver) |
