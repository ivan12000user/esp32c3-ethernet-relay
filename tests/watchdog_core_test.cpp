#include <cassert>
#include <cstdint>
#include <cstdio>
#include "../firmware/ESP32C3_Ethernet_Relay/PingWatchdog.h"

static WdConfig watchdogConfig() {
  WdConfig c;
  c.mode = WD_PING;
  c.armed = true;
  c.intervalSec = 10;
  c.failLimit = 3;
  c.offSec = 10;
  c.graceSec = 120;
  c.maxCycles = 3;
  return c;
}
int main() {
  WdMachine m;
  assert(m.state == WD_DISABLED);
  const auto c=watchdogConfig();
  assert(m.configure(c, 1000) == WD_SET_ON);
  assert(m.state == WD_GRACE);
  assert(m.tick(120000, true) == WD_NONE);
  assert(m.tick(121000, true) == WD_SEND_PING);
  assert(m.reportPing(false, 122000) == WD_NONE);
  assert(m.failures == 1 && m.state == WD_SUSPECT);
  assert(m.tick(132000, true) == WD_SEND_PING);
  assert(m.reportPing(false, 133000) == WD_NONE);
  assert(m.tick(143000, true) == WD_SEND_PING);
  assert(m.reportPing(false, 144000) == WD_SET_OFF);
  assert(m.state == WD_POWER_OFF && m.cyclesTotal == 1);
  assert(m.tick(153000, false) == WD_SET_ON); // Finish OFF pulse even with lost LAN
  assert(m.state == WD_GRACE);
  assert(m.tick(273000, false) == WD_NONE);
  assert(m.state == WD_PAUSED); // Prevent resets without a working local link
  assert(m.tick(274000, true) == WD_NONE);
  assert(m.state == WD_GRACE);

  // A successful probe clears the consecutive-failure counter.
  assert(m.tick(394000, true) == WD_SEND_PING);
  assert(m.reportPing(true, 394200) == WD_NONE);
  assert(m.state == WD_HEALTHY && m.failures == 0);

  // Failover lockout protects against repeated cycling.
  for (int cycle=0; cycle<2; ++cycle) {
    uint32_t now = 404200 + cycle*150600U;
    for (int j=0;j<3;++j) {
      assert(m.tick(now, true) == WD_SEND_PING);
      WdAction a=m.reportPing(false, now+200);
      assert(a == (j==2 ? WD_SET_OFF : WD_NONE));
      now += 10200;
    }
    assert(m.tick(now, true) == WD_SET_ON);
    // Grace 120 seconds
  }
  // Fourth attempted cycle during the same hour enters lockout.
  uint32_t now=705400;
  for (int j=0;j<3;++j) {
    assert(m.tick(now, true) == WD_SEND_PING);
    WdAction a=m.reportPing(false, now+200);
    assert(a == WD_NONE);
    now += 10200;
  }
  assert(m.state == WD_LOCKOUT && m.cyclesTotal == 3);
  // A locked channel may be re-armed explicitly, without an OFF command.
  m.resetLockout(1200000);
  assert(m.state == WD_GRACE && m.failures == 0);

  // Pulse mode is independent and does not initiate pings.
  WdConfig pulse;
  pulse.mode = WD_PULSE;
  pulse.pulseMs = 500;
  assert(m.configure(pulse, 2000) == WD_NONE);
  assert(m.startPulse(2000) == WD_SET_ON);
  assert(m.tick(2499,true) == WD_NONE);
  assert(m.tick(2500,true) == WD_SET_OFF);

  // millis() wraparound safe (deadline + 1000 wraps)
  assert(m.startPulse(0xFFFFFF00u) == WD_SET_ON);
  assert(m.tick(0x000000F3u, true) == WD_NONE);
  assert(m.tick(0x000000F4u, true) == WD_SET_OFF);

  std::puts("watchdog core tests passed");
  return 0;
}
