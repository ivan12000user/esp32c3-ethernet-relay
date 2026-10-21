# Modbus TCP map (firmware v1.3.0)

- Server: TCP port **502**, unit ID **1** is used during tests (server echoes incoming Unit ID).
- Addressing in this document is **zero-based**. Some SCADA tools display Coil 00001 for address 0.

| Function | Zero-based addresses | Meaning |
|---|---|---|
| FC01 Read Coils | 0–3 | Relay 1–4 GPIO states |
| FC02 Read Discrete Inputs | 0–3 | Mirror of relay GPIO states |
| FC05 Write Single Coil | 0–3 | 0xFF00 ON / 0x0000 OFF |
| FC15 Write Multiple Coils | 0–3 | Multiple relay commands |
| FC04 Read Input Registers | 0–7 | Diagnostics below |

| Input register | Meaning |
|---|---|
| 0 | Uptime seconds low 16 bits |
| 1 | Uptime seconds high 16 bits |
| 2 | Ethernet ready: 0/1 |
| 3 | MQTT connected: 0/1 |
| 4 | Wi-Fi STA ready: 0/1 |
| 5 | AP ready: 0/1 |
| 6 | Wi-Fi mode (0 OFF; 1 backup; 2 parallel STA; 3 AP) |
| 7 | STA RSSI dBm (signed 16-bit interpreted as two's complement) |

**Field tests (2026-10-21):** FC01 read of 4 coils at address 0 returned `00-01-00-00-00-04-01-01-01-00`. FC05 to coil 0 via Wi-Fi returned expected ON and OFF echoes; relay 1 physically actuated. TCP port 502 was reachable via Ethernet and Wi-Fi.

Only one Modbus TCP client is serviced at a time in current firmware. Coil and discrete-input values are *requested states*, not physical contact feedback. No encryption/authentication exists on port 502.
