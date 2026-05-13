#!/usr/bin/env python3
"""test/integration/test_thermalcore_tune_pid.py

Closes the codex v7 finding #4 gap: validate the Stage 10
PID-positive tuning path end-to-end.

Spawns thermalcored under --clock=wall with a PID-zone config
(sensor reading 80000 mc, setpoint 75000 mc -> 5000 mc error
-> non-zero PID output before tuning).  Then drives:

  1. `thermalcore-tune --config <pid-config> set-pid soc_zone 0 0 0`
     -> CMD_ACK (exercises the new --config name resolution flow:
        `soc` -> zone slot 0).

  2. Wait two ticks; collect telemetry.

  3. Locate a TEVENT_COMMAND_APPLIED event whose cmd_id arg is
     0x0001 (CMD_SET_PID) — confirms the apply path fired.

  4. Assert every TSIG_PID_OUTPUT frame captured after the apply
     event is 0 (all-zero gains drive the output to zero).

  5. Assert every TSIG_PID_INTEGRAL frame captured after the apply
     event is 0 — PRD section 7.5 line 976 requires SET_PID to
     reset the integral accumulator on accept.

The combination (4) + (5) proves the new gains were both accepted
*and* reflected in subsequent telemetry, matching the impl-plan
section 5 Stage 10 contract.
"""
from __future__ import annotations

import os
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT   = Path(__file__).resolve().parents[2]
DAEMON = ROOT / "build" / "platform-linux" / "thermalcored"
TUNE   = ROOT / "tools" / "thermalcore-tune"
CONFIG = ROOT / "test" / "integration" / "pid-config.json"

# Shared wire codec.
sys.path.insert(0, str(ROOT / "tools"))
import thermalcore_wire as w   # noqa: E402

INTEG_DIR = Path("/tmp/thermal-core-pid-integ")

UDP_HOST         = "127.0.0.1"
UDP_TELEM_PORT   = 9010
UDP_CONTROL_PORT = 9012

# PID telemetry signal IDs (PRD-locked; TSIG_PID_BASE = 0x0300 +
# zone*4 + term).  Zone 0:
TSIG_PID_ERROR_0      = 0x0300
TSIG_PID_INTEGRAL_0   = 0x0301
TSIG_PID_DERIVATIVE_0 = 0x0302
TSIG_PID_OUTPUT_0     = 0x0303

TEVENT_COMMAND_APPLIED = 0x1200
CMD_SET_PID            = 0x0001


def fail(msg: str) -> None:
    sys.stderr.write(f"FAIL: {msg}\n")
    sys.exit(1)


def setup_tmpfs() -> None:
    (INTEG_DIR / "sensors").mkdir(parents=True, exist_ok=True)
    (INTEG_DIR / "contexts").mkdir(parents=True, exist_ok=True)
    # Sensor at 80000 mc; setpoint in the config is 75000 mc.
    # Error = +5000 mc -> non-zero P-term -> non-zero PID output
    # with the initial gains (kp_q16=4915 ~= 0.075 PWM/mc).
    (INTEG_DIR / "sensors" / "soc").write_text("80000\n")
    (INTEG_DIR / "contexts" / "vehicle_speed").write_text("0\n")
    (INTEG_DIR / "pwm1").write_text("0\n")
    (INTEG_DIR / "fan1_input").write_text("1500\n")


def collect_frames(sock: socket.socket, deadline_s: float):
    """Drain UDP frames until the deadline; decode each via the
    shared wire codec.  Returns the list of decoded tuples
    (opcode, seq, ts_ms, payload), skipping malformed frames."""
    frames = []
    while True:
        remaining = deadline_s - time.time()
        if remaining <= 0:
            break
        sock.settimeout(remaining)
        try:
            data, _ = sock.recvfrom(256)
        except socket.timeout:
            break
        dec = w.decode_tc(data, crc=True)
        if dec is not None:
            frames.append(dec)
    return frames


def main() -> int:
    if not DAEMON.exists():
        fail(f"daemon binary missing at {DAEMON} (run `make build` first)")
    if not TUNE.exists():
        fail(f"thermalcore-tune missing at {TUNE}")

    setup_tmpfs()

    # Pre-flight: control port must be free (no leftover daemon).
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.bind((UDP_HOST, UDP_CONTROL_PORT))
        probe.close()
    except OSError:
        probe.close()
        fail(f"control port {UDP_CONTROL_PORT} already in use; kill prior daemon")

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind((UDP_HOST, UDP_TELEM_PORT))

    log_path = Path("/tmp/thermalcored-pid-integ.log")
    log = log_path.open("w")
    daemon = subprocess.Popen(
        [str(DAEMON),
         f"--config={CONFIG}",
         "--clock=wall"],
        stdout=log, stderr=log)

    # Give the daemon a moment to bind the control socket.
    time.sleep(0.3)
    if daemon.poll() is not None:
        log.close()
        udp.close()
        fail(f"daemon exited early (rc={daemon.returncode})\nsee {log_path}")

    try:
        # === Phase 1: baseline.  500 ms => ~5 ticks at 100 ms.
        # Expect at least one TSIG_PID_OUTPUT > 0 since the initial
        # gains + error drive the actuator.
        baseline = collect_frames(udp, time.time() + 0.5)

        saw_baseline_output = False
        for opc, _seq, _ts, payload in baseline:
            if opc != w.OP_TELEM_SAMPLE or len(payload) != 8:
                continue
            sig, _flags, val = struct.unpack("<HHi", payload)
            if sig == TSIG_PID_OUTPUT_0 and val > 0:
                saw_baseline_output = True
                break
        if not saw_baseline_output:
            fail("baseline: no TSIG_PID_OUTPUT > 0 captured "
                 "(check temp_mc > setpoint and PID gains in pid-config.json)")

        # === Phase 2: tune to all-zero gains using the documented
        # name-based CLI form (closes codex v7 finding #3 reproducer).
        cmd = [sys.executable, str(TUNE),
               "--host", UDP_HOST, "--port", str(UDP_CONTROL_PORT),
               "--timeout", "2.0",
               "--config", str(CONFIG),
               "--seq", "1",
               "set-pid", "soc_zone", "0", "0", "0"]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
        out = proc.stdout.strip()
        err = proc.stderr.strip()
        print(f"  [set-pid soc_zone 0 0 0] rc={proc.returncode} "
              f"stdout={out!r} stderr={err!r}")
        if proc.returncode != 0:
            fail(f"set-pid: expected exit 0, got {proc.returncode}: {err}")
        if "OK seq=1 status=0" not in out:
            fail(f"set-pid: expected ACK OK, got: {out}")

        # === Phase 3: post-tune.  Drain until we've seen the apply
        # event + at least one full PID frame set (4 signals).
        # 400 ms => ~4 ticks of telemetry after the command.
        post = collect_frames(udp, time.time() + 0.4)

        saw_apply = False
        post_outputs   = []
        post_integrals = []
        apply_index = None
        for i, (opc, _seq, _ts, payload) in enumerate(post):
            if opc == w.OP_TELEM_EVENT and len(payload) == 18:
                code, a1 = struct.unpack("<HI", payload[0:6])
                if code == TEVENT_COMMAND_APPLIED and a1 == CMD_SET_PID:
                    saw_apply = True
                    apply_index = i
                    break
        if not saw_apply:
            fail("post-tune: no TEVENT_COMMAND_APPLIED (cmd_id=0x0001) "
                 "found in the captured telemetry stream")

        # Frames strictly after the apply event reflect the new gains.
        for opc, _seq, _ts, payload in post[apply_index + 1:]:
            if opc != w.OP_TELEM_SAMPLE or len(payload) != 8:
                continue
            sig, _flags, val = struct.unpack("<HHi", payload)
            if sig == TSIG_PID_OUTPUT_0:
                post_outputs.append(val)
            elif sig == TSIG_PID_INTEGRAL_0:
                post_integrals.append(val)

        if not post_outputs:
            fail("post-tune: no TSIG_PID_OUTPUT frames captured after "
                 "the apply event (telemetry pacing?)")
        non_zero_outputs = [v for v in post_outputs if v != 0]
        if non_zero_outputs:
            fail(f"post-tune: TSIG_PID_OUTPUT not zero after kp=ki=kd=0: "
                 f"{non_zero_outputs!r}")

        if not post_integrals:
            fail("post-tune: no TSIG_PID_INTEGRAL frames captured after "
                 "the apply event")
        non_zero_integrals = [v for v in post_integrals if v != 0]
        if non_zero_integrals:
            fail(f"post-tune: TSIG_PID_INTEGRAL not zero after SET_PID "
                 f"(integral reset per PRD section 7.5 line 976): "
                 f"{non_zero_integrals!r}")

    finally:
        daemon.send_signal(signal.SIGTERM)
        try:
            rc = daemon.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            daemon.kill()
            rc = -1
        log.close()
        udp.close()

    if rc != 0:
        fail(f"daemon exit code {rc}\nsee {log_path}")

    print("integration-pid: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
