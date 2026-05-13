#!/usr/bin/env python3
"""test/integration/test_canbus_obd2.py

Stage 11 11c — hardware-loop integration test for the SocketCAN +
OBD-II path.  Spawns `car-can-emulator` on vcan0 + thermalcored
with `canbus-config.json`; drives three vehicle-speed setpoints
via the emulator's TCP control port; asserts the daemon's
TSIG_CONTEXT_VALUE_0 telemetry frames converge to each setpoint
within the configured IIR filter time constant.

Skip-cleanly when vcan0 is not available (no CAP_NET_ADMIN, vcan
kernel module not loaded, etc.).  Per impl-plan §5 Stage 11, the
canonical CI must run this for real on ubuntu-latest; a skip on
the canonical repo blocks the Stage 11 exit gate.
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

# Setpoints + hold windows (PRD-derived; see plan §3).
SETPOINTS_KMH = [100, 50, 200]
HOLD_SECONDS  = 8.0
TOLERANCE_KMH = 15.0


def fail(msg: str) -> None:
    sys.stderr.write(f"FAIL: {msg}\n")
    sys.exit(1)


def vcan_available() -> bool:
    """vcan0 exists as a network interface.  Cheap probe -- if the
    interface is up the test runs; if not we skip cleanly."""
    return Path("/sys/class/net/vcan0").exists()


def set_emulator_speed(kmh: int) -> None:
    """Open a short-lived TCP connection to the emulator's control
    port and send `speed <kmh>`.  The emulator's TCP server is
    line-oriented per its README."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    try:
        s.connect((UDP_HOST, EMU_TCP_PORT))
        s.sendall(f"speed {kmh}\n".encode())
        # Drain any acknowledgement so the emulator's send doesn't
        # block on a closed peer.
        try:
            s.recv(256)
        except socket.timeout:
            pass
    finally:
        s.close()


def collect_speed_samples(sock: socket.socket, deadline: float):
    """Drain telemetry frames until `deadline`; return the list of
    decoded speed values from TSIG_CONTEXT_VALUE_0 samples."""
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
    # Sensor at 75000 mc -- below the first trip (70000+hyst); zone
    # mostly idle.  Test focuses on the context-signal path, not
    # the thermal loop.
    (INTEG_DIR / "sensors" / "soc").write_text("75000\n")
    (INTEG_DIR / "pwm1").write_text("0\n")
    (INTEG_DIR / "fan1_input").write_text("1500\n")


def main() -> int:
    if not vcan_available():
        print("integration-can: SKIP (no vcan0; install with "
              "`sudo modprobe vcan && sudo ip link add dev vcan0 "
              "type vcan && sudo ip link set up vcan0`)")
        return 0

    if not DAEMON.exists():
        fail(f"daemon binary missing at {DAEMON} (run `make build` first)")
    if not EMULATOR.exists():
        fail(f"emulator binary missing at {EMULATOR} "
             f"(run `cmake -H tools/car-can-emulator -B build/car-can-emulator "
             f"&& cmake --build build/car-can-emulator`)")

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

    emu_log = Path("/tmp/car-can-emulator.log").open("w")
    daemon_log = Path("/tmp/thermalcored-canbus-integ.log").open("w")
    emu = None
    daemon = None
    try:
        # Start the emulator first so the daemon has somewhere to
        # send its OBD-II requests when it comes up.
        emu = subprocess.Popen(
            [str(EMULATOR), "--node=vcan0", "--port=" + str(EMU_TCP_PORT)],
            stdout=emu_log, stderr=emu_log)
        time.sleep(0.5)
        if emu.poll() is not None:
            fail(f"emulator exited early (rc={emu.returncode}); "
                 f"see /tmp/car-can-emulator.log")

        daemon = subprocess.Popen(
            [str(DAEMON), f"--config={CONFIG}", "--clock=wall"],
            stdout=daemon_log, stderr=daemon_log)
        time.sleep(0.5)
        if daemon.poll() is not None:
            fail(f"daemon exited early (rc={daemon.returncode}); "
                 f"see /tmp/thermalcored-canbus-integ.log")

        for setpoint in SETPOINTS_KMH:
            set_emulator_speed(setpoint)
            # Hold long enough for the IIR to converge (~3 time
            # constants at iir_alpha_q16=2048 + 1 Hz CAN refresh +
            # 10 Hz daemon ticks).  Collect every telemetry frame
            # in the window.
            deadline = time.time() + HOLD_SECONDS
            speeds = collect_speed_samples(udp, deadline)
            if not speeds:
                fail(f"setpoint {setpoint}: no TSIG_CONTEXT_VALUE_0 "
                     f"telemetry frames captured in {HOLD_SECONDS}s")
            # Use the last quarter of the window so the IIR has had
            # time to converge; require at least one sample within
            # tolerance.
            tail = speeds[-max(1, len(speeds) // 4):]
            converged = [v for v in tail if abs(v - setpoint) <= TOLERANCE_KMH]
            if not converged:
                fail(f"setpoint {setpoint}: no telemetry samples within "
                     f"+/-{int(TOLERANCE_KMH)} km/h in the convergence tail; "
                     f"tail = {tail[:10]}{'...' if len(tail) > 10 else ''}")
            print(f"  [setpoint {setpoint:>3} km/h] {len(speeds)} samples, "
                  f"tail-converged value(s) within tolerance: "
                  f"{converged[:5]}{'...' if len(converged) > 5 else ''}")

    finally:
        if daemon is not None:
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                daemon.kill()
        if emu is not None:
            emu.send_signal(signal.SIGTERM)
            try:
                emu.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                emu.kill()
        emu_log.close()
        daemon_log.close()
        udp.close()

    print("integration-can: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
