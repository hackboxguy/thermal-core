#!/usr/bin/env python3
"""
test/property/run_property_command.py — property test driver for
thermal_core_apply_command (Stage 8 8b).

Runs build/property/property_command across N seeds and asserts:
  - every returned status is in {OK, INVALID_ARG, BOUNDS,
    REJECTED_SAFETY} -- the documented apply_command set per PRD
    section 7.5 (NOT the broader validate_config {0..7} set).
  - every call emits exactly one log_event (event_count == 1).

Anything else is a property violation: undocumented return, missing
or extra event, crash, or hang.

Prints a status-distribution histogram on PASS.
"""
import subprocess
import sys
from pathlib import Path

# apply_command's documented status set per PRD section 7.5:
#   THERMAL_OK                  = 0
#   THERMAL_ERR_INVALID_ARG     = 1
#   THERMAL_ERR_BOUNDS          = 3
#   THERMAL_ERR_REJECTED_SAFETY = 7
VALID_STATUS = {0, 1, 3, 7}
N_SEEDS = 1000
BIN = Path("build/property/property_command")
TIMEOUT_S = 30


def main() -> int:
    if not BIN.exists():
        print(f"FAIL: {BIN} not built (run `make property-command`)", file=sys.stderr)
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
    if not lines or lines[0] != "seed,command_id,status,event_count":
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
        if len(parts) != 4:
            print(f"FAIL: malformed row: {line!r}", file=sys.stderr)
            return 1
        seed_s, cmd_s, status_s, events_s = parts
        try:
            status = int(status_s)
            events = int(events_s)
        except ValueError:
            print(
                f"FAIL seed {seed_s}: non-integer field in row {line!r}",
                file=sys.stderr,
            )
            return 1
        if status not in VALID_STATUS:
            print(
                f"FAIL seed {seed_s} (command_id={cmd_s}): "
                f"undocumented status {status}",
                file=sys.stderr,
            )
            return 1
        if events != 1:
            print(
                f"FAIL seed {seed_s} (command_id={cmd_s}, status={status}): "
                f"event_count {events} != 1",
                file=sys.stderr,
            )
            return 1
        histogram[status] = histogram.get(status, 0) + 1

    sorted_hist = dict(sorted(histogram.items()))
    print(f"PASS: {N_SEEDS} commands, status distribution: {sorted_hist}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
