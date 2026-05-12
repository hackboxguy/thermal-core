#!/usr/bin/env python3
"""Pure-integer Python reference for thermal_fault_stuck_sensor_step."""

N_TICKS = 40
TICK_MS = 100

PERSIST       = 1
RECOVERY      = 2
DELTA_MC      = 100     # threshold0
WINDOW_TICKS  = 10      # threshold1
SEVERITY      = 1       # DEGRADED
ACTION        = 2       # USE_ZONE_FALLBACK
CORRELATED_CONTEXT_ID = 7  # configured (not advisory)

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


def stuck_sensor_step(s, sensor, valid, load_changing, now_ms):
    if valid:
        if s["window_tick_count"] == 0:
            s["window_value_min"] = sensor
            s["window_value_max"] = sensor
        else:
            if sensor < s["window_value_min"]: s["window_value_min"] = sensor
            if sensor > s["window_value_max"]: s["window_value_max"] = sensor
        s["window_tick_count"] += 1

    cond = 0
    if WINDOW_TICKS > 0 and s["window_tick_count"] >= WINDOW_TICKS:
        advisory = (CORRELATED_CONTEXT_ID == 0xFFFF)
        if not advisory and load_changing:
            delta = s["window_value_max"] - s["window_value_min"]
            if delta < DELTA_MC:
                cond = 1
        # reset window
        s["window_value_min"] = sensor
        s["window_value_max"] = sensor
        s["window_tick_count"] = 1 if valid else 0

    fsm_tick(s, cond, SEVERITY, ACTION, PERSIST, RECOVERY, now_ms)


def main():
    s = {
        "state": NORMAL, "persist_count": 0, "recovery_count": 0,
        "entered_ts_ms": 0,
        "window_value_min": 0, "window_value_max": 0, "window_tick_count": 0,
    }
    print("tick,now_ms,sensor,sensor_valid,load_changing,state,persist_count,recovery_count,window_tick_count,window_delta")
    for t in range(N_TICKS):
        sensor = 75000
        valid = 1
        load = 1
        stuck_sensor_step(s, sensor, valid, load, t * TICK_MS)
        delta = s["window_value_max"] - s["window_value_min"]
        print(f"{t},{t * TICK_MS},{sensor},{valid},{load},"
              f"{s['state']},{s['persist_count']},{s['recovery_count']},"
              f"{s['window_tick_count']},{delta}")


if __name__ == "__main__":
    main()
