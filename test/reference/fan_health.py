#!/usr/bin/env python3
"""
test/reference/fan_health.py -- pure-integer reference for the fan-health
detector (Stage 17, PRD Appendix C).

Re-implements core/thermal_fan_health.c with explicit C99 integer
semantics. Output must match the C driver (test/replay/fan_health_replay)
byte-for-byte; the `replay` CI job diffs them. This is the executable
spec for the delta / EMA / aggregate / severity math.
"""

# --- Scenario: mirrors test/replay/fan_health_replay.c -----------------
PWMS = (64, 128, 192)
RPM = (
    (1300, 2600, 3900),   # healthy   -- delta   0
    (1196, 2392, 3588),   # aging     -- delta  -8
    (1040, 2080, 3120),   # degraded  -- delta -20
    (806, 1612, 2418),    # failing   -- delta -38
)
TICKS_PER_POINT = 40

CFG = {
    "enable": 1,
    "baseline_source": 0,                       # field
    "baseline": [(64, 1300), (128, 2600), (192, 3900)],
    "baseline_count": 3,
    "stable_pwm_ticks": 3,
    "stable_pwm_tolerance": 2,
    "stable_rpm_ticks": 2,
    "stable_rpm_tolerance_pct": 5,
    "min_points_observed": 2,
    "aging_pct": -5,
    "degraded_pct": -15,
    "failing_pct": -30,
}

SEV_HEALTHY, SEV_AGING, SEV_DEGRADED, SEV_FAILING = 0, 1, 2, 3
BASELINE_SRC_MODEL = 2


def c_div(num: int, den: int) -> int:
    """C99 signed integer division: truncation toward zero."""
    sign = -1 if (num < 0) != (den < 0) else 1
    return sign * (abs(num) // abs(den))


def div_round_half_away(num: int, den: int) -> int:
    """Round num/den to the nearest integer, halves away from zero.
    `den` must be > 0. Mirrors div_round_half_away in thermal_fan_health.c."""
    if num >= 0:
        return c_div(2 * num + den, 2 * den)
    return c_div(2 * num - den, 2 * den)


def curve_eval(baseline, count, x):
    """Mirror thermal_curve_eval_y0: clamp at the endpoints, otherwise
    linearly interpolate with C99 truncation."""
    if count == 0:
        return 0
    if x <= baseline[0][0]:
        return baseline[0][1]
    if x >= baseline[count - 1][0]:
        return baseline[count - 1][1]
    i = 0
    while i + 1 < count and x > baseline[i + 1][0]:
        i += 1
    x0, y0 = baseline[i]
    x1, y1 = baseline[i + 1]
    return y0 + c_div((x - x0) * (y1 - y0), x1 - x0)


def nearest_point(cfg, pwm):
    best, best_d = 0, -1
    for i in range(cfg["baseline_count"]):
        d = abs(pwm - cfg["baseline"][i][0])
        if best_d < 0 or d < best_d:
            best_d, best = d, i
    return best


def new_state():
    return {
        "have_ref": 0, "ref_pwm": 0, "pwm_run": 0,
        "prev_rpm": 0, "rpm_run": 0,
        "prev_pwm_low": 1, "spinup_remaining": 0, "prev_override": 0,
        "ema_x256": [0] * len(CFG["baseline"]),
        "sample_count": [0] * len(CFG["baseline"]),
        "health_delta_pct": 0, "severity": SEV_HEALTHY, "confidence": 0,
    }


def recompute(s, cfg):
    sum_wx, sum_w, conf = 0, 0, 0
    for p in range(cfg["baseline_count"]):
        if s["sample_count"][p] == 0:
            continue
        conf += 1
        sc = min(s["sample_count"][p], 16)
        emph = 1 if (p == 0 or p == cfg["baseline_count"] - 1) else 2
        w = sc * emph
        # Positive drift is not health improvement (PRD C.1): clamp each
        # point to its non-positive part before folding into the score.
        ema = min(s["ema_x256"][p], 0)
        sum_w += w
        sum_wx += w * ema
    s["confidence"] = conf
    s["health_delta_pct"] = (div_round_half_away(sum_wx, sum_w * 256)
                             if sum_w > 0 else 0)
    if s["confidence"] < cfg["min_points_observed"]:
        s["severity"] = SEV_HEALTHY
        return
    d = s["health_delta_pct"]
    if d <= cfg["failing_pct"]:
        s["severity"] = SEV_FAILING
    elif d <= cfg["degraded_pct"]:
        s["severity"] = SEV_DEGRADED
    elif cfg["baseline_source"] != BASELINE_SRC_MODEL and d <= cfg["aging_pct"]:
        s["severity"] = SEV_AGING
    else:
        s["severity"] = SEV_HEALTHY


def step(s, cfg, applied_pwm, tach_rpm, tach_valid,
         slew_limited, fault_override, spinup_ticks):
    if not cfg["enable"] or cfg["baseline_count"] < 2:
        return
    pwm_min = cfg["baseline"][0][0]

    pwm_low = 1 if applied_pwm < pwm_min else 0
    if s["prev_pwm_low"] and not pwm_low:
        s["spinup_remaining"] = spinup_ticks
    s["prev_pwm_low"] = pwm_low
    in_spinup = 1 if s["spinup_remaining"] > 0 else 0
    if in_spinup:
        s["spinup_remaining"] -= 1

    override_window = 1 if (fault_override or s["prev_override"]) else 0
    s["prev_override"] = fault_override

    if (not tach_valid) or pwm_low or in_spinup or slew_limited or override_window:
        s["have_ref"] = 0
        s["pwm_run"] = 0
        s["rpm_run"] = 0
        return

    if not s["have_ref"]:
        s["have_ref"] = 1
        s["ref_pwm"] = applied_pwm
        s["pwm_run"] = 1
        s["prev_rpm"] = tach_rpm
        s["rpm_run"] = 1
        return

    if abs(applied_pwm - s["ref_pwm"]) > cfg["stable_pwm_tolerance"]:
        s["ref_pwm"] = applied_pwm
        s["pwm_run"] = 1
        s["prev_rpm"] = tach_rpm
        s["rpm_run"] = 1
        return
    if s["pwm_run"] < 0xFFFF:
        s["pwm_run"] += 1

    rpm_change = abs(tach_rpm - s["prev_rpm"])
    rpm_tol = cfg["stable_rpm_tolerance_pct"] * s["prev_rpm"]
    if rpm_change * 100 <= rpm_tol:
        if s["rpm_run"] < 0xFFFF:
            s["rpm_run"] += 1
    else:
        s["rpm_run"] = 1
    s["prev_rpm"] = tach_rpm

    if s["pwm_run"] < cfg["stable_pwm_ticks"] or s["rpm_run"] < cfg["stable_rpm_ticks"]:
        return

    expected = curve_eval(cfg["baseline"], cfg["baseline_count"], applied_pwm)
    if expected <= 0:
        return

    delta = div_round_half_away((tach_rpm - expected) * 100, expected)
    delta = max(-127, min(127, delta))

    p = nearest_point(cfg, applied_pwm)
    target = delta * 256
    if s["sample_count"][p] == 0:
        s["ema_x256"][p] = target
    else:
        s["ema_x256"][p] += c_div(target - s["ema_x256"][p], 8)
    if s["sample_count"][p] < 0xFFFF:
        s["sample_count"][p] += 1

    recompute(s, cfg)


def main() -> None:
    s = new_state()
    print("tick,applied_pwm,tach_rpm,health_delta_pct,severity,confidence")
    tick = 0
    for ph in range(4):
        for p in range(3):
            for _ in range(TICKS_PER_POINT):
                step(s, CFG, PWMS[p], RPM[ph][p], 1, 0, 0, 0)
                print(f"{tick},{PWMS[p]},{RPM[ph][p]},"
                      f"{s['health_delta_pct']},{s['severity']},"
                      f"{s['confidence']}")
                tick += 1


if __name__ == "__main__":
    main()
