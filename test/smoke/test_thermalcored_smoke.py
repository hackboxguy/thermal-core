#!/usr/bin/env python3
"""test/smoke/test_thermalcored_smoke.py

Automates the manual sanity from Stage 9 9c: start thermalcored
under --clock=scenario, drive a few TICK messages, verify the daemon
emits TELEM_SAMPLE frames on UDP port 9000 and exits cleanly with
code 0 on SIGTERM.

This is the impl-plan section 2.6 smoke test.  No third-party deps;
standard-library only.

Run via:  make smoke
Or directly:
    python3 test/smoke/test_thermalcored_smoke.py
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

ROOT = Path(__file__).resolve().parents[2]
DAEMON = ROOT / "build" / "platform-linux" / "thermalcored"
CONFIG = ROOT / "test" / "smoke" / "smoke-config.json"

SMOKE_DIR  = Path("/tmp/thermal-core-smoke")
CLOCK_PATH = "/tmp/thermalcored-smoke-clock.sock"

UDP_HOST = "127.0.0.1"
UDP_PORT = 9000

TELEM_SAMPLE_TYPE = 0x01
TELEM_SAMPLE_LEN  = 11
TSIG_ZONE_TEMP_0  = 0x0100


def fail(msg: str) -> None:
    sys.stderr.write(f"FAIL: {msg}\n")
    sys.exit(1)


def setup_tmpfs() -> None:
    """Plant the BSP files the daemon will read / write."""
    (SMOKE_DIR / "sensors").mkdir(parents=True, exist_ok=True)
    (SMOKE_DIR / "contexts").mkdir(parents=True, exist_ok=True)
    (SMOKE_DIR / "sensors" / "soc").write_text("75000\n")
    (SMOKE_DIR / "contexts" / "vehicle_speed").write_text("120\n")
    # PWM and tach files (the BSP will overwrite pwm during the loop).
    (SMOKE_DIR / "pwm1").write_text("0\n")
    (SMOKE_DIR / "fan1_input").write_text("1500\n")
    try:
        os.unlink(CLOCK_PATH)
    except FileNotFoundError:
        pass


def main() -> int:
    if not DAEMON.exists():
        fail(f"daemon binary missing at {DAEMON} (run `make build` first)")

    setup_tmpfs()

    # Open UDP listener BEFORE starting the daemon so no frames are lost.
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind((UDP_HOST, UDP_PORT))
    udp.settimeout(2.0)

    log_path = Path("/tmp/thermalcored-smoke.log")
    log = log_path.open("w")
    daemon = subprocess.Popen(
        [str(DAEMON),
         f"--config={CONFIG}",
         "--clock=scenario",
         f"--scenario-clock-uri=unix:{CLOCK_PATH}"],
        stdout=log, stderr=log)

    # Wait for the daemon to create the AF_UNIX socket.
    deadline = time.time() + 2.0
    while not os.path.exists(CLOCK_PATH):
        if time.time() > deadline:
            daemon.kill()
            udp.close()
            fail(f"daemon never created clock socket {CLOCK_PATH}")
        time.sleep(0.05)

    # Drive five ticks (telemetry.period_ticks=1 in smoke-config.json
    # so every tick emits a batch of three signals = 15 frames total).
    clk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    clk.connect(CLOCK_PATH)
    clk.sendall(b"TICK 100\nTICK 200\nTICK 300\nTICK 400\nTICK 500\n")

    # Collect frames (up to 15, or until we time out).
    frames = []
    try:
        while len(frames) < 15:
            data, _ = udp.recvfrom(64)
            frames.append(data)
    except socket.timeout:
        pass

    # Stop the daemon.
    clk.close()
    daemon.send_signal(signal.SIGTERM)
    try:
        rc = daemon.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        daemon.kill()
        rc = -1
    log.close()
    udp.close()

    # Print diagnostics regardless of outcome.
    print(f"daemon exit code: {rc}")
    print(f"telemetry frames received: {len(frames)}")

    # === Assertions =====================================================
    if rc != 0:
        fail(f"daemon did not exit cleanly: rc={rc}\n"
             f"see {log_path} for details")
    if len(frames) < 3:
        fail(f"expected ≥ 3 telemetry frames, got {len(frames)}")

    # Decode at least one sample frame and confirm the zone_temp signal
    # came through with the planted value.
    saw_zone_temp = False
    for f in frames:
        if len(f) == TELEM_SAMPLE_LEN and f[0] == TELEM_SAMPLE_TYPE:
            ts_ms, sig, val = struct.unpack("<IHi", f[1:])
            if sig == TSIG_ZONE_TEMP_0:
                saw_zone_temp = True
                if val != 75000:
                    fail(f"zone_temp value mismatch: got {val}, expected 75000")
                break

    if not saw_zone_temp:
        fail("no TELEM_SAMPLE frame for zone_temp_0 (signal 0x0100) seen")

    print("smoke: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
