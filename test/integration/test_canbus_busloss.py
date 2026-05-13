#!/usr/bin/env python3
"""test/integration/test_canbus_busloss.py

Stage 11 11d — CAN bus-loss module golden.  Closes the
impl-plan section 5 Stage 11 exit gate.

Drives a real car-can-emulator + thermalcored, lets the daemon
converge on a non-zero vehicle_speed, then SIGTERMs the emulator
mid-run and asserts:

  1. The daemon survives the bus loss (no crash, no early exit).
  2. After `timeout_ms` (3000 ms in canbus-config.json) the
     daemon's TSIG_CONTEXT_VALUE_0 telemetry collapses to 0,
     which is the observable end of the cascade:

         BSP timeout -> sample.valid=0
           -> thermal_filter_step sets filter.valid=0
             -> step5_modifier_eval reads filter.valid=0
               -> fail_safe = ASSUME_STATIONARY
                 -> effective_value = 0

Skip-cleanly when vcan0 is unavailable; canonical CI on
ubuntu-latest provisions vcan0 and runs for real.  Reuses
canbus-config.json from 11c — same config exhibits both happy-
path convergence (test_canbus_obd2.py) and bus-loss fail-safe
(this test).
"""
from __future__ import annotations

import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT     = Path(__file__).resolve().parents[2]
DAEMON   = ROOT / "build" / "platform-linux" / "thermalcored"
EMULATOR = ROOT / "build" / "car-can-emulator" / "car-can-emulator"
CONFIG   = ROOT / "test" / "integration" / "canbus-config.json"

# Shared wire codec.
sys.path.insert(0, str(ROOT / "tools"))
import thermalcore_wire as w   # noqa: E402

INTEG_DIR = Path("/tmp/thermal-core-canbus-integ")

UDP_HOST          = "127.0.0.1"
UDP_TELEM_PORT    = 9020
UDP_CONTROL_PORT  = 9022
EMU_TCP_PORT      = 8080

# Context value signal (PRD-locked TSIG_CONTEXT_VALUE_0 = 0x0400).
TSIG_CONTEXT_VALUE_0 = 0x0400

# Test timings (s).  See plan section 1 (Test flow).
DRIVEN_SPEED_KMH = 100
CONVERGE_SECONDS = 8.0       # IIR convergence window pre-loss
TIMEOUT_GRACE_S  = 5.0       # timeout_ms (3 s) + 2 s grace
POSTLOSS_SECONDS = 3.0       # post-grace observation window


def fail(msg: str) -> None:
    sys.stderr.write(f"FAIL: {msg}\n")
    sys.exit(1)


def vcan_available() -> bool:
    return Path("/sys/class/net/vcan0").exists()


def set_emulator_speed(kmh: int) -> None:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    try:
        s.connect((UDP_HOST, EMU_TCP_PORT))
        s.sendall(f"speed {kmh}\n".encode())
        try:
            s.recv(256)
        except socket.timeout:
            pass
    finally:
        s.close()


def collect_speed_samples(sock: socket.socket, deadline: float):
    """Drain telemetry frames until `deadline`; return decoded
    TSIG_CONTEXT_VALUE_0 sample values (int km/h after the IIR /
    fail_safe pipeline)."""
    speeds = []
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            break
        sock.settimeout(remaining)
        try:
            data, _ = sock.recvfrom(256)
        except socket.timeout:
            break
        dec = w.decode_tc(data, crc=True)
        if dec is None:
            continue
        op, _seq, _ts, payload = dec
        if op != w.OP_TELEM_SAMPLE or len(payload) != 8:
            continue
        sig, _flags, val = struct.unpack("<HHi", payload)
        if sig == TSIG_CONTEXT_VALUE_0:
            speeds.append(val)
    return speeds


def setup_tmpfs() -> None:
    (INTEG_DIR / "sensors").mkdir(parents=True, exist_ok=True)
    (INTEG_DIR / "contexts").mkdir(parents=True, exist_ok=True)
    (INTEG_DIR / "sensors" / "soc").write_text("75000\n")
    (INTEG_DIR / "pwm1").write_text("0\n")
    (INTEG_DIR / "fan1_input").write_text("1500\n")


def main() -> int:
    if not vcan_available():
        print("integration-can-busloss: SKIP (no vcan0; install with "
              "`sudo modprobe vcan && sudo ip link add dev vcan0 "
              "type vcan && sudo ip link set up vcan0`)")
        return 0

    if not DAEMON.exists():
        fail(f"daemon binary missing at {DAEMON} (run `make build` first)")
    if not EMULATOR.exists():
        fail(f"emulator binary missing at {EMULATOR}")

    setup_tmpfs()

    # Pre-flight: control port must be free.
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

    emu_log = Path("/tmp/car-can-emulator-busloss.log").open("w")
    daemon_log = Path("/tmp/thermalcored-canbus-busloss.log").open("w")
    emu = None
    daemon = None
    try:
        emu = subprocess.Popen(
            [str(EMULATOR), "--node=vcan0", "--port=" + str(EMU_TCP_PORT)],
            stdout=emu_log, stderr=emu_log)
        time.sleep(0.5)
        if emu.poll() is not None:
            fail(f"emulator exited early (rc={emu.returncode}); "
                 f"see /tmp/car-can-emulator-busloss.log")

        daemon = subprocess.Popen(
            [str(DAEMON), f"--config={CONFIG}", "--clock=wall"],
            stdout=daemon_log, stderr=daemon_log)
        time.sleep(0.5)
        if daemon.poll() is not None:
            fail(f"daemon exited early (rc={daemon.returncode}); "
                 f"see /tmp/thermalcored-canbus-busloss.log")

        # === Phase 1: pre-loss convergence ============================
        # Drive 100 km/h and hold; require at least one telemetry
        # sample > 50 km/h (i.e., daemon was actually receiving
        # speed via CAN before we cut the bus).
        set_emulator_speed(DRIVEN_SPEED_KMH)
        pre_deadline = time.time() + CONVERGE_SECONDS
        pre_speeds = collect_speed_samples(udp, pre_deadline)
        if not any(v > 50 for v in pre_speeds):
            fail(f"daemon never saw speed > 50 km/h before bus loss; "
                 f"pre-loss samples: {pre_speeds[:10]}")
        print(f"  [pre-loss] {len(pre_speeds)} samples, "
              f"max = {max(pre_speeds) if pre_speeds else None} km/h")

        # === Phase 2: kill the bus ===================================
        emu.send_signal(signal.SIGTERM)
        try:
            emu.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            emu.kill()
        print(f"  [bus loss] emulator stopped at t = {time.time():.1f}")

        # Wait through the timeout_ms (3 s) + grace.  The daemon's
        # BSP cache expires, then the filter flips invalid, then
        # the modifier applies fail_safe.  Telemetry during this
        # transient window may still show non-zero values; we
        # don't assert on it.
        grace_deadline = time.time() + TIMEOUT_GRACE_S
        _ = collect_speed_samples(udp, grace_deadline)

        # === Phase 3: post-loss assertion =============================
        # By now the cascade has fired.  ALL TSIG_CONTEXT_VALUE_0
        # frames in this window must be 0 (assume_stationary).
        post_deadline = time.time() + POSTLOSS_SECONDS
        post_speeds = collect_speed_samples(udp, post_deadline)
        if not post_speeds:
            fail("no telemetry frames in the post-loss window "
                 "(daemon may have died -- check log)")
        non_zero = [v for v in post_speeds if v != 0]
        if non_zero:
            fail(f"acoustic_mask did not fall back to assume_stationary "
                 f"after timeout_ms; post-loss samples contained "
                 f"non-zero values: {non_zero[:10]}")
        print(f"  [post-loss] {len(post_speeds)} samples, "
              f"all == 0 (assume_stationary)")

        # Daemon must still be running.
        if daemon.poll() is not None:
            fail(f"daemon exited during the bus-loss test "
                 f"(rc={daemon.returncode}); see "
                 f"/tmp/thermalcored-canbus-busloss.log")

    finally:
        if daemon is not None:
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                daemon.kill()
        if emu is not None and emu.poll() is None:
            emu.send_signal(signal.SIGTERM)
            try:
                emu.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                emu.kill()
        emu_log.close()
        daemon_log.close()
        udp.close()

    print("integration-can-busloss: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
