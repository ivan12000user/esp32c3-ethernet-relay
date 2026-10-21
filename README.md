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

The source in Git is a **credential-sanitized copy** of the working v1.3.0 sketch. The deployed firmware was tested on the actual hardware; this repository version additionally requires a local secrets header, and has **not yet been independently flashed**.

## Build / install

1. In Arduino IDE install **esp32 by Espressif Systems** (3.x) and **PubSubClient** (Nick O'Leary).
2. Copy `firmware/ESP32C3_Ethernet_Relay/secrets.example.h` to `firmware/ESP32C3_Ethernet_Relay/secrets.h` and enter strong unique passwords. **Do not commit `secrets.h`.**
3. Open `firmware/ESP32C3_Ethernet_Relay/ESP32C3_Ethernet_Relay.ino`.
4. Board: ESP32C3 Dev Module. Flash: 4 MB. USB CDC On Boot: Enabled. Partition scheme: **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**.
5. Build and flash via USB initially. Subsequent **application .ino.bin** updates can be uploaded via `/update` using the Web credentials. Do **not** upload merged/bootloader/partition images through Web OTA.

### Important security and operational limitations

- The repository is currently **public**. No live passwords, Wi-Fi credentials, broker credentials, tokens, or API keys belong in commits.
- The built-in HTTP + MQTT 1883 + Modbus TCP are unencrypted. Keep network access limited to a trusted LAN or VPN. Modbus write and Web relay endpoints in v1.3.0 do not implement per-client authorization.
- Avoid controlling critical / life-safety loads. Test power loss, reboot, network outages, and relay startup transitions with loads disconnected first.
- Existing devices running v1.3.0 with previously hardcoded demo Web password should have that credential rotated before exposure to less-trusted networks.

## Development

Next planned branch: `feature/ping-watchdog`.

Design: up to four **independent Ping Watchdogs**, one per relay, each with its own IP, ping interval, failure threshold, power-off pulse time, boot grace period, lockout/cooldown, and event counters. Watchdog is **disabled by default** and must be explicitly armed. On total network outage do not power-cycle all downstream devices.

Only reviewed, tested changes will be merged into `main`.
