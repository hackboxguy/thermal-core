#!/usr/bin/env python3
"""tools/thermalcore-scenario/check_determinism.py

Stage 12 12d determinism gate.  Two modes:

  --twice:           every deterministic .scn runs twice; SHA-256
                     over the captured CSV must match across runs.

  --cross-compiler:  rebuild the daemon + libplant under CC=gcc
                     and CC=clang in turn; SHA-256s of the
                     captured CSVs must match across compilers.

Only deterministic (non-CAN) scenarios participate.  CAN
scenarios depend on car-can-emulator's wall-clock-driven CAN
broadcast cadence which v1 doesn't promise to bit-deterministic.

The daemon runs under --clock=scenario so its tick cadence is
fully orchestrator-driven; the plant simulator is Q16.16
deterministic by 12a's contract; the probe's CSV is
byte-stable by 12b's contract.  Together these mean two runs
(or two compilers) MUST produce identical CSV bytes; any
divergence is a real determinism regression.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def scenario_csv(scn: Path) -> Path:
    return Path(f"/tmp/scenario-{scn.stem}.csv")


def is_can_scenario(scn: Path) -> bool:
    """Read the scenario's `config <path>` directive and check
    whether the named daemon config has any `canbus:` context
    source."""
    cfg_rel = None
    for raw in scn.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if line.startswith("config "):
            cfg_rel = line.split(None, 1)[1].strip()
            break
    if cfg_rel is None:
        return False
    cfg_path = ROOT / cfg_rel
    if not cfg_path.exists():
        return False
    try:
        cfg = json.loads(cfg_path.read_text())
    except (OSError, ValueError):
        return False
    for c in cfg.get("context_signals", []):
        if str(c.get("source", "")).startswith("canbus:"):
            return True
    return False


def deterministic_scenarios() -> list[Path]:
    scns = sorted((ROOT / "scenarios").glob("*.scn"))
    return [s for s in scns if not is_can_scenario(s)]


def run_one(scn: Path) -> None:
    """Run a scenario via run.py; raise on non-zero exit."""
    subprocess.run(
        [sys.executable, str(HERE / "run.py"), str(scn)],
        cwd=ROOT, check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# === Mode A: same-build run-twice =====================================

def check_twice() -> int:
    scns = deterministic_scenarios()
    if not scns:
        print("determinism (--twice): no deterministic scenarios")
        return 1
    failures = []
    for scn in scns:
        run_one(scn)
        csv = scenario_csv(scn)
        if not csv.exists():
            print(f"  FAIL {scn.name}: missing CSV at {csv}")
            failures.append(scn.name)
            continue
        first = sha256(csv)
        run_one(scn)
        second = sha256(csv)
        ok = (first == second)
        marker = "PASS" if ok else "FAIL"
        print(f"  {marker} {scn.name}: run1={first[:12]}  run2={second[:12]}")
        if not ok:
            failures.append(scn.name)
    if failures:
        print(f"determinism (--twice): FAIL ({len(failures)} scenario(s))")
        return 1
    print("determinism (--twice): ALL PASS")
    return 0


# === Mode B: cross-compiler ==========================================

def make(cc: str, target: str) -> None:
    subprocess.run(["make", f"CC={cc}", target],
                    cwd=ROOT, check=True,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build_with(cc: str) -> dict[str, str]:
    """Clean-rebuild + run every deterministic scenario under
    `cc`.  Returns {scn_stem: sha256(csv)}."""
    print(f"  rebuilding with CC={cc} ...")
    make(cc, "clean")
    make(cc, "build")
    # libplant.so is built by the top-level Makefile; the
    # CFLAGS_BASE rule in there picks up $(CC) when CC= is set.
    make(cc, "build/tools/thermalcore-scenario/libplant.so")
    shas: dict[str, str] = {}
    for scn in deterministic_scenarios():
        run_one(scn)
        csv = scenario_csv(scn)
        shas[scn.stem] = sha256(csv)
    return shas


def check_cross_compiler() -> int:
    gcc_shas   = build_with("gcc")
    clang_shas = build_with("clang")
    failures = []
    for stem in sorted(gcc_shas):
        ok = (gcc_shas[stem] == clang_shas.get(stem))
        marker = "PASS" if ok else "FAIL"
        print(f"  {marker} {stem}: "
               f"gcc={gcc_shas[stem][:12]}  "
               f"clang={clang_shas.get(stem, '<missing>')[:12]}")
        if not ok:
            failures.append(stem)
    if failures:
        print(f"determinism (--cross-compiler): "
               f"FAIL ({len(failures)} scenario(s))")
        return 1
    print("determinism (--cross-compiler): ALL PASS")
    return 0


# === Main ============================================================

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="check_determinism")
    parser.add_argument("--twice", action="store_true",
                        help="same-build run-twice SHA-256 check")
    parser.add_argument("--cross-compiler", action="store_true",
                        help="gcc-vs-clang SHA-256 check")
    args = parser.parse_args(argv)
    if not args.twice and not args.cross_compiler:
        parser.error("pick at least one of --twice / --cross-compiler")
    rc = 0
    if args.twice:
        rc |= check_twice()
    if args.cross_compiler:
        rc |= check_cross_compiler()
    return rc


if __name__ == "__main__":
    sys.exit(main())
