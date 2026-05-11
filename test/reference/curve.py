#!/usr/bin/env python3
"""
test/reference/curve.py — pure-integer reference for thermal_curve_eval.

Implements PRD §4.8 line 677 verbatim. Output must match the C
implementation (test/replay/curve_replay) byte-for-byte. Used by the
`replay` CI job and as a sanity check that the C math hasn't drifted
from the documented formula even when goldens are re-baselined.
"""

# Canonical acoustic_mask curve from PRD §5.1 lines 729-734.
CURVE = [
    (0,   120, 0),
    (30,  180, 0),
    (80,  255, -5000),
    (130, 255, -8000),
]


def trunc_div(num: int, den: int) -> int:
    """C99 signed integer division: truncation toward zero.

    Python's `//` floors toward -inf; for mixed-sign operands this
    diverges from C99. Returning sign * (|num| // |den|) restores C99
    semantics.
    """
    sign = -1 if (num < 0) != (den < 0) else 1
    return sign * (abs(num) // abs(den))


def curve_eval(x: int, axis: int) -> int:
    """axis=0 -> value0, axis=1 -> value1."""
    pts = [(p[0], p[1 + axis]) for p in CURVE]
    n = len(pts)
    if n == 0:
        return 0
    if x <= pts[0][0]:
        return pts[0][1]
    if x >= pts[n - 1][0]:
        return pts[n - 1][1]
    i = 0
    while i + 1 < n and x > pts[i + 1][0]:
        i += 1
    x0, y0 = pts[i]
    x1, y1 = pts[i + 1]
    return y0 + trunc_div((x - x0) * (y1 - y0), x1 - x0)


def main() -> None:
    print("x_kmh,y0_pwm_cap,y1_trip_offset_mc")
    for x in range(0, 201):
        print(f"{x},{curve_eval(x, 0)},{curve_eval(x, 1)}")


if __name__ == "__main__":
    main()
