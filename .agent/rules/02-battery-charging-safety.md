---
trigger: always_on
description: Mandatory LiPo battery charging safety limits
---

# Battery Charging Safety Directives

**CRITICAL BATTERY CHARGING SAFETY RULE:**
1. **Never Exceed 1C Charge Rate:** LiPo battery charging rates must NEVER exceed 1C ($I_{max} = 1.0 \times \text{Capacity in Ah}$).
2. **500mAh Cell Max Charge:** For a 500mAh cell, maximum charging current is strictly capped at **500mA (0.5A)**.
3. **Thermal & Voltage Safety:** All charging circuit designs must use CC/CV architecture with thermal regulation and a strict **4.20V** maximum termination voltage.
