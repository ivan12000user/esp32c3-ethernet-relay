# Wiring (tested on ESP32-C3 Super Mini)

## Ethernet W5500 (SPI)

| W5500 pin | ESP32-C3 |
|---|---|
| SCLK | GPIO4 |
| MISO | GPIO5 |
| MOSI | GPIO6 |
| SCS / CS | GPIO7 |
| INT | GPIO21 |
| RESET | Not connected in this build (`ETH_RST=-1`) |
| GND | Common GND |
| 5V | Regulated 5V (module's **5V** pin) |

## 12V 4-channel relay module

Relays observed: **Songle SRD-12VDC-SL-C**, *not* 5V-coil relays. Control board jumpers set to **H** (HIGH=ON).

| ESP32-C3 | Relay board |
|---|---|
| GPIO0 → **1 kΩ** | IN1 |
| GPIO1 → **1 kΩ** | IN2 |
| GPIO3 → **1 kΩ** | IN3 |
| GPIO10 → **1 kΩ** | IN4 |
| GND | DC- |
| 12 V DC | DC+ |

Bench measurement in H mode: 3.3V through 1 kΩ gave about **2.6V** on an IN pin, **0.7 mA**, and actuated the relay.

Supply topology: 12V PSU → relay board DC+, DC/DC 12V→5V → ESP32-C3 5V and W5500 5V. All logic grounds common. Keep mains on the relay contact side physically separated from low-voltage wiring.

For safe startup: program holds outputs LOW (=OFF). Confirm on each actual board that relay contacts do not momentarily switch while the MCU boots/resets. Do not use for safety-critical loads.
