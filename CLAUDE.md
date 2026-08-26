# Smart BLE Disney Ears — Project Doc

Wearable BLE-controlled LED Mickey ears headband, built around a Seeed XIAO nRF52840 Sense. Detachable ears connect via 6-pin magnetic pogo connectors.

---

## Power Architecture

**Topology:** split-rail — XIAO runs off raw battery via the BMS's P+/P− output; a separate 5V boost feeds the LED rail.

**Battery:** single 18650 cell, permanently installed (not swappable).

**BMS:** Teyleten 1S 15A protection board (DW01+8205A family, strip form factor, 35×7.6×2mm). Continuity-tested: P+ = B+ directly, low-side switching only.

**FET matrix:**

| FET | Role | Status |
|---|---|---|
| FET0 | Was XIAO rail hard switch | **Eliminated** — nRF52840 System OFF sleep (~1–5µA) is negligible vs. Li-ion self-discharge; firmware deep sleep used instead |
| FET1 | Firmware-controlled LED gate | **Bench-verified** — SSM3J328R P-FET, gate driven via S8050 NPN level-shifter from XIAO GPIO. Direct 3.3V GPIO drive against the 5V source rail only reaches ~−1.8V VGS — insufficient for guaranteed hard-off, hence the level-shifter is required, not optional. Confirmed on hardware: rail cleanly switches ~5.15V/~0V under firmware control, integrated into production rail-gating logic in `ears/src/main.cpp`. |
| FET2 | Per-ear passive presence gate | **Bench-verified** — source fed from FET1's *drain* (downstream of the firmware-gated LED rail, not straight off the battery), so it only ever sees voltage when FET1 is already on. Gate held off by default; pulled on only when the ear's SENSE pin is grounded via a jumper on the passive ear PCB — fully passive, no GPIO/firmware involvement at all. Confirmed on hardware across all 4 combinations of FET1 on/off × ear attached/detached: ear power only present when both conditions are true. |
| FET4 | Reverse-polarity protection | **Removed for first prototype** (accepted risk, given permanent cell install). If reintroduced later, must sit between the raw battery terminal and the shared B+/P+ node — not downstream of the BMS. |

**FET1 level-shift stage values:** 10kΩ gate pull-up to 5V rail, 1kΩ S8050 base resistor, S8050 emitter to GND.

**FET2 gate pull-up:** 100kΩ, gate to source (FET1's drain rail) — held off by default at ~50µA idle draw; the ear's GND-jumpered SENSE pin pulls the gate down to turn it on when attached. Switching speed is irrelevant here (only transitions on a physical plug/unplug event), so the high value trades speed for minimal idle current.

**LED rail control:** IP5310 boost module, KEY button (single tap on/double tap off) as physical LED rail control. 2.3V residual on boost output in standby doesn't pass through FET1 (VGS ≈ 0). Supports pass-through charging natively; XIAO USB-C is flashing-only. Sealed enclosure avoids dual-charger conflict.

**Data line protection:** original ESD7351HT1G TVS diode tested functional (0.831V forward drop, clean reverse block) but its clamping voltage (8–10V under ESD pulse) exceeds the nRF52840 GPIO absolute max (3.9V). Replaced with **PRTR5V0U2X steering diode array + 330Ω series resistor** — steering diodes clamp to VDD+0.3V/GND−0.3V (tracking the rail) rather than a fixed breakdown, which gets much closer to the tight GPIO ceiling. 330Ω also matches the standard series-resistor value for SK6812/WS28xx data lines.

---

## Battery Installation (Permanent 18650)

- **Nickel strip:** nickel-plated steel, 0.1–0.15mm thick, ~3mm wide for the BMS pad connections (board is only 7.6mm wide)
- **Cell-side connection:** spot-welded (2–4 welds per terminal), using the **UK1 (AWithZ)** portable battery spot welder
- **BMS-side connection:** nickel strip is **hand-soldered** to B+/B− (not spot-welded — PCB pad adhesion is too weak for the weld pen's pressure/current pulse), at 660–700°F with 63/37 0.6mm solder and RMA no-clean liquid flux (Quimtech), nickel strip surface mechanically abraded before soldering
- **P+/P− (protected output side):** hand-soldered, **22AWG silicone-insulated wire** (chosen over 26/24AWG for headroom against the ~4.2–5.2A worst-case draw; 26AWG was too close to its ceiling, 22AWG gives comfortable margin and matches common JST-PH/XT30 pigtail gauge)
- **Wire strain relief:** small dab of hot glue at each solder joint before wrapping
- **BMS mounting orientation:** FET/component side of the BMS faces the battery, B+/B−/P+/P− pad side faces outward (more exposed bare copper on the pad side, so it's the side to protect if the insulation ever wears through; also keeps all four solder points accessible)
- **Insulation between BMS and cell:** Highland barley paper (0.2mm, one-side adhesive, purpose-made for 18650/21700/26650 pack insulation) — adhesive side to the BMS board, non-adhesive side against the cell wrap
- **Outer mechanical retention:** heat-shrink tube, **sized up to 21700 diameter** (not 18650) to clear the ~2.2mm bump from the BMS board + barley paper stacked on the cell — bare 18650 shrink has essentially no stretch margin left over for that bump and risks tenting or stressing the board's corners
- **Wire exit through heat-shrink:** since P+/P− sit mid-board, cut a small slit in the tube at the marked P+/P− location (not just using the tube's open end), feed both wires through while positioning the tube, shrink, then seal the slit with a dab of hot glue afterward
- Board corners should be taped/rounded before shrinking to avoid concentrating stress on the solder joints

**Open item:** exact wire routing destination from P+/P− (straight to FET0 vs. an intermediate connector) not yet finalized — affects how much slack to leave before final glue-seal of the heat-shrink exit.

---

## KiCad Schematic Progress

- Project: `disney_ears_power_stage`, using **JLCImport** plugin (installed via third-party PCM repo URL) for verified symbol+footprint+3D imports, avoiding manual pin-mapping risk
- **Verified parts imported (LCSC part numbers):**
  - SSM3J328R,LF — P-FET, ×4 instances (FET1, FET2; FET0/FET4 no longer used) — **C396016**
  - S8050-J3Y — NPN level-shifter — **C18221467** (chosen over SS8050/C916392; same manufacturer JSMSEMI, functionally interchangeable, J3Y is Basic-library/cheaper/higher stock)
  - 330Ω resistor, 0603 — **C23138** (Uni-Royal 0603WAF3300T5E)
  - PRTR5V0U2X — data line steering diode array (replacing ESD7351HT1G)
- **SSM3J328R pinout confirmed:** Gate–Source–Drain (pins 1-2-3), matches Toshiba datasheet and the JLCImport symbol — verified before wiring
- **AO3401A evaluated as a cheaper alternative P-FET, rejected:** lower current rating (−4A vs. SSM3J328R's −6A) and roughly double the Rds(on) (~65mΩ vs. ~30mΩ @ Vgs=−4.5V) made it unsuitable for FET0/FET4's full-current path; would have been acceptable for FET1/FET2's lower-current roles only. Since FET0/FET4 are no longer in the design, this tradeoff is largely moot but SSM3J328R was kept throughout for BOM consistency.
- FET1 + S8050 level-shift stage wiring plan finalized (see Power Architecture above)
- Net labeling convention: `VBAT_RAW`, `RAIL_5V`, `LED_5V_SW`, standard GND power symbol (not text labels)

**Open items:**
- FET0's control scheme was under discussion but FET0 has since been eliminated entirely (see above) — no longer applicable
- FET2's sense method is finalized (passive GND-jumper gate, see FET matrix above) — no longer an open item

**6-pin pogo connector assignment (per ear):** PWR, GND, DATA, SENSE currently in use; DATA is daisy-chained (one XIAO GPIO feeds ear 1, ear 1's data-out feeds ear 2 — removing ear 1 breaks the chain to ear 2, an accepted tradeoff of this topology). The remaining 2 pins are reserved for a future resistor-ladder ear-ID scheme (distinguishing ear type/variant), not yet wired or used by any current firmware.

---

## Assembly Notes

- SOT-23 hand-soldering technique: iron at 340°C (Yihua), fine chisel/conical tip, drag-soldering one leg at a time — deliberately **not** using hot air for SOT-23-scale discretes (risk of tombstoning/shifting lightweight parts); hot air reserved for future leadless/QFN-style parts if the BOM ever needs one
- Flux: Quimtech RMA (rosin mildly activated), no-clean, acid-free liquid dropper flux — safe to leave residue except on **pogo pin contacts**, which should be wiped with 90%+ IPA after soldering since flux residue on a mechanical sliding contact can cause intermittent resistance over time
- Spot welder selected: **AWithZ UK1** (5,000mAh battery, 9,000W peak, 0.1–0.3mm nickel range, 99-step gear) — sufficient for single-cell permanent installation; UK3/UKF10/UF20B evaluated but not needed for this scope

---

## Enclosure & Mechanical

- Two-piece telescoping shell, modeled in Fusion 360
- Back plate: 2mm thick, 0.8mm outer wall, 12mm rise
- Top plate: 2mm thick, 0.8mm inner wall, 14mm descent
- 0.2mm radial gap, 12mm overlap — forms a labyrinth joint
- Bambu X1C confirmed capable of holding 0.2mm tolerance on 0.8mm walls

---

## Fiber Optic Subsystem

- Disc component: 80mm inner / 100mm outer / 16mm tall
- Outward-facing SK6812 pixels illuminate a glow-in-the-dark PLA diffuser wall
- 1.0mm unjacketed PMMA end-glow fiber (~200m spool) for pinhole light points
- Fiber coupling: heat shrink over the LED dome with fiber strands fed in, shrunk down, secured with UV epoxy
- Four SK6812 pixels positioned around the ring wall
- Front/rear LED sub-assemblies bench-built before installation

---

## LED Spec

- 30× SK6812 RGBW pixels per ear (BTF-LIGHTING SK68121M144RGBNWW65, 144 LED/m, IP65, 4000K)
- 64 total addressable pixels per ear string, including fiber optic pixels
