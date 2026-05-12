#!/usr/bin/env python3
"""
test/property/run_property.py — property test driver for validate_config.

Runs build/property/property_config across N seeds and asserts every
returned status is one of the documented thermal_status_t values
(THERMAL_OK..THERMAL_ERR_REJECTED_SAFETY, i.e., 0..7). Anything else
is a property violation: undocumented return, crash, hang.

Prints a histogram of observed status codes for visibility — useful for
spotting "validator always returns the same status" regressions even
though those wouldn't strictly violate the property.
"""
import subprocess
import sys
from pathlib import Path

VALID_STATUS = {0, 1, 2, 3, 4, 5, 6, 7}
N_SEEDS = 1000
BIN = Path("build/property/property_config")
TIMEOUT_S = 30


def main() -> int:
    if not BIN.exists():
        print(f"FAIL: {BIN} not built (run `make property`)", file=sys.stderr)
        return 1

    try:
        proc = subprocess.run(
            [str(BIN), str(N_SEEDS)],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_S,
            check=False,
        )
    except subprocess.TimeoutExpired:
        print(f"FAIL: property binary timed out after {TIMEOUT_S}s", file=sys.stderr)
        return 1

    if proc.returncode != 0:
        print(f"FAIL: property binary exited {proc.returncode}", file=sys.stderr)
        if proc.stderr:
            print(proc.stderr, file=sys.stderr)
        return 1

    lines = proc.stdout.splitlines()
    if not lines or lines[0] != "seed,status":
        print("FAIL: missing or wrong CSV header", file=sys.stderr)
        return 1
    if len(lines) - 1 != N_SEEDS:
        print(
            f"FAIL: expected {N_SEEDS} rows, got {len(lines) - 1}",
            file=sys.stderr,
        )
        return 1

    histogram: dict[int, int] = {}
    for line in lines[1:]:
        parts = line.split(",")
        if len(parts) != 2:
            print(f"FAIL: malformed row: {line!r}", file=sys.stderr)
            return 1
        seed_s, status_s = parts
        try:
            status = int(status_s)
        except ValueError:
            print(
                f"FAIL seed {seed_s}: non-integer status {status_s!r}",
                file=sys.stderr,
            )
            return 1
        if status not in VALID_STATUS:
            print(
                f"FAIL seed {seed_s}: undocumented status {status}",
                file=sys.stderr,
            )
            return 1
        histogram[status] = histogram.get(status, 0) + 1

    sorted_hist = dict(sorted(histogram.items()))
    print(f"PASS: {N_SEEDS} configs, status distribution: {sorted_hist}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
