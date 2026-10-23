# MQTT topics (v1.4.4-dev)

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

## Home Assistant MQTT Discovery (v1.4.4-dev)

The firmware publishes four retained discovery configurations on each successful MQTT connection:

`homeassistant/switch/esp32c3_relay_<efuse-mac>_relayN/config` (N=1..4).

They all use one stable device identifier based on the ESP32-C3 e-fuse MAC and appear in Home Assistant as **ESP32-C3 Ethernet Relay**, with four switch entities. Each switch uses the existing `<mqtt-base>/relayN/set` and `<mqtt-base>/relayN/state` topics; availability uses `<mqtt-base>/status` (`online`/`offline`).

- MQTT Discovery must be enabled in Home Assistant's MQTT integration (default prefix `homeassistant`).
- **The relay and Home Assistant must use the SAME broker instance**. The fact that both clients can connect to port 1883 does not mean they are connected to the same broker. Different VPN addresses can reach the same broker, but verify before troubleshooting discovery.
- `/api/state` reports `mqtt_connected` and `ha_discovery_published`. Both should be `true` after connection.
- Config and state payloads are published with `retain=true`, so HA can discover/recover devices on its restart. Older firmware versions did not publish discovery configuration.
- In armed Ping Watchdog mode, relay commands are deliberately refused; the HA switch still shows the relay output but cannot override watchdog safety logic.
- The MQTT Discovery prefix is fixed to `homeassistant` in this version. Device identity is stable even when routed through Ethernet, Wi-Fi or VPN.
