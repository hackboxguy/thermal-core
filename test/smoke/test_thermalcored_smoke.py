#!/usr/bin/env python3
"""test/smoke/test_thermalcored_smoke.py

Automates the daemon end-to-end after Stage 10 10b:

  1. Start thermalcored under --clock=scenario.
  2. Drive 5 TICKs over the AF_UNIX clock socket.
  3. Verify the daemon emits TELEM_SAMPLE frames in the canonical
     "TC" wire format (header + payload + CRC-16/CCITT-FALSE).
  4. Send a CMD_SET_SETPOINT to the control listener and verify
     the daemon replies with a matching CMD_ACK.
  5. SIGTERM the daemon; expect exit code 0.

No third-party deps; standard library only.

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
UDP_TELEM_PORT   = 9000
UDP_CONTROL_PORT = 9002

# Wire constants (mirror protocol/thermal_wire_opcodes.h).
WIRE_MAGIC      = b"TC"
WIRE_VERSION    = 1
WIRE_HEADER_LEN = 12
WIRE_CRC_LEN    = 2

OP_TELEM_SAMPLE = 0x01
OP_TELEM_EVENT  = 0x02
OP_CMD_REQUEST  = 0x10
OP_CMD_ACK      = 0x11
OP_CMD_NACK     = 0x12

CMD_SET_TRIP = 0x0003

TSIG_ZONE_TEMP_0 = 0x0100


# === CRC-16/CCITT-FALSE (poly=0x1021, init=0xFFFF, no refl, no XOR) =====

def crc16(buf: bytes) -> int:
    crc = 0xFFFF
    for b in buf:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


# === TC frame helpers ===================================================

def encode_tc(opcode: int, seq: int, ts_ms: int, payload: bytes,
              crc_enabled: bool = True) -> bytes:
    header = struct.pack("<2sBBHHI", WIRE_MAGIC, WIRE_VERSION,
                         opcode, seq, len(payload), ts_ms)
    body = header + payload
    if crc_enabled:
        body += struct.pack("<H", crc16(body))
    return body


def decode_tc(data: bytes, crc_enabled: bool = True):
    """Returns (opcode, seq, ts_ms, payload) or None if frame invalid."""
    if len(data) < WIRE_HEADER_LEN:
        return None
    if data[0:2] != WIRE_MAGIC:
        return None
    if data[2] != WIRE_VERSION:
        return None
    opcode = data[3]
    seq, payload_len, ts_ms = struct.unpack("<HHI", data[4:12])
    need = WIRE_HEADER_LEN + payload_len + (WIRE_CRC_LEN if crc_enabled else 0)
    if len(data) < need:
        return None
    payload = data[WIRE_HEADER_LEN:WIRE_HEADER_LEN + payload_len]
    if crc_enabled:
        got = struct.unpack("<H", data[WIRE_HEADER_LEN + payload_len:
                                       WIRE_HEADER_LEN + payload_len + 2])[0]
        expected = crc16(data[:WIRE_HEADER_LEN + payload_len])
        if got != expected:
            return None
    return opcode, seq, ts_ms, payload


# === Test scaffolding ===================================================

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

    # Drive 5 ticks (smoke-config period_ticks=1 -> 3 signals per tick = 15 frames).
    clk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    clk.connect(CLOCK_PATH)
    clk.sendall(b"TICK 100\nTICK 200\nTICK 300\nTICK 400\nTICK 500\n")

    # Collect telemetry frames.
    frames = []
    try:
        while len(frames) < 15:
            data, _ = udp.recvfrom(256)
            frames.append(data)
    except socket.timeout:
        pass

    # Send a CMD_SET_TRIP to the control listener.  SET_TRIP works on
    # any zone (the PID-only commands SET_PID / SET_SETPOINT would
    # NACK with INVALID_ARG against the smoke config's step_wise
    # zone).  Payload: u16 command_id, u16 zone, u16 trip_idx,
    # i32 temp_mc, i32 hyst_mc.  Bumping trip 0 from 70000 mC to
    # 72000 mC keeps the trip ordering valid (trip 1 is at 90000).
    REQUEST_SEQ = 7
    cmd_payload = struct.pack("<HHHii",
                              CMD_SET_TRIP,
                              0,        # zone_id
                              0,        # trip_idx
                              72000,    # temp_mc
                              2000)     # hyst_mc
    cmd_frame = encode_tc(OP_CMD_REQUEST, REQUEST_SEQ, 999, cmd_payload,
                          crc_enabled=True)
    cmd_sock.sendto(cmd_frame, (UDP_HOST, UDP_CONTROL_PORT))

    ack_decoded = None
    try:
        # The daemon only drains commands at each tick.  Push one more
        # TICK so the queued CMD_REQUEST gets serviced.
        clk.sendall(b"TICK 600\n")
        deadline = time.time() + 2.0
        while time.time() < deadline:
            data, _ = cmd_sock.recvfrom(256)
            dec = decode_tc(data, crc_enabled=True)
            if dec is None:
                continue
            ack_decoded = dec
            break
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
        print(f"command response: opcode=0x{op:02x} seq={seq} ts_ms={ts_ms} payload_len={len(payload)}")
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
        dec = decode_tc(raw, crc_enabled=True)
        if dec is None:
            continue
        op, _, _, payload = dec
        if op != OP_TELEM_SAMPLE or len(payload) != 8:
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
    if op != OP_CMD_ACK:
        fail(f"expected CMD_ACK (0x{OP_CMD_ACK:02x}), got opcode 0x{op:02x}")
    if len(payload) != 8:
        fail(f"unexpected ACK payload length {len(payload)}")
    request_seq, status, _detail = struct.unpack("<HHI", payload)
    if request_seq != REQUEST_SEQ:
        fail(f"CMD_ACK echoed request_seq={request_seq}, expected {REQUEST_SEQ}")
    if status != 0:
        fail(f"CMD_ACK status={status}, expected 0 (THERMAL_OK)")

    print("smoke: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
