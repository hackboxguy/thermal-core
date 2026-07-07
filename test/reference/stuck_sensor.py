#!/usr/bin/env python3
"""Pure-integer Python reference for thermal_fault_stuck_sensor_step."""

N_TICKS = 40
TICK_MS = 100

PERSIST       = 1
RECOVERY      = 2
DELTA_MC      = 100     # threshold0
WINDOW_TICKS  = 10      # threshold1
CONTEXT_DELTA = 50      # threshold2
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


def stuck_sensor_step(s, sensor, valid, context_valid, context_value, now_ms):
    advisory = (CORRELATED_CONTEXT_ID == 0xFFFF)
    if valid:
        if s["window_tick_count"] == 0:
            s["window_value_min"] = sensor
            s["window_value_max"] = sensor
            s["window_context_count"] = 0
        else:
            if sensor < s["window_value_min"]: s["window_value_min"] = sensor
            if sensor > s["window_value_max"]: s["window_value_max"] = sensor
        if not advisory and context_valid:
            if s["window_context_count"] == 0:
                s["window_context_min"] = context_value
                s["window_context_max"] = context_value
            else:
                if context_value < s["window_context_min"]:
                    s["window_context_min"] = context_value
                if context_value > s["window_context_max"]:
                    s["window_context_max"] = context_value
            if s["window_context_count"] < 0xFFFF:
                s["window_context_count"] += 1
        s["window_tick_count"] += 1

    cond = 0
    if WINDOW_TICKS > 0 and s["window_tick_count"] >= WINDOW_TICKS:
        correlated_material_change = 0
        if not advisory and s["window_context_count"] > 0:
            context_delta = s["window_context_max"] - s["window_context_min"]
            if context_delta >= CONTEXT_DELTA:
                correlated_material_change = 1
        if advisory or correlated_material_change:
            delta = s["window_value_max"] - s["window_value_min"]
            if delta < DELTA_MC:
                cond = 1
        # reset window
        s["window_value_min"] = sensor
        s["window_value_max"] = sensor
        s["window_tick_count"] = 1 if valid else 0
        s["window_context_count"] = 0
        if valid and not advisory and context_valid:
            s["window_context_min"] = context_value
            s["window_context_max"] = context_value
            s["window_context_count"] = 1

    fsm_tick(s, cond, SEVERITY, ACTION, PERSIST, RECOVERY, now_ms)


def main():
    s = {
        "state": NORMAL, "persist_count": 0, "recovery_count": 0,
        "entered_ts_ms": 0,
        "window_value_min": 0, "window_value_max": 0, "window_tick_count": 0,
        "window_context_min": 0, "window_context_max": 0,
        "window_context_count": 0,
    }
    print("tick,now_ms,sensor,sensor_valid,context_valid,context_value,state,persist_count,recovery_count,window_tick_count,window_delta,context_delta")
    for t in range(N_TICKS):
        sensor = 75000
        valid = 1
        context_valid = 1
        context_value = 0 if t % 10 < 5 else 100
        stuck_sensor_step(s, sensor, valid, context_valid,
                          context_value, t * TICK_MS)
        delta = s["window_value_max"] - s["window_value_min"]
        context_delta = s["window_context_max"] - s["window_context_min"]
        print(f"{t},{t * TICK_MS},{sensor},{valid},{context_valid},"
              f"{context_value},"
              f"{s['state']},{s['persist_count']},{s['recovery_count']},"
              f"{s['window_tick_count']},{delta},{context_delta}")


if __name__ == "__main__":
    main()
