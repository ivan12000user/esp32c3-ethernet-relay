# v1.4.0 hardware test plan (pre-release)

**No mains or valuable devices on relay COM/NO/NC during initial tests.**
Use only relay board LEDs/clicks. Keep USB recovery accessible and confirm OTA partition setting before any Web update.

## Prerequisites

- Arduino-ESP32 3.x and PubSubClient installed. **No `secrets.h` required in v1.4.2-dev.**
- Board ESP32C3 Dev Module, Flash 4 MB, **Minimal SPIFFS (1.9 MB APP with OTA/128 KB SPIFFS)**, USB CDC On Boot Enabled.
- Build branch `feature/ping-watchdog`, not `main`.
- Keep a backup of field-tested v1.3.0 in case USB rollback is needed.

## Safe smoke test

1. Compile in Arduino IDE. Flash via **USB** for this first experimental build, with relay output contacts disconnected from loads.
2. Verify Ethernet Web, Wi-Fi Web, MQTT and Modbus FC01/FC05 still work normally when modes = Manual.
3. Open `http://<device-ip>/watchdog`. There is **no Web login** in v1.4.2-dev: test only in a trusted isolated LAN.
4. Start with one channel: select **Ping Watchdog** on Relay 1; enter reachable router `192.168.1.1`, period 5 s, failures 2, OFF 3 s, grace 10 s, cap 1/h; activate. It should turn ON after save, then stay ON if the router pings successfully. Other channels remain Manual.
5. Switch Relay 1 target to a deliberately unreachable reserved test address (e.g. `192.0.2.1` TEST-NET-1), save/activate. After 10 s grace + two failed checks, relay 1 should turn OFF for 3 s then return ON. Only Relay 1 may change.
6. On the next group of failed checks, cap 1/h must enter **LOCKOUT** and leave Relay 1 ON, until explicit reset.
7. Disconnect local Ethernet/Wi-Fi so no viable uplink remains. Controller must not initiate new power cycles; **if a power-off pulse is already in progress, it must finish with power restored**.
8. Verify Modbus FC04 8–13 reports Relay 1 mode, state, failures and cycle counters. Verify HTTP `/api/state` watchdog array and MQTT `/relay1/watchdog/event`.
9. In armed watchdog mode, check manual ON/OFF Web returns HTTP 409; Modbus FC05 returns exception 0x85/06; MQTT relay1/set is ignored. Disarm Relay 1 and confirm manual control works again.
10. Test Relay 2, 3 and 4 separately. Test manual/pulse mode, input validation, reboot, watchdog NVS persistence and OTA at the end.

## Limitations of first release candidate

- Link-loss protection checks Ethernet or Wi-Fi STA *interface readiness*. It **does not prove that the gateway or upstream network is healthy**. When the local link stays UP during a site-wide outage, multiple targets can still time out. Do not arm several channels until that failure mode has been tested.
- Device reboot initializes hardware relays OFF before software turns an explicitly armed watchdog channel ON. This brief interruption must be acceptable for the load.
- The per-hour rate limiter is RAM-based and resets after MCU reboot; a separate persistent anti-flapping guard is planned.
- Timing is controlled using `millis()`, but actual ICMP scheduling and response times can vary under network load.
- No physical sensing of relay contacts or load power.
- HTTP and Modbus TCP are not encrypted. The v1.4.2-dev Web UI, settings, relay commands and even OTA upload are **unauthenticated**, so network access must be restricted. Never expose these ports to the public Internet.
