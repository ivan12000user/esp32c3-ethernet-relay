# Changelog

## v1.3.0 (field tested, 2026-10-21)

- Ethernet W5500, Wi-Fi modes OFF/backup/parallel STA/AP
- Four independently controllable 12V relay channels (active HIGH)
- Web status/control with individual indicator dots and 1.5s refresh
- Web OTA support, MQTT with NVS settings, Modbus TCP FC01/02/04/05/15
- Field-tested Ethernet, STA, Web, FC01, FC05 relay actuation, MQTT commands/states
- Repository copy externalizes Web/AP passwords into untracked `secrets.h` (small intentional build configuration change)

## v1.4.1-dev (2026-10-23, unreleased)

- Limit maximum Wi-Fi transmit power to **8.5 dBm** for both STA and AP.
- Apply limit just after `WiFi.begin()` and successful `WiFi.softAP()` startup, including Wi-Fi backup restarts.
- Report `wifi_tx_dbm` via `/api/state` and readback in Serial Monitor.
- Preserve existing Wi-Fi modes, W5500 Ethernet, relay control, Web/MQTT/Modbus/OTA/watchdog features.
- **Not yet tested on real ESP32-C3 hardware**. Keep watchdog disarmed until runtime checks are complete.

## v1.4.0 (candidate, hardware testing in progress; not released)

- Four independent per-channel ping watchdogs with NVS settings, Web UI, MQTT events, Modbus diagnostics and lockouts.
- Actual ESP32-C3 bench testing verified `HEALTHY` on a live IP, one physical OFF/ON relay cycle on a nonresponsive test IP and `LOCKOUT` with a stable ON output.
- Fix after hardware tests: Web **Save** preserves an existing LOCKOUT, does not interrupt a watchdog power-off pulse, and does not reset the hourly rate-limit history.
- Disarm/rearm cannot bypass the hourly limit; only the explicit **Reset LOCKOUT** action clears its history.
- Web status and `/api/state` now expose `cycles_hour` separately from total `cycles`.
- Form inputs use `box-sizing: border-box` to stay within their borders.

**Pending:** regression check on ESP32-C3 hardware; multi-channel behavior, outage detection with live LAN link, and safe startup remain to be qualified. Do not use to power-cycle critical loads.
