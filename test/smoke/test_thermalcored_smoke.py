#!/usr/bin/env python3
"""test/smoke/test_thermalcored_smoke.py

Automates the daemon end-to-end:

  1. Start thermalcored under --clock=scenario.
  2. Drive 5 TICKs over the AF_UNIX clock socket.
  3. Verify the daemon emits TELEM_SAMPLE frames in the canonical
     "TC" wire format (header + payload + CRC-16/CCITT-FALSE).
  4. Send a CMD_SET_TRIP to the control listener and verify the
     daemon replies with a matching CMD_ACK.
  5. SIGTERM the daemon; expect exit code 0.

Wire helpers live in tools/thermalcore_wire.py (Stage 10 10c).
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

# Shared wire codec.
sys.path.insert(0, str(ROOT / "tools"))
import thermalcore_wire as w   # noqa: E402

SMOKE_DIR  = Path("/tmp/thermal-core-smoke")
CLOCK_PATH = "/tmp/thermalcored-smoke-clock.sock"

UDP_HOST = "127.0.0.1"
UDP_TELEM_PORT   = 9000
UDP_CONTROL_PORT = 9002

TSIG_ZONE_TEMP_0          = 0x0100
TEVENT_COMMAND_APPLIED    = 0x1200


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


def main() -> int:
    if not DAEMON.exists():
        fail(f"daemon binary missing at {DAEMON} (run `make build` first)")

    setup_tmpfs()

    # UDP listener for telemetry frames.
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind((UDP_HOST, UDP_TELEM_PORT))
    udp.settimeout(2.0)

    # UDP socket for sending commands AND receiving ACK/NACK.
    cmd_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    cmd_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    cmd_sock.bind((UDP_HOST, 0))   # ephemeral source port
    cmd_sock.settimeout(2.0)

    log_path = Path("/tmp/thermalcored-smoke.log")
    log = log_path.open("w")
    daemon = subprocess.Popen(
        [str(DAEMON),
         f"--config={CONFIG}",
         "--clock=scenario",
         f"--scenario-clock-uri=unix:{CLOCK_PATH}"],
        stdout=log, stderr=log)

    deadline = time.time() + 2.0
    while not os.path.exists(CLOCK_PATH):
        if time.time() > deadline:
            daemon.kill()
            udp.close()
            cmd_sock.close()
            fail(f"daemon never created clock socket {CLOCK_PATH}")
        time.sleep(0.05)

    # Send the CMD_REQUEST *before* the first TICK so the daemon
    # drains it on TICK 100 and the resulting
    # TEVENT_COMMAND_APPLIED carries ts_ms = 100 (timestamp-
    # propagation assertion below).  SET_TRIP works on any
    # governor (the PID-only commands would NACK against the
    # smoke config's step_wise zone).
    REQUEST_SEQ = 7
    EXPECTED_CMD_TS = 100
    payload = w.encode_set_trip(zone_id=0, trip_idx=0,
                                temp_mc=72000, hyst_mc=2000)
    cmd_frame = w.encode_tc(w.OP_CMD_REQUEST, REQUEST_SEQ, ts_ms=999,
                            payload=payload, crc=True)
    cmd_sock.sendto(cmd_frame, (UDP_HOST, UDP_CONTROL_PORT))
    # Tiny pause so the kernel UDP buffer holds the datagram by
    # the time the daemon's drain_commands recvfrom runs.
    time.sleep(0.05)

    # Drive 5 ticks (period_ticks=1 -> 3 signals per tick + 1
    # event frame on TICK 100 when the CMD is applied = 16 frames).
    clk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    clk.connect(CLOCK_PATH)
    clk.sendall(b"TICK 100\nTICK 200\nTICK 300\nTICK 400\nTICK 500\n")

    # Collect telemetry frames; stay up to 16 (15 SAMPLEs + 1 EVENT).
    frames = []
    try:
        while len(frames) < 16:
            data, _ = udp.recvfrom(256)
            frames.append(data)
    except socket.timeout:
        pass

    # Read the ACK that the daemon sent back on the control port
    # after applying the queued CMD on TICK 100.
    ack_decoded = None
    try:
        cmd_sock.settimeout(1.0)
        data, _ = cmd_sock.recvfrom(256)
        ack_decoded = w.decode_tc(data, crc=True)
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
    cmd_sock.close()

    # === Diagnostics =====================================================
    print(f"daemon exit code: {rc}")
    print(f"telemetry frames received: {len(frames)}")
    if ack_decoded is not None:
        op, seq, ts_ms, payload = ack_decoded
        print(f"command response: opcode=0x{op:02x} seq={seq} "
              f"ts_ms={ts_ms} payload_len={len(payload)}")
    else:
        print("command response: (none)")

    # === Assertions ======================================================
    if rc != 0:
        fail(f"daemon did not exit cleanly: rc={rc}\nsee {log_path} for details")
    if len(frames) < 3:
        fail(f"expected >= 3 telemetry frames, got {len(frames)}")

    # Decode every frame; find at least one zone_temp_0 = 75000.
    saw_zone_temp = False
    for raw in frames:
        dec = w.decode_tc(raw, crc=True)
        if dec is None:
            continue
        op, _, _, payload = dec
        if op != w.OP_TELEM_SAMPLE or len(payload) != 8:
            continue
        sig, _flags, val = struct.unpack("<HHi", payload)
        if sig == TSIG_ZONE_TEMP_0 and val == 75000:
            saw_zone_temp = True
            break
    if not saw_zone_temp:
        fail("no TELEM_SAMPLE for zone_temp_0 with value 75000")

    if ack_decoded is None:
        fail("no CMD_ACK / CMD_NACK received from control listener")
    op, _, _, payload = ack_decoded
    if op != w.OP_CMD_ACK:
        fail(f"expected CMD_ACK (0x{w.OP_CMD_ACK:02x}), got opcode 0x{op:02x}")
    parsed = w.decode_ack_or_nack(payload)
    if parsed is None:
        fail(f"unexpected ACK payload length {len(payload)}")
    request_seq, status, _detail = parsed
    if request_seq != REQUEST_SEQ:
        fail(f"CMD_ACK echoed request_seq={request_seq}, expected {REQUEST_SEQ}")
    if status != 0:
        fail(f"CMD_ACK status={status}, expected 0 (THERMAL_OK)")

    # Timestamp-propagation assertion (Stage 10 10d): the daemon
    # services the queued CMD_REQUEST on TICK 100 and the resulting
    # TEVENT_COMMAND_APPLIED's ts_ms must equal that tick's
    # now_ms (impl-plan section 5 Stage 10 + PRD section 4.4).
    saw_event = False
    for raw in frames:
        dec = w.decode_tc(raw, crc=True)
        if dec is None:
            continue
        opc, _seq, ts_ms, ev_payload = dec
        if opc != w.OP_TELEM_EVENT or len(ev_payload) != 18:
            continue
        code = struct.unpack("<H", ev_payload[0:2])[0]
        if code != TEVENT_COMMAND_APPLIED:
            continue
        saw_event = True
        if ts_ms != EXPECTED_CMD_TS:
            fail(f"TEVENT_COMMAND_APPLIED ts_ms={ts_ms}, "
                 f"expected {EXPECTED_CMD_TS} (timestamp propagation)")
        break
    if not saw_event:
        fail(f"no TEVENT_COMMAND_APPLIED (code 0x{TEVENT_COMMAND_APPLIED:04x}) "
             f"in the captured telemetry stream")

    print("smoke: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
