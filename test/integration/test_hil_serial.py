#!/usr/bin/env python3
"""test/integration/test_hil_serial.py -- PTY-driven HIL integration test.

Stage 14 14d: closes the bsp_hil_serial.c coverage gap left by 14c
without requiring an ESP32 on a bench.  Opens a Unix PTY pair,
runs thermalcored with `hil.transport = "serial:<pty-slave>"`,
injects crafted `TELEM_SAMPLE` TC frames on the master side
(simulating the ESP32 firmware's sensor + tach broadcasts), and
verifies the daemon emits `CMD_REQUEST(HIL_CMD_SET_PWM_DUTY)`
frames on the same channel as the synthetic temperature climbs
across the configured trip points.

Drives the daemon under `--clock=scenario` + DONE handshake so
the test is strictly synchronous: inject samples -> TICK -> DONE
-> drain output -> repeat.  No wall-clock dependencies.

Exits non-zero on any failed assertion (same convention as the
sibling integration tests).
"""
from __future__ import annotations

import fcntl
import json
import os
import pty
import signal as pysignal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent

# Make the wire codec importable.
sys.path.insert(0, str(ROOT / "tools"))
import thermalcore_wire as w   # noqa: E402

DAEMON_PATH       = ROOT / "build" / "platform-linux" / "thermalcored"
CFG_PATH          = "/tmp/hil-pty-config.json"
CLOCK_PATH        = "/tmp/thermalcored-hil-pty-clock.sock"
DAEMON_LOG_PATH   = "/tmp/thermalcored-hil-pty.log"

UDP_HOST          = "127.0.0.1"
UDP_TELEM_PORT    = 9099

# Canonical HIL signal IDs from core/thermal_signals.h (TSIG_HIL_*).
SIG_SENSOR_TEMP_0 = 0x0700
SIG_TACH_RPM_0    = 0x0710

# Platform-private HIL command ID per PRD section 8.3 (0x8000+).
HIL_CMD_SET_PWM_DUTY = 0x8001

# Trip schedule (matches the test config below):
#   <30 C   -> cooling_state 0 -> duty 0
#   30-44   -> state 1         -> duty 100
#   45-59   -> state 2         -> duty 160
#   60+     -> state 3         -> duty 220
EXPECTED_DUTIES_AT_TRIPS = [100, 160, 220]


def fail(msg: str) -> None:
    sys.stderr.write(f"FAIL: {msg}\n")
    sys.exit(1)


# -------------------------------------------------------------- config

def write_config(slave_path: str) -> None:
    cfg = {
        "config_version":     1,
        "control_period_ms":  100,
        "sensors": [{
            "id": 0, "name": "soc",
            "iir_alpha_q16": 16384,
            "max_staleness_ms": 5000,
            "source": "hil:slot0",
        }],
        "context_signals": [],
        "actuators": [{
            "id": 0, "name": "main_fan",
            "pwm_min": 0, "pwm_max": 255,
            "slew_per_tick": 64,    # generous slew so the test sees crisp duty steps
            "spinup_pwm": 0, "spinup_ms": 0,
            "state_pwm": [0, 100, 160, 220, 255],
            "pwm": "hil:slot0", "tach": "hil:slot0",
            "pwm_freq_hz": 25000, "tach_pulses_per_rev": 2,
        }],
        "zones": [{
            "name": "soc_zone",
            "sensors": ["soc"],
            "aggregation": "max",
            "fallback_temp_mc": 85000,
            "governor": "step_wise",
            "actuators": ["main_fan"],
            "trips": [
                {"temp_mc": 30000, "hyst_mc": 2000,
                 "severity": "warn",     "cooling_state": 1},
                {"temp_mc": 45000, "hyst_mc": 2000,
                 "severity": "warn",     "cooling_state": 2},
                {"temp_mc": 60000, "hyst_mc": 2000,
                 "severity": "critical", "cooling_state": 3},
            ],
        }],
        "policy_modifiers": [],
        "fault_detection": {},
        "telemetry": {
            "enable": True, "period_ticks": 1,
            "signals": ["zone_temp_*", "actuator_pwm_*"],
            "transport": f"udp:{UDP_HOST}:{UDP_TELEM_PORT}",
        },
        "control": {"listen": "", "enable": False},
        "hil":     {"transport": f"serial:{slave_path}"},
    }
    Path(CFG_PATH).write_text(json.dumps(cfg, indent=2))


# -------------------------------------------------------------- helpers

def inject_telem_sample(master_fd: int, seq: int, ts_ms: int,
                        signal_id: int, value: int) -> None:
    payload = struct.pack("<HHi", signal_id, 0, value)   # flags=0
    frame = w.encode_tc(w.OP_TELEM_SAMPLE, seq, ts_ms, payload, crc=True)
    os.write(master_fd, frame)


def recv_done(clk: socket.socket, timeout_s: float = 5.0) -> None:
    """Block until the daemon writes 'DONE\\n' on the scenario clock socket
    (the 9eb9996 handshake)."""
    clk.settimeout(timeout_s)
    buf = b""
    while b"\n" not in buf:
        chunk = clk.recv(64)
        if not chunk:
            fail("scenario clock EOF before DONE")
        buf += chunk
    line = buf.split(b"\n", 1)[0]
    if line != b"DONE":
        fail(f"scenario clock: expected 'DONE', got {line!r}")


def drain_master_cmds(master_fd: int) -> list:
    """Read all bytes currently available on the master fd, scan for
    complete TC frames, return list of (opcode, command_id, duty) for
    every CMD_REQUEST we see and (opcode, status, detail) for ACK/NACK."""
    out = []
    chunks = []
    while True:
        try:
            data = os.read(master_fd, 4096)
        except BlockingIOError:
            break
        if not data:
            break
        chunks.append(data)
    buf = b"".join(chunks)
    if not buf:
        return out
    i = 0
    while True:
        j = buf.find(b"TC", i)
        if j < 0 or j + 14 > len(buf):
            break
        dec = w.decode_tc(buf[j:j + 256], crc=True)
        if dec is None:
            i = j + 1
            continue
        opcode, seq, ts_ms, payload = dec
        # Bump cursor past the frame so the next find() doesn't re-decode.
        i = j + 14 + len(payload) + 2
        if opcode == w.OP_CMD_REQUEST and len(payload) >= 3:
            cmd_id = struct.unpack("<H", payload[0:2])[0]
            duty = payload[2] if len(payload) >= 3 else 0
            out.append(("CMD_REQUEST", cmd_id, duty))
        elif opcode in (w.OP_CMD_ACK, w.OP_CMD_NACK):
            req_seq, status, detail = w.decode_ack_or_nack(payload) or (0, 0, 0)
            kind = "CMD_ACK" if opcode == w.OP_CMD_ACK else "CMD_NACK"
            out.append((kind, status, detail))
    return out


# -------------------------------------------------------------- main

def main() -> int:
    if not DAEMON_PATH.exists():
        fail(f"daemon binary missing at {DAEMON_PATH}; run `make build` first")

    # Pre-clean any stale clock socket from a prior run.
    try:
        os.unlink(CLOCK_PATH)
    except FileNotFoundError:
        pass

    master_fd, slave_fd = pty.openpty()
    slave_path = os.ttyname(slave_fd)
    # Make master non-blocking so drain_master_cmds doesn't hang.
    flags = fcntl.fcntl(master_fd, fcntl.F_GETFL, 0)
    fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    write_config(slave_path)

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind((UDP_HOST, UDP_TELEM_PORT))
    udp.settimeout(0.1)

    daemon_log = Path(DAEMON_LOG_PATH).open("w")
    daemon = None
    clk = None
    cmds_observed: list = []
    try:
        daemon = subprocess.Popen(
            [str(DAEMON_PATH),
             f"--config={CFG_PATH}",
             "--clock=scenario",
             f"--scenario-clock-uri=unix:{CLOCK_PATH}"],
            stdout=daemon_log, stderr=daemon_log)

        # Wait for the daemon to bind the scenario clock socket.
        deadline = time.time() + 5.0
        while not os.path.exists(CLOCK_PATH):
            if time.time() > deadline:
                fail(f"daemon did not bind {CLOCK_PATH} within 5 s; "
                     f"see {DAEMON_LOG_PATH}")
            if daemon.poll() is not None:
                fail(f"daemon exited early (rc={daemon.returncode}); "
                     f"see {DAEMON_LOG_PATH}")
            time.sleep(0.05)

        clk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        clk.connect(CLOCK_PATH)

        # Drive 50 ticks over 5 s scenario time.  Ramp the synthetic
        # sensor temp from 25 C to 75 C linearly.  At control_period_ms
        # = 100 + IIR alpha = 16384 (=0.25), the filter tracks the ramp
        # within a few hundred mc.
        TICK_COUNT = 50
        TEMP_START_MC = 25000
        TEMP_END_MC   = 75000
        seq = 0
        for tick in range(TICK_COUNT):
            now_ms = tick * 100
            temp_mc = TEMP_START_MC + (TEMP_END_MC - TEMP_START_MC) \
                                       * tick // (TICK_COUNT - 1)
            # Inject sensor sample first, then tach (fan-off baseline).
            inject_telem_sample(master_fd, seq, now_ms,
                                SIG_SENSOR_TEMP_0, temp_mc)
            seq = (seq + 1) & 0xFFFF
            inject_telem_sample(master_fd, seq, now_ms,
                                SIG_TACH_RPM_0, 0)
            seq = (seq + 1) & 0xFFFF

            clk.sendall(f"TICK {now_ms}\n".encode("ascii"))
            recv_done(clk)

            # Drain any CMD_REQUEST / ACK / NACK frames the daemon wrote.
            cmds_observed.extend(drain_master_cmds(master_fd))

    finally:
        if clk is not None:
            clk.close()
        if daemon is not None:
            daemon.send_signal(pysignal.SIGTERM)
            try:
                daemon.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                daemon.kill()
        os.close(master_fd)
        os.close(slave_fd)
        daemon_log.close()
        udp.close()

    # === Assertions =====================================================
    nacks = [c for c in cmds_observed if c[0] == "CMD_NACK"]
    if nacks:
        fail(f"unexpected NACKs from daemon: {nacks[:5]}")

    cmd_reqs = [c for c in cmds_observed if c[0] == "CMD_REQUEST"]
    if not cmd_reqs:
        fail(f"no CMD_REQUEST frames seen on PTY master over "
             f"{TICK_COUNT} ticks; see {DAEMON_LOG_PATH}")

    wrong_id = [c for c in cmd_reqs if c[1] != HIL_CMD_SET_PWM_DUTY]
    if wrong_id:
        fail(f"unexpected command_id values: {wrong_id[:5]}")

    duties = [c[2] for c in cmd_reqs]
    print(f"  [test_hil_serial] {len(duties)} CMD_REQUEST frames; "
          f"duty range = {min(duties)}..{max(duties)}")

    if max(duties) < 220:
        fail(f"expected duty to reach 220 (critical state) but "
             f"max observed = {max(duties)}")

    # First few ticks: synthetic temp is 25 C, below the 30 C warn trip,
    # so duty must be 0 (fan off).
    if duties[0] != 0:
        fail(f"expected duty=0 at first tick (temp=25 C, below warn) "
             f"but observed = {duties[0]}")

    # As the temp climbs, duty must visit each cooling-state plateau.
    for state_pwm in EXPECTED_DUTIES_AT_TRIPS:
        if state_pwm not in duties:
            fail(f"duty never crossed expected cooling-state plateau "
                 f"{state_pwm}; observed duty trajectory: {duties}")

    print("test_hil_serial: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
