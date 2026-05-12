#!/usr/bin/env python3
"""Pure-integer Python reference for thermal_fault_stale_context_step."""

N_TICKS    = 30
TICK_MS    = 100
TIMEOUT_MS = 3000

PERSIST   = 2
RECOVERY  = 2
SEVERITY  = 1     # DEGRADED
ACTION    = 2     # USE_ZONE_FALLBACK

NORMAL, DEGRADED, CRITICAL, LATCHED, RECOVERING = 0, 1, 2, 3, 4


def action_to_state(action, severity):
    if action in (4, 5):
        return LATCHED
    if severity == 2:
        return CRITICAL
    return DEGRADED


def fsm_tick(s, cond, severity, action, persist_t, recovery_t, now_ms):
    if s["state"] == NORMAL:
        if cond:
            s["persist_count"] += 1
            if s["persist_count"] >= persist_t:
                s["state"] = action_to_state(action, severity)
                s["entered_ts_ms"] = now_ms
                s["recovery_count"] = 0
        else:
            s["persist_count"] = 0
    elif s["state"] in (DEGRADED, CRITICAL):
        if cond:
            s["recovery_count"] = 0
        else:
            s["state"] = RECOVERING
            s["recovery_count"] = 0
            s["entered_ts_ms"] = now_ms
    elif s["state"] == RECOVERING:
        if cond:
            s["state"] = action_to_state(action, severity)
            s["persist_count"] = 0
            s["recovery_count"] = 0
            s["entered_ts_ms"] = now_ms
        else:
            s["recovery_count"] += 1
            if s["recovery_count"] >= recovery_t:
                s["state"] = NORMAL
                s["persist_count"] = 0
                s["recovery_count"] = 0
                s["entered_ts_ms"] = now_ms
    elif s["state"] == LATCHED:
        if cond:
            s["recovery_count"] = 0
        elif s["recovery_count"] < 0xFFFF:
            s["recovery_count"] += 1


def stale_context_step(s, ms_since_last_valid, timeout_ms, now_ms):
    cond = 1 if ms_since_last_valid >= timeout_ms else 0
    fsm_tick(s, cond, SEVERITY, ACTION, PERSIST, RECOVERY, now_ms)


def main():
    s = {
        "state": NORMAL, "persist_count": 0, "recovery_count": 0,
        "entered_ts_ms": 0,
    }
    print("tick,now_ms,ms_since_last_valid,state,persist_count,recovery_count")
    for t in range(N_TICKS):
        if t < 10:
            ms_since = t * 500
        else:
            ms_since = 100
        stale_context_step(s, ms_since, TIMEOUT_MS, t * TICK_MS)
        print(f"{t},{t * TICK_MS},{ms_since},"
              f"{s['state']},{s['persist_count']},{s['recovery_count']}")


if __name__ == "__main__":
    main()
