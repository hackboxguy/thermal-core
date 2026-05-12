#!/usr/bin/env python3
"""Pure-integer Python reference for thermal_fault_runaway_step (PRD §4.7)."""

N_TICKS = 50
TICK_MS = 100

PERSIST   = 10
RECOVERY  = 5
RISE_MC   = 500    # threshold0
COOL_PWM  = 200    # threshold1
SEVERITY  = 2      # CRITICAL
ACTION    = 4      # FORCE_PWM_MAX_AND_LATCH
WINDOW_MAX = 64

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


def runaway_step(s, temp, pwm, now_ms):
    persist = PERSIST
    if persist == 0:
        return
    if persist > WINDOW_MAX:
        persist = WINDOW_MAX

    s["temp_window"][s["window_head"]] = temp
    s["pwm_window"][s["window_head"]] = pwm
    s["window_head"] = (s["window_head"] + 1) % persist
    if s["window_head"] == 0:
        s["window_filled"] = 1

    cond = 0
    if s["window_filled"]:
        newest = (s["window_head"] + persist - 1) % persist
        oldest = s["window_head"]
        temp_N = s["temp_window"][newest]
        temp_M = s["temp_window"][oldest]
        pwm_min = 255
        for i in range(persist):
            if s["pwm_window"][i] < pwm_min:
                pwm_min = s["pwm_window"][i]
        if (temp_N - temp_M) >= RISE_MC and pwm_min >= COOL_PWM:
            cond = 1

    # Runaway uses persist_ticks=1 in FSM; window IS the persist mechanism
    fsm_tick(s, cond, SEVERITY, ACTION, 1, RECOVERY, now_ms)


def main():
    s = {
        "state": NORMAL, "persist_count": 0, "recovery_count": 0,
        "entered_ts_ms": 0,
        "temp_window": [0] * WINDOW_MAX,
        "pwm_window":  [0] * WINDOW_MAX,
        "window_head": 0, "window_filled": 0,
    }
    print("tick,now_ms,temp,pwm,window_filled,state,persist_count,recovery_count")
    for t in range(N_TICKS):
        if t < 10:
            temp = 80000 + t * 100
            pwm = 220
        else:
            temp = 80900
            pwm = 100
        runaway_step(s, temp, pwm, t * TICK_MS)
        print(f"{t},{t * TICK_MS},{temp},{pwm},"
              f"{s['window_filled']},{s['state']},"
              f"{s['persist_count']},{s['recovery_count']}")


if __name__ == "__main__":
    main()
