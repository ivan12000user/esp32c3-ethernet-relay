# ESP32-C3 Ethernet Relay

4-channel network relay on **ESP32-C3 Super Mini**, **W5500 SPI Ethernet** and a **12 V four-relay board**.

## Hardware

| Signal | ESP32-C3 GPIO |
|---|---:|
| W5500 SCK | 4 |
| W5500 MISO | 5 |
| W5500 MOSI | 6 |
| W5500 CS | 7 |
| W5500 INT | 21 |
| Relay IN1 (via 1 kΩ) | 0 |
| Relay IN2 (via 1 kΩ) | 1 |
| Relay IN3 (via 1 kΩ) | 3 |
| Relay IN4 (via 1 kΩ) | 10 |

Relay jumpers: **H** (active HIGH). Board relay supply: **12 V**. ESP32-C3 and W5500 boards: **5 V** to their 5 V input, using a suitable 12→5 V DC/DC. Common low-voltage ground. Relay indicators show *commanded GPIO states*, not feedback from contacts.

## Field-tested reference: v1.3.0

- W5500 Ethernet, DHCP, HTTP Web UI, Web OTA
- Wi-Fi modes: disabled, Ethernet backup, parallel STA, local AP
- Modbus TCP server at port 502: coils 0–3 and input registers
- MQTT client, retained relay states and status
- Four relay outputs with colored status LEDs in Web UI

See [wiring](docs/wiring.md), [Modbus map](docs/modbus-registers.md), [MQTT topics](docs/mqtt-topics.md).

The `main` branch contains the v1.3.0 reference. The `feature/ping-watchdog` branch now has **v1.4.2-dev** with independent Ping Watchdog and **Wi-Fi TX capped at 8.5 dBm**. Its Ping Watchdog v1.4.0 predecessor was bench-tested; **v1.4.2-dev still needs hardware validation**. There is no `secrets.h` dependency in the development sketch.

## Build / install

1. In Arduino IDE install **esp32 by Espressif Systems** (3.x) and **PubSubClient** (Nick O'Leary).
2. No `secrets.h` is required for **v1.4.2-dev**. The Web UI has **no authentication** by user request. Default AP password is `12357895`, which is **publicly visible in the source code**. If an older AP password was stored in NVS, it takes precedence: change it at `/wifi`.
3. Open `firmware/ESP32C3_Ethernet_Relay/ESP32C3_Ethernet_Relay.ino`.
4. Board: ESP32C3 Dev Module. Flash: 4 MB. USB CDC On Boot: Enabled. Partition scheme: **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**.
5. Build and flash via USB initially. Web OTA at `/update` accepts **application `.ino.bin`** files (not merged/bootloader/partition images). **Warning: OTA is not password-protected in v1.4.2-dev; anyone with network access to TCP/80 can upload firmware.** Restrict TCP/80 to trusted hosts or use USB instead.

### Important security and operational limitations

- The repository is **public**. The requested default AP password `12357895` appears in the public sketch and **must not be considered a secret**. Change it in Web settings for a deployment; avoid AP mode when not needed. Never commit saved Wi-Fi STA/MQTT credentials.
- **No Web login**: Web relay/settings/firmware upload are accessible to any host permitted to connect to port **80**. Modbus TCP writes on **502** have no authentication. MQTT on **1883** is unencrypted unless separately tunneled. Use firewall/VLAN/VPN isolation and never forward TCP 80/502 from the public Internet.
- Avoid controlling critical / life-safety loads. Test power loss, reboot, network outages, and relay startup transitions with loads disconnected first.
- v1.4.2-dev removes Web username/password entirely, by user request; this is a **security downgrade** from password-protected management/OTA and is suitable only for isolated trusted test networks.

## Development

Active experimental branch: `feature/ping-watchdog` (PR #2).

Design: up to four **independent Ping Watchdogs**, one per relay, each with its own IP, ping interval, failure threshold, power-off pulse time, boot grace period, lockout/cooldown, and event counters. Watchdog is **disabled by default** and must be explicitly armed. On total network outage do not power-cycle all downstream devices.

Only reviewed, tested changes will be merged into `main`.
