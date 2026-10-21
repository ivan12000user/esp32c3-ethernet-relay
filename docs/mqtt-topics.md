# MQTT topics (v1.3.0)

MQTT broker settings are entered in the Web UI (`/mqtt`) and stored in ESP32 NVS; do not put credentials into this repository.

Default base topic: `ethernet-relay`.

| Topic | Direction | Payload |
|---|---|---|
| `ethernet-relay/relayN/set` | Broker → device | `ON`, `OFF`, `1`, `0`, `TOGGLE` |
| `ethernet-relay/relayN/state` | Device → broker | `ON` / `OFF` retained |
| `ethernet-relay/status` | Device → broker | `online` / `offline` retained/LWT |
| `ethernet-relay/ip` | Device → broker | Chosen outgoing interface IP retained |

`N` is one of 1–4. **Do not retain commands** on `/set` topics. In field testing the device connected to a configured MQTT broker and commands and states worked via LAN/VPS.

MQTT over unencrypted port 1883 must remain inside a trusted path (LAN or VPN); use TLS or network isolation before exposing this device to the Internet.
