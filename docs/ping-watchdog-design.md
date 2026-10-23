# Ping Watchdog design — v1.4.0 (proposal)

Status: **design only — not flashed or field tested**.

## Independent per-channel state machine

Each channel has independent parameters, counters and scheduling. Times below are defaults, configurable in Web UI.

| State | Meaning | Next action |
|---|---|---|
| DISABLED | Manual/pulse mode or watchdog disarmed | No ping/restarts |
| GRACE | Relay has just been turned ON | Keep ON; wait 120 s for host boot |
| HEALTHY | Host reachable | Ping after 10 s |
| SUSPECT | 1–2 consecutive ping failures | Ping again; don't power-cycle |
| POWER_OFF | Failed 3 times | Switch controlled relay OFF for 10 s |
| RECOVERY | Reapply relay ON | Enter GRACE |
| LOCKOUT | Too many restart attempts | Keep relay ON, stop cycling; report alarm |

Only the channel whose IP failed can change outputs. Hardware assumes **NO** contacts for controlled power supply: ON means power delivered; OFF means power cut. A network reset changes TCP sessions but must not cause automatic relay cycling.

## Edge cases

- On startup, outputs initialize **OFF** like v1.3.0; after loading a properly armed channel config, an explicit watchdog policy will turn the selected relay ON, then start GRACE. The effect of firmware boot on a connected load must be documented and accepted before enabling watchdog.
- Do not trigger on Wi-Fi/Ethernet **local interface outage**. Suspend watchdog evaluation while route to the target is unavailable. Delay retries after route returns.
- Avoid counting failed pings when the device itself has no working network stack. A local link-up is necessary but not sufficient evidence of target reachability: optional second "health probe" (e.g., gateway) can qualify restart decisions.
- A target that intentionally blocks ICMP must not be monitored in ping-watchdog mode.
- Safe defaults: watchdog mode OFF, interval 10 s, failures 3, OFF pulse 10 s, boot grace 120 s, cap 3/h.
- Guard counter overflow (`millis()` wrap), invalid IPv4 and unspecified network path.
- **Do not use a synchronous multi-second ping loop** in the Arduino main loop; maintain HTTP, MQTT, Modbus and OTA servicing.
- Define ownership of relay state: in watchdog mode, remote commands should be rejected with an explicit error unless a controlled manual-override mode is deliberately enabled.
- Event codes: `ping_ok`, `ping_fail`, `power_cycle`, `recovered`, `lockout`, `watchdog_paused`. Publish per channel with rate limiting.
- Keep existing Modbus coil 0–3 command semantics in manual mode, and IR 0–7 diagnostic addresses unchanged; add new registers at higher addresses.

## Proposed Web inputs per channel

- display name
- mode `manual / pulse / ping-watchdog`
- target IPv4
- check interval, seconds
- failures to trigger
- relay power OFF duration, seconds
- boot grace, seconds
- maximum cycles per hour
- watchdog arm/disarm, lockout reset

Web status shows channel active state, last ICMP RTT/time, consecutive failures, number of cycles and reason for lockout.

## Validation

Before testing watchdog with real equipment: test on disconnected loads; then LED dummy-load; simulate individual host loss, total LAN outage, reboot, brownout, and OTA. Confirm no cross-channel relays cycle due to single host failure.

Issue: #1.
