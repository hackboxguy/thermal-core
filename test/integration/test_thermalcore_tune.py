#!/usr/bin/env python3
"""test/integration/test_thermalcore_tune.py

Daemon ↔ tools/thermalcore-tune end-to-end integration test
(Stage 10 10c).

Spawns thermalcored under --clock=scenario, then runs the
thermalcore-tune CLI as a subprocess for every supported
subcommand against it, asserting the expected ACK/NACK + exit
code given the smoke-config's step_wise zone, configured
acoustic_mask modifier, and absence of an active fault:

    SET_TRIP            -> ACK (exit 0)
    SET_PID             -> NACK INVALID_ARG (step_wise governor)
    SET_SETPOINT        -> NACK INVALID_ARG (step_wise governor)
    SET_CURVE_POINT     -> ACK (acoustic_mask modifier exists)
    CLEAR_FAULT         -> NACK INVALID_ARG (no active fault)
"""
from __future__ import annotations

import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT   = Path(__file__).resolve().parents[2]
DAEMON = ROOT / "build" / "platform-linux" / "thermalcored"
TUNE   = ROOT / "tools" / "thermalcore-tune"
CONFIG = ROOT / "test" / "smoke" / "smoke-config.json"

SMOKE_DIR  = Path("/tmp/thermal-core-integ")
CLOCK_PATH = "/tmp/thermalcored-integ-clock.sock"

CONTROL_HOST = "127.0.0.1"
CONTROL_PORT = 9002


def fail(msg: str) -> None:
    sys.stderr.write(f"FAIL: {msg}\n")
    sys.exit(1)


def setup_tmpfs() -> None:
    (SMOKE_DIR / "sensors").mkdir(parents=True, exist_ok=True)
    (SMOKE_DIR / "contexts").mkdir(parents=True, exist_ok=True)
    (SMOKE_DIR / "sensors" / "soc").write_text("75000\n")
    (SMOKE_DIR / "contexts" / "vehicle_speed").write_text("120\n")
    (SMOKE_DIR / "pwm1").write_text("0\n")
    (SMOKE_DIR / "fan1_input").write_text("1500\n")
    try:
        os.unlink(CLOCK_PATH)
    except FileNotFoundError:
        pass


def write_integ_config() -> Path:
    """Same as smoke-config.json but uses /tmp/thermal-core-integ paths
    so the integration test runs side-by-side with smoke without file
    contention."""
    src = CONFIG.read_text()
    src = src.replace("/tmp/thermal-core-smoke", str(SMOKE_DIR))
    dest = Path("/tmp/thermalcored-integ-config.json")
    dest.write_text(src)
    return dest


def run_tune(*args: str, expect_exit: int, label: str) -> str:
    """Run thermalcore-tune with the given subcommand args; assert
    its exit code matches expectations.  Returns stdout."""
    cmd = [sys.executable, str(TUNE),
           "--host", CONTROL_HOST, "--port", str(CONTROL_PORT),
           "--timeout", "2.0", *args]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
    out = proc.stdout.strip()
    err = proc.stderr.strip()
    print(f"  [{label}] rc={proc.returncode} stdout={out!r} stderr={err!r}")
    if proc.returncode != expect_exit:
        fail(f"{label}: expected exit {expect_exit}, got {proc.returncode}\n"
             f"  stdout: {out}\n  stderr: {err}")
    return out


def main() -> int:
    if not DAEMON.exists():
        fail(f"daemon binary missing at {DAEMON} (run `make build` first)")
    if not TUNE.exists():
        fail(f"thermalcore-tune missing at {TUNE}")

    setup_tmpfs()
    integ_config = write_integ_config()

    # Make sure the control port isn't held by a stale smoke daemon.
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.bind((CONTROL_HOST, CONTROL_PORT))
        probe.close()
    except OSError:
        probe.close()
        fail(f"control port {CONTROL_PORT} already in use; kill prior daemon")

    log_path = Path("/tmp/thermalcored-integ.log")
    log = log_path.open("w")
    # Use wall clock so the daemon ticks autonomously every
    # control_period_ms (100 ms in the smoke config).  Each CLI
    # call has a 2 s response timeout, comfortably more than one
    # tick, so the queued CMD_REQUEST gets drained on the next
    # tick after sendto.
    daemon = subprocess.Popen(
        [str(DAEMON),
         f"--config={integ_config}",
         "--clock=wall"],
        stdout=log, stderr=log)

    # Give the daemon a moment to open the control socket.
    time.sleep(0.3)
    if daemon.poll() is not None:
        log.close()
        fail(f"daemon exited early (rc={daemon.returncode})\nsee {log_path}")

    try:
        # === Scenario 1: SET_TRIP -> ACK ============================
        run_tune("--seq", "1",
                 "set-trip", "0", "0", "72000", "2000",
                 expect_exit=0, label="set-trip OK")

        # === Scenario 2: SET_PID -> NACK INVALID_ARG ================
        # Zone is step_wise; SET_PID is PID-only.
        out = run_tune("--seq", "2",
                       "set-pid", "0", "5000", "400", "0",
                       expect_exit=1, label="set-pid NACK")
        if "status=1" not in out:
            fail(f"set-pid: expected status=1 (INVALID_ARG), got {out!r}")

        # === Scenario 3: SET_SETPOINT -> NACK INVALID_ARG ===========
        out = run_tune("--seq", "3",
                       "set-setpoint", "0", "75000",
                       expect_exit=1, label="set-setpoint NACK")
        if "status=1" not in out:
            fail(f"set-setpoint: expected status=1, got {out!r}")

        # === Scenario 4: SET_CURVE_POINT -> ACK =====================
        # acoustic_mask is modifier 0 in the smoke config (curve len 2).
        # Update point 1: x=30000 -> 40000, value0=255 -> 200, value1=0.
        # 40000 > point 0's x (0) so monotonicity is preserved.
        run_tune("--seq", "4",
                 "set-curve-point", "0", "1", "40000", "200", "0",
                 expect_exit=0, label="set-curve-point OK")

        # === Scenario 5: CLEAR_FAULT with bad target_id -> NACK ====
        # fault_type=1 (STALL), target_id=99 (no such actuator).
        # PRD §7.5 line 980: "CMD_CLEAR_FAULT for a (fault_type,
        # target_id) pair that does not correspond to a configured
        # detector instance returns THERMAL_ERR_INVALID_ARG".
        out = run_tune("--seq", "5",
                       "clear-fault", "1", "99",
                       expect_exit=1, label="clear-fault NACK (bad target)")
        if "status=1" not in out:
            fail(f"clear-fault: expected status=1 (INVALID_ARG), got {out!r}")

    finally:
        daemon.send_signal(signal.SIGTERM)
        try:
            rc = daemon.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            daemon.kill()
            rc = -1
        log.close()

    if rc != 0:
        fail(f"daemon exit code {rc}\nsee {log_path}")

    print("integration: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
