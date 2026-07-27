# v2.0 Custom All-in-One Ear PCB Architecture (KiCad 10)

> **Overview:** Universal 25mm–30mm Circular All-in-One Custom PCB for Disney Interactive Ear Receiver Wearables.

---

## 1. Silicon Chipset Specifications

| Component Subsystem | Chipset / Part Number | Manufacturer | Key Specification / Function |
|---|---|---|---|
| **Microcontroller & BLE** | **nRF52840-QIAA** (or **nRF52840-MINI** module) | Nordic Semiconductor | 32-bit ARM Cortex-M4F, 4.8mA active BLE RX, 1.5µA System OFF sleep |
| **LiPo Battery Charger** | **MCP73831T-2ACI/OT** | Microchip | SOT-23-5 Linear Charger, $R_{\text{PROG}} = 2.0\text{k}\Omega$ (**Strict 500mA 1C MAX Limit**) |
| **5V LED Boost Converter** | **TPS61023DRLR** | Texas Instruments | Synchronous Step-Up Boost ($3.0\text{V}-4.2\text{V} \rightarrow 5.0\text{V} @ 3.0\text{A}$), 95% efficiency, `EN` pin control |
| **Battery Fuel Gauge** | **MAX17048G+T10** | Analog Devices / Maxim | I2C ModelGauge battery percentage % & voltage sensor |
| **Real-time Current Monitor** | **INA219AIDR** | Texas Instruments | I2C High-side current & power meter |
| **3-Axis Motion Sensor** | **LIS3DSH** (or **BMI270**) | STMicroelectronics / Bosch | I2C Accelerometer with `INT1` hardware wake interrupt pin |
| **Vibration Motor Driver** | **SI2302CDS-T1-GE3** | Vishay / Diodes Inc | N-Channel MOSFET ($V_{DS}=20\text{V}, I_D=2.8\text{A}$), SOT-23 package |
| **USB Charging Port** | **TYPE-C-31-M-12** (16-pin USB-C) | Midya / C&K | USB 2.0 16-pin USB-C connector with dual $5.1\text{k}\Omega$ CC pull-down resistors |

---

## 2. Pin Netlist Mapping (nRF52840)

| nRF52840 GPIO Pin | Connected Signal Net | Function |
|---|---|---|
| **P0.06** | `I2C_SDA` | Shared I2C Data Line (LIS3DSH, MAX17048, INA219) |
| **P0.08** | `I2C_SCL` | Shared I2C Clock Line (LIS3DSH, MAX17048, INA219) |
| **P0.10** | `MOTION_INT1` | Hardware Wake-On-Motion Interrupt from LIS3DSH |
| **P0.12** | `NEOPIXEL_DATA` | FastLED / WS2812B 2020 Data Signal Output |
| **P0.15** | `HAPTIC_PWM` | PWM Control Signal to SI2302 MOSFET Gate |
| **P0.17** | `BOOST_ENABLE` | High = Enable 5V Boost Converter, Low = Shut Down 5V Boost |
| **P0.20** | `KEY_BUTTON` | Input with Pull-Up for External Tactile Power Switch |

---

## 3. PCB Layout Guidelines
- **Board Shape:** Circular PCB, $\varnothing 25.0\text{mm} - 30.0\text{mm}$.
- **USB-C Placement:** Edge-mounted on the bottom perimeter.
- **Layer Count:** 2-Layer (or 4-Layer with internal GND / Power planes for optimal RF performance).
- **Antenna:** On-board Meander Inverted-F PCB Trace Antenna or 2.4GHz Chip Antenna.
