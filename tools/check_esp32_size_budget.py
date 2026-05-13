#!/usr/bin/env python3
"""tools/check_esp32_size_budget.py -- enforce PRD section 9.2 size
budgets on the core/ + protocol/ contribution to the ESP32 firmware.

PRD section 9.2 (line 1088) commits the project to:

    ESP32: <= 64 KB text, <= 16 KB bss

These budgets apply to the core/ + protocol/ portion of the linked
firmware (the "thermal-core layer"), not the whole binary -- the
whole binary is dominated by ESP-IDF + drivers, which live under
a separate "70 % free of the 1 MB app partition" loose constraint
documented in impl-plan section 5 Stage 13.

This script:

  1. Walks `core/*.c` + `protocol/*.c` to build the basename set
     (no hardcoded list -- new sources are picked up automatically;
     a removed source no longer counts).
  2. For each `libmain.a:<basename>.obj` entry in the IDF-emitted
     size-files JSON, sums `.flash.text + .iram0.text` into the
     text total and `.dram0.bss` into the bss total.
  3. Prints a per-file breakdown sorted by text descending.
  4. Prints grand totals and the budget verdict.
  5. Exits 1 if either budget is exceeded, 0 otherwise.

Run from the repo root (which contains `core/` and `protocol/`),
or pass `--repo-root` to point elsewhere.

Usage:
    python3 tools/check_esp32_size_budget.py <size-files.json>
                                             [--repo-root PATH]
                                             [--text-budget 65536]
                                             [--bss-budget 16384]

The JSON file is produced by `idf.py size-files --format json
--output-file <path>` inside an ESP-IDF build directory.

See impl-plan section 5 Stage 13d for the CI wiring; the workflow
at .github/workflows/ci.yml runs this script on the STANDALONE
matrix leg only (REPLAY dead-strips drivers/BSPs and would
understate the real footprint).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


TEXT_BUDGET_DEFAULT = 64 * 1024   # PRD section 9.2
BSS_BUDGET_DEFAULT  = 16 * 1024   # PRD section 9.2

# Match `libmain.a:thermal_core.c.obj` and similar.
LIBMAIN_OBJ_RE = re.compile(r"^libmain\.a:(?P<base>.+)\.obj$")


def discover_basenames(repo_root: Path) -> set[str]:
    """Return the set of `.c` basenames under `core/` + `protocol/`."""
    out: set[str] = set()
    for sub in ("core", "protocol"):
        d = repo_root / sub
        if not d.is_dir():
            raise SystemExit(
                f"check_esp32_size_budget: {d} not found; "
                f"pass --repo-root to point at the thermal-core tree")
        for c in d.glob("*.c"):
            out.add(c.name)
    return out


def aggregate(size_files: dict, basenames: set[str]) \
        -> tuple[list[tuple[str, int, int]], int, int]:
    """Return (per_file_rows, total_text, total_bss).

    per_file_rows is a list of (basename, text, bss) for every entry
    in size_files whose basename is in `basenames`, sorted by text
    descending then basename ascending."""
    rows: list[tuple[str, int, int]] = []
    text_total = 0
    bss_total = 0
    for key, val in size_files.items():
        m = LIBMAIN_OBJ_RE.match(key)
        if not m:
            continue
        base = m.group("base")
        if base not in basenames:
            continue
        text = int(val.get(".flash.text", 0)) + int(val.get(".iram0.text", 0))
        bss  = int(val.get(".dram0.bss", 0))
        rows.append((base, text, bss))
        text_total += text
        bss_total  += bss
    rows.sort(key=lambda r: (-r[1], r[0]))
    return rows, text_total, bss_total


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="check_esp32_size_budget")
    parser.add_argument("size_files_json", type=Path,
                        help="path to idf.py size-files --format "
                             "json --output-file <path> output")
    parser.add_argument("--repo-root", type=Path, default=Path("."),
                        help="repository root containing core/ + "
                             "protocol/ (default: cwd)")
    parser.add_argument("--text-budget", type=int,
                        default=TEXT_BUDGET_DEFAULT,
                        help=f"text byte budget (default "
                             f"{TEXT_BUDGET_DEFAULT}, PRD section 9.2)")
    parser.add_argument("--bss-budget", type=int,
                        default=BSS_BUDGET_DEFAULT,
                        help=f"bss byte budget (default "
                             f"{BSS_BUDGET_DEFAULT}, PRD section 9.2)")
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    basenames = discover_basenames(repo_root)

    try:
        with args.size_files_json.open("r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        raise SystemExit(
            f"check_esp32_size_budget: cannot parse "
            f"{args.size_files_json}: {e}")

    rows, text_total, bss_total = aggregate(data, basenames)

    if not rows:
        raise SystemExit(
            f"check_esp32_size_budget: no libmain.a:<basename>.obj "
            f"entries matched core/+protocol/ sources in "
            f"{args.size_files_json}.  Did you build the firmware "
            f"and run `idf.py size-files --format json --output-file`?")

    print(f"Per-source contribution to ESP32 firmware "
          f"(core/ + protocol/, libmain.a):")
    for base, text, bss in rows:
        print(f"   {base:30s} text={text:6d} B   bss={bss:5d} B")

    print(f"                                  -----          -----")
    print(f"   {'TOTAL':30s} text={text_total:6d} B   bss={bss_total:5d} B")
    print(f"   Budget (PRD section 9.2)      "
          f"text<={args.text_budget:<6d}   bss<={args.bss_budget}")

    text_ok = text_total <= args.text_budget
    bss_ok  = bss_total  <= args.bss_budget
    text_verdict = "OK" if text_ok else \
        f"FAIL ({text_total} > {args.text_budget})"
    bss_verdict  = "OK" if bss_ok else \
        f"FAIL ({bss_total} > {args.bss_budget})"
    print(f"   {'Verdict':30s} text={text_verdict:<20s} "
          f"bss={bss_verdict}")

    return 0 if (text_ok and bss_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
