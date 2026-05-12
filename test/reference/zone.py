#!/usr/bin/env python3
"""
test/reference/zone.py — pure-integer Python reference for Stage 4's
heat-soak module golden.

Mirrors thermal_core_step's filter dispatch + zone aggregation + step-
wise governor for a 1-sensor, 1-zone config with 3 trips and passthrough
filter alpha. Output must match test/replay/zone_replay byte-for-byte.

Aggregation in this golden is trivial (single sensor, MAX mode -> the
filtered value itself), so the test focuses on the governor's hysteresis
and the wiring path. IIR drift is exercised by the separate
filter_sweep golden (test/reference/iir.py).
"""

N_TICKS = 600
TICK_MS = 100
Q16_ONE = 0x00010000
ALPHA = Q16_ONE  # passthrough

INT32_MAX = (1 << 31) - 1
INT32_MIN = -(1 << 31)

# Trips: (temp_mc, hyst_mc, cooling_state). Severities are noted in the
# C config but don't affect cooling_state in step-wise mode.
TRIPS = [
    (60000, 2000, 1),
    (75000, 2000, 2),
    (85000, 2000, 3),
]


def synth_temp(t: int) -> int:
    """Triangle wave 45000 mc -> 90000 mc -> 45000 mc over 600 ticks."""
    if t <= 300:
        return 45000 + t * 150
    return 90000 - (t - 300) * 150


def filter_step(state: dict, alpha_q16: int, sample: int, sample_valid: int) -> None:
    """Mirror of thermal_filter_step. Passthrough when alpha == Q16_ONE."""
    if not sample_valid:
        state["valid"] = 0
        return
    if not state["initialized"]:
        state["filtered_value"] = sample
        state["initialized"] = 1
        state["valid"] = 1
        return
    delta = (alpha_q16 * (sample - state["filtered_value"])) >> 16
    nxt = state["filtered_value"] + delta
    if nxt > INT32_MAX:
        nxt = INT32_MAX
    elif nxt < INT32_MIN:
        nxt = INT32_MIN
    state["filtered_value"] = nxt
    state["valid"] = 1


def governor_step_wise(temp: int, prev_mask: int) -> tuple[int, int]:
    """Mirror of thermal_governor_step_wise: Schmitt-trigger hysteresis."""
    new_mask = 0
    max_cs = 0
    for i, (t_temp, t_hyst, t_cs) in enumerate(TRIPS):
        bit = 1 << i
        prev = 1 if (prev_mask & bit) else 0
        if temp >= t_temp:
            active = 1
        elif temp < t_temp - t_hyst:
            active = 0
        else:
            active = prev
        if active:
            new_mask |= bit
            if t_cs > max_cs:
                max_cs = t_cs
    return new_mask, max_cs


def main() -> None:
    fstate = {"filtered_value": 0, "valid": 0, "initialized": 0}
    prev_mask = 0
    print("tick,now_ms,zone_temp,active_trip_mask,cooling_state")
    for t in range(N_TICKS):
        sample = synth_temp(t)
        filter_step(fstate, ALPHA, sample, 1)
        # max-aggregation over a single sensor = the filtered value
        zone_temp = fstate["filtered_value"]
        new_mask, cs = governor_step_wise(zone_temp, prev_mask)
        prev_mask = new_mask
        print(f"{t},{t * TICK_MS},{zone_temp},{new_mask},{cs}")


if __name__ == "__main__":
    main()
