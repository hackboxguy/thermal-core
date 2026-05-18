#!/usr/bin/env python3
"""test/integration/test_hil_serial.py -- PTY-driven HIL integration test.

Stage 14 14d: closes the bsp_hil_serial.c coverage gap left by 14c
without requiring an ESP32 on a bench.  Opens a Unix PTY pair,
runs thermalcored with `hil.transport = "serial:<pty-slave>"`,
injects crafted `TELEM_SAMPLE` TC frames on the master side
(simulating the ESP32 firmware's sensor + tach broadcasts), and
verifies the daemon's behaviour.

Two scenarios:
  - ramp:      synthetic temp climbs through the trip points; the
               daemon must emit CMD_REQUEST(HIL_CMD_SET_PWM_DUTY)
               frames that visit each cooling-state plateau.
  - staleness: one cold reading, then the stream stalls; the
               cached sample must expire after max_staleness_ms so
               the zone falls back to fallback_temp_mc and the fan
               is driven to the critical-state plateau (the
               codex-v8 finding-1 regression guard).

Drives the daemon under `--clock=scenario` + DONE handshake so
each scenario is strictly synchronous: inject -> TICK -> DONE ->
drain output -> repeat.  No wall-clock dependencies.

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

DAEMON_PATH = ROOT / "build" / "platform-linux" / "thermalcored"
CFG_PATH    = "/tmp/hil-pty-config.json"
CLOCK_PATH  = "/tmp/thermalcored-hil-pty-clock.sock"

UDP_HOST       = "127.0.0.1"
UDP_TELEM_PORT = 9099

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

# config max_staleness_ms -- the HIL cache freshness window.
MAX_STALENESS_MS = 5000


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
            "max_staleness_ms": MAX_STALENESS_MS,
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
    complete TC frames, return list of (opcode, command_id, slot, duty) for
    every CMD_REQUEST we see and (kind, status, detail) for ACK/NACK."""
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
        if opcode == w.OP_CMD_REQUEST and len(payload) >= 4:
            # 4-byte HIL_CMD_SET_PWM_DUTY payload:
            # u16 cmd_id LE + u8 slot + u8 duty.
            cmd_id = struct.unpack("<H", payload[0:2])[0]
            slot = payload[2]
            duty = payload[3]
            out.append(("CMD_REQUEST", cmd_id, slot, duty))
        elif opcode in (w.OP_CMD_ACK, w.OP_CMD_NACK):
            req_seq, status, detail = w.decode_ack_or_nack(payload) or (0, 0, 0)
            kind = "CMD_ACK" if opcode == w.OP_CMD_ACK else "CMD_NACK"
            out.append((kind, status, detail))
    return out


# -------------------------------------------------------------- session

def run_session(name: str, tick_count: int, tick_callback) -> list:
    """Open a PTY, run thermalcored under the scenario clock for
    `tick_count` ticks (control_period_ms = 100, so now_ms = tick*100),
    calling tick_callback(tick, now_ms, master_fd, seq) -> seq each
    tick to inject frames.  Returns a per-tick list: element `t` is
    the list of CMD/ACK/NACK tuples the daemon emitted on tick `t`."""
    log_path = f"/tmp/thermalcored-hil-pty-{name}.log"

    master_fd, slave_fd = pty.openpty()
    slave_path = os.ttyname(slave_fd)
    # Non-blocking master so drain_master_cmds doesn't hang.
    flags = fcntl.fcntl(master_fd, fcntl.F_GETFL, 0)
    fcntl.fcntl(master_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    write_config(slave_path)

    try:
        os.unlink(CLOCK_PATH)
    except FileNotFoundError:
        pass

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind((UDP_HOST, UDP_TELEM_PORT))
    udp.settimeout(0.1)

    daemon_log = Path(log_path).open("w")
    daemon = None
    clk = None
    per_tick: list = []
    try:
        daemon = subprocess.Popen(
            [str(DAEMON_PATH),
             f"--config={CFG_PATH}",
             "--clock=scenario",
             f"--scenario-clock-uri=unix:{CLOCK_PATH}"],
            stdout=daemon_log, stderr=daemon_log)

        deadline = time.time() + 5.0
        while not os.path.exists(CLOCK_PATH):
            if time.time() > deadline:
                fail(f"[{name}] daemon did not bind {CLOCK_PATH} within "
                     f"5 s; see {log_path}")
            if daemon.poll() is not None:
                fail(f"[{name}] daemon exited early (rc={daemon.returncode}); "
                     f"see {log_path}")
            time.sleep(0.05)

        clk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        clk.connect(CLOCK_PATH)

        seq = 0
        for tick in range(tick_count):
            now_ms = tick * 100
            seq = tick_callback(tick, now_ms, master_fd, seq)
            clk.sendall(f"TICK {now_ms}\n".encode("ascii"))
            recv_done(clk)
            per_tick.append(drain_master_cmds(master_fd))
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
    return per_tick


# -------------------------------------------------------------- ramp test

RAMP_TICKS    = 50
TEMP_START_MC = 25000
TEMP_END_MC   = 75000


def run_ramp_test() -> None:
    """Ramp the synthetic sensor 25 C -> 75 C and verify the duty
    trajectory crosses each cooling-state plateau."""
    def ramp_tick(tick, now_ms, master_fd, seq):
        temp_mc = TEMP_START_MC + (TEMP_END_MC - TEMP_START_MC) \
                                   * tick // (RAMP_TICKS - 1)
        inject_telem_sample(master_fd, seq, now_ms,
                            SIG_SENSOR_TEMP_0, temp_mc)
        seq = (seq + 1) & 0xFFFF
        inject_telem_sample(master_fd, seq, now_ms, SIG_TACH_RPM_0, 0)
        seq = (seq + 1) & 0xFFFF
        return seq

    per_tick = run_session("ramp", RAMP_TICKS, ramp_tick)
    cmds = [c for tick_cmds in per_tick for c in tick_cmds]

    nacks = [c for c in cmds if c[0] == "CMD_NACK"]
    if nacks:
        fail(f"[ramp] unexpected NACKs from daemon: {nacks[:5]}")

    cmd_reqs = [c for c in cmds if c[0] == "CMD_REQUEST"]
    if not cmd_reqs:
        fail(f"[ramp] no CMD_REQUEST frames seen over {RAMP_TICKS} ticks")

    wrong_id = [c for c in cmd_reqs if c[1] != HIL_CMD_SET_PWM_DUTY]
    if wrong_id:
        fail(f"[ramp] unexpected command_id values: {wrong_id[:5]}")

    wrong_slot = [c for c in cmd_reqs if c[2] != 0]
    if wrong_slot:
        fail(f"[ramp] unexpected slot values (expected 0): {wrong_slot[:5]}")

    duties = [c[3] for c in cmd_reqs]
    print(f"  [ramp] {len(duties)} CMD_REQUEST frames; "
          f"duty range = {min(duties)}..{max(duties)}")

    if max(duties) < 220:
        fail(f"[ramp] expected duty to reach 220 (critical state) but "
             f"max observed = {max(duties)}")
    if duties[0] != 0:
        fail(f"[ramp] expected duty=0 at first tick (temp=25 C, below "
             f"warn) but observed = {duties[0]}")
    for state_pwm in EXPECTED_DUTIES_AT_TRIPS:
        if state_pwm not in duties:
            fail(f"[ramp] duty never crossed expected cooling-state "
                 f"plateau {state_pwm}; observed trajectory: {duties}")
    print("  [ramp] PASS")


# -------------------------------------------------------------- staleness test

STALE_TICKS = 70


def run_staleness_test() -> None:
    """Inject a cold (25 C) reading for the first 10 ticks, then let
    the stream stall.  The cached HIL sample must expire once it is
    older than max_staleness_ms: the zone then falls back to
    fallback_temp_mc (85 C) and the fan is driven to the critical
    plateau.  Before the codex-v8 finding-1 fix the cache never
    expired and the daemon regulated on the frozen 25 C reading
    forever -- this scenario is the regression guard."""
    def staleness_tick(tick, now_ms, master_fd, seq):
        if tick < 10:
            inject_telem_sample(master_fd, seq, now_ms,
                                SIG_SENSOR_TEMP_0, 25000)
            seq = (seq + 1) & 0xFFFF
            inject_telem_sample(master_fd, seq, now_ms, SIG_TACH_RPM_0, 0)
            seq = (seq + 1) & 0xFFFF
        return seq

    per_tick = run_session("staleness", STALE_TICKS, staleness_tick)

    def duties_in(lo, hi):
        return [c[3] for tick in range(lo, hi)
                for c in per_tick[tick] if c[0] == "CMD_REQUEST"]

    nacks = [c for tc in per_tick for c in tc if c[0] == "CMD_NACK"]
    if nacks:
        fail(f"[staleness] unexpected NACKs from daemon: {nacks[:5]}")

    # The last good frame is drained at now_ms = 900.  Ticks 0..54
    # (now_ms <= 5400) keep age <= 4500 ms, inside the 5000 ms
    # window: the sample stays valid at 25 C, below all trips ->
    # duty 0.
    fresh = duties_in(0, 55)
    if not fresh or any(d != 0 for d in fresh):
        fail(f"[staleness] expected duty 0 while the cached 25 C sample "
             f"is fresh, observed duties {sorted(set(fresh))}")

    # Ticks 66..69 (now_ms >= 6600) put age >= 5700 ms, past the
    # 5000 ms window.  The sample must expire -> zone temp falls back
    # to 85 C -> critical cooling state -> duty 220.
    stale = duties_in(66, STALE_TICKS)
    if not stale or any(d != 220 for d in stale):
        fail(f"[staleness] expected duty 220 (stale sample -> 85 C "
             f"fallback) once the HIL stream stalled, observed duties "
             f"{sorted(set(stale))}")

    print(f"  [staleness] PASS (fresh-window duty 0, "
          f"post-staleness duty {stale[-1]})")


# -------------------------------------------------------------- main

def main() -> int:
    if not DAEMON_PATH.exists():
        fail(f"daemon binary missing at {DAEMON_PATH}; run `make build` first")
    run_ramp_test()
    run_staleness_test()
    print("test_hil_serial: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
