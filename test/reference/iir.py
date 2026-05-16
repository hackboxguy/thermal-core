#!/usr/bin/env python3
"""
test/reference/iir.py — pure-integer Python reference for thermal_filter.

Implements PRD §4.5 lines 554-575 + lifecycle. Output must match the C
implementation (test/replay/filter_replay) byte-for-byte.
"""

N_SAMPLES = 1000
ALPHA_Q16 = 16384   # 0.25 in Q16.16

INT32_MAX = (1 << 31) - 1
INT32_MIN = -(1 << 31)
UINT32_MASK = (1 << 32) - 1


def base_temp(i: int) -> int:
    if i <= 500:
        return 45000 + i * 90
    return 90000 - (i - 500) * 90


def jitter(i: int) -> int:
    # uint32 Knuth multiplicative hash with explicit wraparound.
    h = (i * 2654435761) & UINT32_MASK
    return ((h >> 16) & 0x1FF) - 256


def sample_is_valid(i: int) -> int:
    return 0 if 300 <= i < 350 else 1


def filter_step(state: dict, alpha_q16: int, sample: int, sample_valid: int) -> None:
    if not sample_valid:
        state["valid"] = 0
        return
    if not state["initialized"]:
        state["filtered_value"] = sample
        state["initialized"] = 1
        state["valid"] = 1
        return
    # delta = round(alpha_q16 * (sample - filtered) / 2^16), rounded
    # half-away-from-zero -- mirrors thermal_filter_step exactly.
    product = alpha_q16 * (sample - state["filtered_value"])
    delta = ((product + 32768) >> 16 if product >= 0
             else -(((-product) + 32768) >> 16))
    nxt = state["filtered_value"] + delta
    if nxt > INT32_MAX:
        nxt = INT32_MAX
    elif nxt < INT32_MIN:
        nxt = INT32_MIN
    state["filtered_value"] = nxt
    state["valid"] = 1


def main() -> None:
    state = {"filtered_value": 0, "valid": 0, "initialized": 0}
    print("idx,sample,sample_valid,filtered,filter_valid")
    for i in range(N_SAMPLES):
        sample = base_temp(i) + jitter(i)
        valid = sample_is_valid(i)
        filter_step(state, ALPHA_Q16, sample, valid)
        print(f"{i},{sample},{valid},{state['filtered_value']},{state['valid']}")


if __name__ == "__main__":
    main()
