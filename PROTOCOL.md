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
Offset:  [0]  [1]  [2]  [3]  [4]    [5]    [6]    [7]      [8]    [9]    [10]   [11]   [12]
Byte:    E1   00   E9   09   00   Timing   0F   Center     NE     SE     SW     NW   VibeByte

Details: each color byte = 0xA0 | (PaletteIdx & 0x1F)
```

⚠️ **Order corrected from an earlier community-sourced "TL, BL, BR, TR, Center" labeling** (from `research/emcot.txt`, also copied uncritically into the Adafruit reference project's `build_five_color()`) — bench-confirmed against a real MagicBand+ via the same one-at-a-time isolation testing used for `E9 10`: the actual order is **Center, NE, SE, SW, NW**, identical to `E9 10`'s confirmed order. Neither emcot.txt nor Adafruit had actually tested this ordering against real hardware; both just repeated the same unverified label. This is a good example of why [[protocol_md_confidence_level]] applies even to details other community references agree on — agreement between two unverified sources isn't verification.

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

Captured from Walt Disney World park infrastructure (Spaceship Earth, HarmonioUS, etc.). Unlike the guest/wearable "Family 1" layout above, these broadcast `E9`/`EA` directly as manufacturer-data byte 2 (right after `83 01`), with no `[E1|E2][00]` wrapper in front — so the header sits 2 bytes earlier than Family 1's offset. Bench-confirmed live against a real MagicBand+ (not just captured-and-replayed) via a crafted-payload test rig, in a session that also independently re-derived the Family 1 `Timing Byte` formula's correctness against this same real hardware.

- **`E9 04`**: Park Show Sync Base
- **`E9 08`** *(Short Form Direct 5-Slot)*: `E9 08 ... 0F [C0] [C1] [C2] [C3] [C4]`
- **`EA 14`**: Long-format park show sequence sharing the same `F4 48 82` byte sequence noted below for `E9 10` — not decoded. One real captured example tested live (`research/BLE_Beacon_Ears`'s "white sparkling" capture) produced **no reaction at all** on a real MagicBand+, unlike every `E9 10`/`E9 13` example tested — possibly a different target device class than a standard band, or a stale/replay-sensitive capture, not confirmed either way.

#### `E9 10` — "Alternating Colors" (bench-confirmed layout)

```
Offset:  [0]  [1]  [2]     [3]        [4]      [5]      [6]      [7..11]        [12]        [13..21]
Byte:    83   01   E9      10         00       Timing   0F       5×ColorByte    PatternID   (unmapped)
```

- **Timing Byte** (offset 5): same encoding/formula as Family 1's Timing Byte (see section 3) — confirmed against a real band holding a `0x0F` byte for exactly the predicted 29.0s (`1.5×15+6.5`).
- **5×ColorByte** (offset 7–11): five consecutive bytes, each independently addressing one of the real band's 5 physical LEDs, in this order: **Center, NE (top-right), SE (bottom-right), SW (bottom-left), NW (top-left)**. Each byte packs `(Mode << 5) | PaletteIndex` — same two-field split documented above for `E9 05`'s LED-position mask, just with a different meaning in the top 3 bits here. Confirmed one-at-a-time by toggling each byte to `Off` (palette index 29) and back while holding the other four fixed, watching the corresponding physical LED turn dark and back on.
  - **`E9 09` uses this exact same order** (Center, NE, SE, SW, NW) — bench-confirmed by the same one-at-a-time isolation method (see `E9 09`'s section above). The two opcodes turned out to share a byte-order convention after all, once `E9 09`'s previously-uncorroborated documented order was actually tested against real hardware.
  - ⚠️ Palette index `31` (`0x1F`) is documented in `research/emcot.txt` line 349 as `11111b = random` — i.e. not a real color, a "pick something random" sentinel. Confirmed live: a slot set to index 31 visibly switches between two different colors ~1s after lighting, with no repeatable second color across retests. Avoid this index in any deterministic test payload.
  - **Byte 7's own `Mode` field (top 3 bits) selects between two entirely different rendering families** — this matters more than it looks like a throwaway detail:
    - **`Mode` 5, 6, or 7** ("rainbow family", confirmed identical to each other, mirroring `E9 05`'s own documented high-value fallback design): all 5 `ColorByte`s render independently and simultaneously. Whether they're static or animated is controlled by `PatternID` (below).
    - **`Mode` 0–4** (includes the real captured default's own mode, `2` — likely the most common real-world case): behaves completely differently from the rainbow family — a **two-parameter background+chaser model, not a 5-independent-LED one**: **Center's color fills the strip, and NE's color sweeps around it as a moving ~1/5-strip chaser.** SE/SW/NW's bytes are decoded but don't visibly affect this render path. This was initially miscategorized as "only Center matters, chaser is always a dark gap" based solely on `E9 10`'s real captured default — which happens to have **NE = Off**, making the chaser read as darkness rather than a color. Cross-testing against `E9 13` (whose real captures have non-Off NE values) revealed the chaser is actually NE's color, not a fixed dark gap — `E9 10`'s "dark gap" appearance was a coincidence of that one example's specific NE value, not the real mechanism. Confirmed live: changing `E9 13`'s NE byte from Blue 2 to Green changed the chaser from (perceived) white/pale to a clearly-green chaser on the real band. The exact trigger condition for *when* this animates (vs. renders static) isn't confirmed — firmware defaults to animating for this mode range since that's what every real-world example of it has done.
- **`PatternID`** (offset 12): a small enumerated animation-ID byte, not a clean bitfield — tested a community-sourced "high-nibble selects pattern" theory (`research/emcot.txt` line 218: `3 = Palette B Spin`) and it does not hold (`0x35`/`0x3A`/`0x3F` all share high-nibble `3` with the confirmed-spin `0x30`/`0x31` but render static). Confirmed values (**rainbow family only, `Mode` 5-7** — `PatternID`'s effect on `Mode` 0-4 is unconfirmed, see above):
  - `0x82` — the real captured default's own `PatternID` value — **static** under the rainbow family, each of the 5 LEDs shows its own decoded color, no motion.
  - `0x30` / `0x31` — **rotating chase**: the 5 assigned colors visit the 5 LED positions in sequence (Center→NE→SE→SW→NW order, confirmed by watching two adjacent colors "chase" each other around the ring). The two values differ only in the low bit and produce opposite rotation directions.
  - Every other tested value (`0x00, 0x10, 0x20, 0x32-0x35, 0x3A, 0x3F, 0x40, 0x50, 0x60, 0x70, 0x90`) renders static under the rainbow family, same as `0x82`.
- Real captured example (`research/emcot.txt` line 253, unwrapped): `E9 10 00 0F 0F 54 5D 58 F4 48 82 D1 46 09 0A D0 65 28 21 02` — decodes to Center=Red Orange (`Mode`=2, so the "`Mode` 0-4" render path applies, not the rainbow family), NE=Off, SE=Cyan 4, SW=Red Orange (with a mismatched `Mode`=7 on this one byte only), NW=Pink, `PatternID`=`0x82`, Timing=`0x0F`→29.0s. On real hardware this renders as the strip mostly lit Red Orange with a dark gap sweeping around — bench-confirmed matching between the real band and the ears firmware's implementation of this case.
- Offsets 13–21 (`D1 46 09 0A D0 65 28 21 02` in the example above): individually toggled to `Off` one at a time with no observed effect on any LED or on vibration — purpose still unknown.
- The wrapped variant of this opcode (`E1 00 E9 10 00 13 48 97 D0 0E A0 D1 46 06 0F 30 D0 4E 07 B0`, also the only `E9 10` example in the `research/flipper` Magic Band Plus Lights app, itself sourced from the same `emcot.world` community page) does **not** follow this byte layout — its byte 4 (where the `0F` marker sits in every other confirmed E9 command) is `0x48`, not `0x0F`, so it's a genuinely different sub-format, not decoded.
- Vibration is **not** payload-byte-controlled for `E9 10` — confirmed by grafting `E9 12`'s trailing bytes (which do vibrate) onto an `E9 10` packet with the opcode byte still `10`: no vibration. Changing only the opcode byte from `10`→`12` (same trailing bytes) restored vibration — meaning vibration is gated by the opcode itself (`E9 12` is literally named "Circle With Vibration" in the community notes), not by any specific data byte.

#### `E9 13` (unwrapped) — shares `E9 10`'s byte layout and Timing Byte formula

Bench-confirmed against a real MagicBand+: the same `[Center, NE, SE, SW, NW]` `(Mode<<5)|PaletteIndex` color layout and Timing Byte formula (offset 3, same as `E9 10`) apply here too — the model generalizes across opcodes, not opcode-specific. Real captured example (`research/BLE_Beacon_Ears`'s "orange red sparkle" capture): `E9 13 00 B6 0F 40 44 58 F4 48 82 D0 65 19 D1 46 06 0A 30 7B FF` — Timing Byte `0xB6` has `ALWAYS_ON=1`, so it never expires.

- Duration formula confirmed correct for `TIME_VAL≥1` (matched predicted vs. observed within ~1s across two different values). `TIME_VAL=0` is a real anomaly: predicted 6.5s, actual ~10s — likely a firmware-enforced minimum floor or `0` treated as a "use default" sentinel rather than the literal formula value.
- `FADE_CODE` has **no observable effect** on this animation, tested across its full range (`00`/`01`/`11`) — always an identical hard cutoff, never a visible fade. The documented fade-out table (section 3) may not apply uniformly to every command.
- The animation consistently reserves a fixed **~4.5–5s "hold last frame, then hard-cut" tail** at the end of the show, regardless of total duration — confirmed across four different total durations (6.5–30.3s predicted range). The visible motion itself scales with the Timing-Byte-commanded total (`motion_duration ≈ total_duration − ~4.5s`), so the overall show length is genuinely Timing-Byte-controlled; it's specifically this tail behavior that's fixed and indifferent to `FADE_CODE`.
- This example's `Mode` (2, from `Center`'s byte `0x40`) uses the same background+chaser render as `E9 10`'s `Mode` 0-4 — see that section above. Unlike `E9 10`'s real default (whose NE happens to be Off), this capture's NE is a real color (Blue 2), which is what revealed that NE controls the chaser's color rather than the chaser always being a dark gap.

#### `E9 14` (wrapped) — same Center/NE two-color model as E9 10/13, but a fast flicker instead of a steady chaser

Bench-confirmed against a real MagicBand+, in the standard **wrapped** layout (`[E1/E2][00][E9][14][00][Timing][0F][Center][NE]...`, Timing at offset 7, colors starting at offset 9 — same convention as `E9 05/06/08/09`), not the unwrapped Family 3 layout `E9 10`/`E9 13` use. Real captured example: `E2 00 E9 14 00 42 0F 55 5B 58 F4 48 82 D0 65 1B D1 46 2A 02 30 7B 5D B0` → Center=Red, NE=White.

- Same underlying two-color model as `E9 10`/`E9 13`'s `Mode` 0-4 (confirmed via isolation: setting all colors but NE to Off still showed nothing until NE itself had a real value) — but the animation is **much faster and less predictable** than their steady sweeping chaser, closer to a fast random flicker between the two colors than a moving gap. Implemented as a fast (~90ms) 5-zone random flicker between Center and NE rather than reusing the sweep renderer, which looked visibly wrong for this command.
- Other real captures of this opcode seen (`E1 00 E9 14 00 0C D0 37 F0 D2 3D 05 0C 0C 0E EC 89 83 51 0E EE 0C 3D B0`, described as "pink pulse, hard cutoff, fades in") do **not** match this layout at all — byte 6 isn't the expected `0F` marker, so like the wrapped `E9 10`/`E9 13` examples, this is a separate undecoded sub-format, not a contradiction of the model above.

#### Non-standard wrapped "pulse" sub-format (seen on both `E9 13` and `E9 14`)

A second wrapped sub-format, distinct from both the standard wrapped layout above and the unwrapped Family 3 layout — identifiable by a `D0 37 F0 D2 3D`-ish byte run starting at offset 6 (where the standard `0x0F` marker would be) instead of the marker itself. Seen on real captures of both `E9 13` (`E1 00 E9 13 00 [TimingByte] D0 37 F0 D2 3D 05 05 00 0E FA 89 83 51 0E E7 A0 B0`) and `E9 14` (`E1 00 E9 14 00 0C D0 37 F0 D2 3D 05 0C 0C 0E EC 89 83 51 0E EE 0C 3D B0`), suggesting it's a shared alternate encoding, not opcode-specific. Most of this structure remains undecoded, but **offset 7 (the same position as the standard Timing Byte) is confirmed live to still control both duration and a visible pulse count**, just via a different formula than the standard one:

- **`pulse_count = 2 × TIME_VAL`** (low nibble of the offset-7 byte) — confirmed exactly across 6 tested values (`TIME_VAL` = 1, 2, 3, 4, 6, 8 → 2, 4, 6, 8, 12, 16 pulses), zero deviation.
- **`duration ≈ 1.5 × TIME_VAL`** — same slope as the standard formula's scaler=0 case, but with **no `+6.5` constant offset** (every other confirmed command has one). Confirmed against 3 timed values: `TIME_VAL=2`→~3s (predicted 3.0), `TIME_VAL=4`→~7s (predicted 6.0), `TIME_VAL=6`→9.78s (predicted 9.0) — small residual differences likely just measurement imprecision on the rougher readings.
- **Offset 9 is a pattern selector**: `0x37`/`0x38` produce the pulse behavior, `0x31` produces a fast sparkle instead. Exact boundary/full range not mapped.
- **Offset 12 affects fade amount/speed** — confirmed by varying across `0x3A`-`0x3F`, each value visibly different. Exact formula not characterized.
- **Offset 20 is a real color byte**, same `(Mode<<5)|PaletteIndex` formula as every other confirmed command. Found by comparing three real captured examples of this sub-format byte-by-byte for ones that varied uniquely across all three (`E9 13`→`0xE7`/Deep Purple, `E9 14` pink→`0xEE`/Pink 7, `E9 14` blue→`0xE3`/Midnight Blue — two of three matched the reported color well), then confirmed with a live isolated test: changing just this byte to Green (`0xF9`) turned the display green.
- **Offset 19 has a real effect, but doesn't look like a normal color/pattern parameter.** Its value in every real capture is a constant `0x0E`. Live testing (idx20 held fixed at White in both): `0x0E` (the natural value) shows White, matching idx20 correctly; `0x0D` — a one-decrement, not a color-scale change — shows a completely different color (green) instead of a blend or shift. That behavior looks more like a narrow validity check than a parameter: at its expected value it lets idx20's color render normally, and off that value it seems to trigger some other (possibly hardcoded) fallback — echoing the same "only specific values are recognized" pattern found for the unwrapped format's `PatternID` byte (`0x30`/`0x31` only). Not fully mapped — only two adjacent values tested.
- A second color (described as "blue or purple") appears alongside idx20's color in some tests; the offset responsible for it is not yet confirmed. Offset 11 (`0x05`→Bright Purple in the `E9 13`/`E9 14`-pink captures, `0x02`→Blue in the `E9 14`-blue capture) is a plausible candidate by the same real-capture-comparison method used to find offset 20, and a live isolated test was built for it, but the result was never reported/confirmed — treat as an untested hypothesis, not a finding.
- Offsets 6-8, 10, 13-18 tested (individually or in batches) with no observed visual effect so far — likely padding, checksum, or a rolling/anti-replay code, not color or pattern data.

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
