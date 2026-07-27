# Post-MVP Feature Backlog
> NOTE: Features in this file are NOT part of the current build milestone.

## v1.1 - Enhancements
- [ ] LIS3DSH motion sensor & haptic motor hardware integration (software code ready; hardware deferred to v1.1 enclosure revision)
- [ ] i2c battery info
- [ ] battery sweep on start (2x 10k resistor divider to ADC pin for LiPo B+ voltage monitoring)
- [ ] RSSI filter threshold (-90 dBm)
- [ ] General packet deduplication window (2.5s)
- [ ] Custom trigger cooldown (4.0s)
- [ ] Statue beacon cooldown (30.0s)
- [ ] Hardware: Solder wire tap to IP5310 KEY button pads for external power ON/OFF tactile switch
- [ ] Enclosure design: Add 3D-printed 4-cell anti-bleed light baffle / divider panel with clear PLA light pipes for IP5310 battery status LEDs
- [ ] Enclosure design: Add 3.0mm pass-through opening for LED strip wires on bottom-left of inner mounting ring
- [ ] Enclosure design: Add retention clips / tabs to securely hold down the 503450 LiPo battery
- [ ] Enclosure design: Extend ESP32-S3 SuperMini mounting tray/pocket slightly longer on the long side for a clean fit
- [ ] Enclosure design: Shift ESP32-S3 SuperMini holder to the left to clear room for the inner LED channel path
- [ ] Enclosure design: Add open wire channel / path off IP5310 board for LiPo battery wiring (B+ / B-)
- [ ] Battery Optimization (LIS3DSH 25Hz ODR, 100ms polling interval, Light Sleep between BLE scans, 70% FastLED brightness cap)

## v2.0 - Advanced Features
- [ ] esp now to sync ears with master slave switching to keep battery even
- [ ] Mulit Power Modes (idle, active, show mode, deep sleep)
- [ ] motion control for sleep and wake up
- [ ] wave register for beacons
