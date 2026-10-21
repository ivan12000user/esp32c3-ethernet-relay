# Changelog

## v1.3.0 (field tested, 2026-10-21)

- Ethernet W5500, Wi-Fi modes OFF/backup/parallel STA/AP
- Four independently controllable 12V relay channels (active HIGH)
- Web status/control with individual indicator dots and 1.5s refresh
- Web OTA support, MQTT with NVS settings, Modbus TCP FC01/02/04/05/15
- Field-tested Ethernet, STA, Web, FC01, FC05 relay actuation, MQTT commands/states
- Repository copy externalizes Web/AP passwords into untracked `secrets.h` (small intentional build configuration change)

## v1.4.0 (planned, not released)

- Four independent per-channel ping watchdogs with safety lockouts, logs and configuration page
