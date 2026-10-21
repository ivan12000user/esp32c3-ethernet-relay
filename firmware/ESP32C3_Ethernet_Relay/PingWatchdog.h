#pragma once
#include <stdint.h>

// Small deterministic watchdog state machine; does not perform network I/O.
// A transport adapter must carry out PING and supply reportPing() result.
// ON means GPIO HIGH, relay energized, power supplied via normally-open contacts.
enum WdMode : uint8_t { WD_MANUAL = 0, WD_PULSE = 1, WD_PING = 2 };
enum WdState : uint8_t {
  WD_DISABLED = 0, WD_GRACE = 1, WD_HEALTHY = 2, WD_SUSPECT = 3,
  WD_POWER_OFF = 4, WD_LOCKOUT = 5, WD_PAUSED = 6
};
enum WdAction : uint8_t { WD_NONE = 0, WD_SET_ON = 1, WD_SET_OFF = 2, WD_SEND_PING = 3 };

struct WdConfig {
  uint8_t mode = WD_MANUAL;
  bool armed = false;
  uint16_t intervalSec = 10;
  uint8_t failLimit = 3;
  uint16_t offSec = 10;
  uint16_t graceSec = 120;
  uint8_t maxCycles = 3;
  uint32_t pulseMs = 500;
};

class WdMachine {
public:
  WdConfig config;
  WdState state = WD_DISABLED;
  uint8_t failures = 0;
  uint32_t cyclesTotal = 0;
  uint32_t lastPingMs = 0;
  bool lastPingOk = false;
  bool pingPending = false;
  uint32_t dueMs = 0;
  uint32_t pulseDueMs = 0;
  bool pulseActive = false;

  static bool due(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
  }

  bool active() const { return config.mode == WD_PING && config.armed; }

  // A newly armed watchdog must first power its NO-wired load and wait for boot.
  WdAction configure(const WdConfig &next, uint32_t now) {
    const bool wasActive = active();
    const bool wasOff = state == WD_POWER_OFF;
    config = next;
    pingPending = false;
    failures = 0;
    pulseActive = false;
    if (!active()) {
      state = WD_DISABLED;
      return wasOff ? WD_SET_ON : WD_NONE;
    }
    state = WD_GRACE;
    dueMs = now + (uint32_t)config.graceSec * 1000UL;
    (void)wasActive;
    return WD_SET_ON;
  }

  WdAction startPulse(uint32_t now) {
    pulseActive = true;
    pulseDueMs = now + config.pulseMs;
    return WD_SET_ON;
  }

  void cancelPulse() { pulseActive = false; }

  // Tick is non-blocking; all times computed from millis() (wrap-safe).
  WdAction tick(uint32_t now, bool networkReady) {
    if (config.mode == WD_PULSE && pulseActive && due(now, pulseDueMs)) {
      pulseActive = false;
      return WD_SET_OFF;
    }
    if (!active()) return WD_NONE;

    // Never strand a power-controlled device OFF due to a lost uplink.
    if (state == WD_POWER_OFF) {
      if (due(now, dueMs)) {
        state = WD_GRACE;
        dueMs = now + (uint32_t)config.graceSec * 1000UL;
        return WD_SET_ON;
      }
      return WD_NONE;
    }
    if (state == WD_LOCKOUT) return WD_NONE;
    if (!networkReady) {
      pingPending = false;
      state = WD_PAUSED;
      return WD_NONE;
    }
    if (state == WD_PAUSED) {
      state = WD_GRACE;
      failures = 0;
      dueMs = now + (uint32_t)config.graceSec * 1000UL;
      return WD_NONE;
    }
    if (pingPending) return WD_NONE;
    if (due(now, dueMs)) {
      pingPending = true;
      return WD_SEND_PING;
    }
    return WD_NONE;
  }

  WdAction reportPing(bool ok, uint32_t now) {
    if (!active() || !pingPending) return WD_NONE;
    pingPending = false;
    lastPingMs = now;
    lastPingOk = ok;
    dueMs = now + (uint32_t)config.intervalSec * 1000UL;
    if (ok) {
      failures = 0;
      state = WD_HEALTHY;
      return WD_NONE;
    }
    if (failures < 255) ++failures;
    state = WD_SUSPECT;
    if (failures < config.failLimit) return WD_NONE;

    uint8_t count = 0;
    for (uint8_t i = 0; i < historyUsed; ++i) {
      if ((uint32_t)(now - history[i]) < 3600000UL) ++count;
    }
    if (count >= config.maxCycles) {
      state = WD_LOCKOUT;
      return WD_NONE; // Leave powered ON, require deliberate re-arm
    }

    history[historyNext] = now;
    historyNext = (historyNext + 1) % 10;
    if (historyUsed < 10) ++historyUsed;
    ++cyclesTotal;
    failures = 0;
    state = WD_POWER_OFF;
    dueMs = now + (uint32_t)config.offSec * 1000UL;
    return WD_SET_OFF;
  }

  void abortPing(uint32_t now) {
    pingPending = false;
    if (active() && state != WD_POWER_OFF && state != WD_LOCKOUT) {
      state = WD_PAUSED;
      dueMs = now;
    }
  }

  void resetLockout(uint32_t now) {
    historyUsed = historyNext = 0;
    failures = 0;
    pingPending = false;
    if (active()) {
      state = WD_GRACE;
      dueMs = now + (uint32_t)config.graceSec * 1000UL;
    } else state = WD_DISABLED;
  }

private:
  uint32_t history[10] = {};
  uint8_t historyUsed = 0;
  uint8_t historyNext = 0;
};
