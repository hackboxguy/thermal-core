#!/usr/bin/env python3
"""
test/reference/pid.py -- pure-integer Python reference for thermal_pid_step.

Mirrors PRD §4.8 PID + the implementation in core/thermal_pid.c. Output
must match test/replay/pid_replay byte-for-byte.

Critical math conventions:
- Integer multiplications happen at "C int64" semantics (Python ints are
  arbitrary precision; we clamp manually to int32 saturation).
- Integer division uses C99 truncation toward zero (sign-preserving abs-
  div helper), not Python's floor-toward-minus-infinity `//`.
- Arithmetic right shift `>> 16` uses Python's `>>`, which floors toward
  -inf -- matches gcc/clang's arithmetic shift on signed int.
- uint32 wraparound for now_ms subtraction: masked with 2**32 - 1.
"""

N_TICKS  = 200
TICK_MS  = 100
KP_Q16   = 819
KI_Q16   = 327
KD_Q16   = 0
SETPOINT = 75000
DT_MIN   = 50
DT_MAX   = 500
PWM_MIN  = 0
PWM_MAX  = 255

INT32_MAX   = (1 << 31) - 1
INT32_MIN   = -(1 << 31)
UINT32_MASK = (1 << 32) - 1


def trunc_div(num: int, den: int) -> int:
    """C99 signed integer division (truncation toward zero)."""
    sign = -1 if (num < 0) != (den < 0) else 1
    return sign * (abs(num) // abs(den))


def clamp_i32(v: int) -> int:
    if v > INT32_MAX:
        return INT32_MAX
    if v < INT32_MIN:
        return INT32_MIN
    return v


def synth_temp(t: int) -> int:
    return 50000 if t < 10 else 85000


def pid_step(state, kp, ki, kd, setpoint, temp, now_ms, dt_min, dt_max,
             pwm_min, pwm_max):
    # dt
    if state["initialized"]:
        dt = (now_ms - state["prev_now_ms"]) & UINT32_MASK
    else:
        dt = dt_min
    if dt < dt_min:
        dt = dt_min
    if dt > dt_max:
        dt = dt_max

    error = temp - setpoint
    p_q16 = kp * error
    i_inc = trunc_div(ki * error * dt, 1000)

    i_tent = clamp_i32(state["integral_q16"] + i_inc)

    d_q16 = 0
    if state["initialized"]:
        d_temp = temp - state["prev_temp_mc"]
        d_q16 = trunc_div(kd * d_temp * 1000, dt)

    output_q16 = clamp_i32(p_q16 + i_tent + d_q16)
    output_pwm = output_q16 >> 16

    sat_hi = 0
    sat_lo = 0
    if output_pwm > pwm_max:
        output_pwm = pwm_max
        sat_hi = 1
    if output_pwm < pwm_min:
        output_pwm = pwm_min
        sat_lo = 1

    commit = True
    if sat_hi and i_inc > 0:
        commit = False
    if sat_lo and i_inc < 0:
        commit = False
    if commit:
        state["integral_q16"] = i_tent

    state["prev_temp_mc"] = temp
    state["prev_now_ms"] = now_ms
    state["initialized"] = 1

    return {
        "p": clamp_i32(p_q16),
        "i": state["integral_q16"],
        "d": clamp_i32(d_q16),
        "output_pwm": output_pwm,
        "sat_hi": sat_hi,
        "sat_lo": sat_lo,
    }


def main() -> None:
    state = {
        "integral_q16": 0, "prev_temp_mc": 0, "prev_now_ms": 0, "initialized": 0,
    }
    print("tick,now_ms,temp_mc,error_mc,p_q16,i_q16,d_q16,output_pwm,sat_hi,sat_lo")
    for t in range(N_TICKS):
        now_ms = t * TICK_MS
        temp = synth_temp(t)
        r = pid_step(state, KP_Q16, KI_Q16, KD_Q16, SETPOINT, temp,
                     now_ms, DT_MIN, DT_MAX, PWM_MIN, PWM_MAX)
        print(f"{t},{now_ms},{temp},{temp - SETPOINT},"
              f"{r['p']},{r['i']},{r['d']},"
              f"{r['output_pwm']},{r['sat_hi']},{r['sat_lo']}")


if __name__ == "__main__":
    main()
