#!/usr/bin/env python3
"""Pure-integer Python reference for thermal_fault_stall_step (PRD §4.7)."""

N_TICKS = 50
TICK_MS = 100

# Detector cfg
PERSIST   = 5
RECOVERY  = 3
STALL_RPM = 200    # threshold0
STALL_PWM = 80     # threshold1
SEVERITY  = 1      # DEGRADED
ACTION    = 3      # FORCE_PWM_MAX_UNTIL_RECOVERED

# Fault states
NORMAL, DEGRADED, CRITICAL, LATCHED, RECOVERING = 0, 1, 2, 3, 4


def action_to_state(action, severity):
    if action in (4, 5):    # FORCE_PWM_MAX_AND_LATCH, REQUEST_SHUTDOWN
        return LATCHED
    if severity == 2:       # CRITICAL
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


def stall_step(s, pwm, tach, valid, spinup_ticks, now_ms):
    pwm_is_zero = (pwm == 0)
    if s["prev_pwm_was_zero"] and not pwm_is_zero:
        s["spinup_remaining"] = spinup_ticks
    s["prev_pwm_was_zero"] = 1 if pwm_is_zero else 0

    in_spinup = s["spinup_remaining"] > 0
    if in_spinup:
        s["spinup_remaining"] -= 1

    cond = (
        (not in_spinup)
        and valid
        and pwm >= STALL_PWM
        and tach < STALL_RPM
    )
    fsm_tick(s, 1 if cond else 0, SEVERITY, ACTION, PERSIST, RECOVERY, now_ms)


def main():
    s = {
        "state": NORMAL, "persist_count": 0, "recovery_count": 0,
        "entered_ts_ms": 0, "spinup_remaining": 0, "prev_pwm_was_zero": 1,
    }
    print("tick,now_ms,pwm,tach,tach_valid,state,persist_count,recovery_count,spinup_remaining")
    for t in range(N_TICKS):
        pwm = 0 if t < 5 else 100
        tach = 0 if t < 30 else 500
        valid = 1
        stall_step(s, pwm, tach, valid, 10, t * TICK_MS)
        print(f"{t},{t * TICK_MS},{pwm},{tach},{valid},"
              f"{s['state']},{s['persist_count']},{s['recovery_count']},"
              f"{s['spinup_remaining']}")


if __name__ == "__main__":
    main()
