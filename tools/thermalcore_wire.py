"""tools/thermalcore_wire.py — Python helpers for the TC binary wire.

Python equivalent of `protocol/thermal_wire.c` and
`protocol/thermal_wire_opcodes.h`.  Stdlib only.  Used by:
- `tools/thermalcore-tune` (this commit)
- `test/smoke/test_thermalcored_smoke.py` (refactored in this commit)
- `tools/thermalcore-probe` (future Stage 11+)

This module is intentionally a thin port — the C side is the
authoritative implementation; the Python side mirrors it.  When
something changes in `protocol/`, update here too.

Frame layout (PRD section 7.2 lines 902-911 + 920-921):

    magic[2] = "TC"
    version  = 1                     (u8)
    opcode                           (u8)
    seq                              (u16 little-endian)
    payload_len                      (u16 little-endian)
    ts_ms                            (u32 little-endian)
    payload[payload_len]
    crc16                            (u16 little-endian; always present)

The trailing `u16 crc16` field is **always** on the wire.  A
`0x0000` value means "CRC disabled" only on transports that
explicitly allow it (loopback UDP).  The `crc` argument on
`encode_tc` / `decode_tc` controls *validation*, not *frame shape*.

CRC: CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection,
no final XOR), covering bytes from magic[0] through the last
payload byte.
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

# === Frame constants ====================================================

WIRE_MAGIC      = b"TC"
WIRE_VERSION    = 1
WIRE_HEADER_LEN = 12
WIRE_CRC_LEN    = 2

# Receive caps (PRD section 7.2 line 923).
WIRE_MAX_LINUX  = 1024
WIRE_MAX_MCU    = 256

# Opcodes (PRD section 7.2 lines 925-933).
OP_TELEM_SAMPLE = 0x01
OP_TELEM_EVENT  = 0x02
OP_CMD_REQUEST  = 0x10
OP_CMD_ACK      = 0x11
OP_CMD_NACK     = 0x12

# Command IDs — must match core/thermal_commands.h.
CMD_SET_PID         = 0x0001
CMD_SET_SETPOINT    = 0x0002
CMD_SET_TRIP        = 0x0003
CMD_SET_CURVE_POINT = 0x0004
CMD_CLEAR_FAULT     = 0x0005


# === CRC-16/CCITT-FALSE =================================================

def crc16_ccitt_false(buf: bytes) -> int:
    """CRC-16/CCITT-FALSE.

    poly=0x1021, init=0xFFFF, refin=false, refout=false, xorout=0.
    Canonical "123456789" -> 0x29B1.
    """
    crc = 0xFFFF
    for b in buf:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# === Outer-frame encode / decode =======================================

def encode_tc(opcode: int, seq: int, ts_ms: int,
              payload: bytes, crc: bool = True) -> bytes:
    """Build a complete TC frame: header + payload + trailing CRC field.

    The trailing 2 CRC bytes are always emitted (PRD section 7.2 lines
    910 + 920-921).  When `crc=False`, the field is written as 0x0000
    instead of the computed CRC."""
    header = struct.pack("<2sBBHHI", WIRE_MAGIC, WIRE_VERSION,
                         opcode & 0xFF, seq & 0xFFFF,
                         len(payload), ts_ms & 0xFFFFFFFF)
    body = header + payload
    crc_val = crc16_ccitt_false(body) if crc else 0
    return body + struct.pack("<H", crc_val)


def decode_tc(buf: bytes, crc: bool = True):
    """Parse a TC frame.

    Returns (opcode, seq, ts_ms, payload) on success, or None if
    the frame is malformed (bad magic / version / CRC / truncated).

    The trailing 2 CRC bytes are always required to be present
    on the wire; `crc=False` only suppresses *validation* of those
    bytes (their value is informational on transports that allow
    CRC-disabled frames).
    """
    if len(buf) < WIRE_HEADER_LEN:
        return None
    if buf[0:2] != WIRE_MAGIC:
        return None
    if buf[2] != WIRE_VERSION:
        return None
    opcode = buf[3]
    seq, payload_len, ts_ms = struct.unpack("<HHI", buf[4:12])
    need = WIRE_HEADER_LEN + payload_len + WIRE_CRC_LEN
    if len(buf) < need:
        return None
    payload = buf[WIRE_HEADER_LEN:WIRE_HEADER_LEN + payload_len]
    if crc:
        got = struct.unpack(
            "<H",
            buf[WIRE_HEADER_LEN + payload_len:
                WIRE_HEADER_LEN + payload_len + 2])[0]
        expected = crc16_ccitt_false(buf[:WIRE_HEADER_LEN + payload_len])
        if got != expected:
            return None
    return opcode, seq, ts_ms, payload


# === CMD_REQUEST payload encoders ======================================
#
# Each helper returns the bytes inside the TC payload (starting with
# the u16 command_id).  Callers wrap with encode_tc(OP_CMD_REQUEST,
# ...).

def encode_set_pid(zone_id: int, kp_q16: int,
                   ki_q16: int, kd_q16: int) -> bytes:
    return struct.pack("<HHiii",
                       CMD_SET_PID, zone_id,
                       kp_q16, ki_q16, kd_q16)


def encode_set_setpoint(zone_id: int, setpoint_mc: int) -> bytes:
    return struct.pack("<HHi", CMD_SET_SETPOINT, zone_id, setpoint_mc)


def encode_set_trip(zone_id: int, trip_idx: int,
                    temp_mc: int, hyst_mc: int) -> bytes:
    return struct.pack("<HHHii",
                       CMD_SET_TRIP, zone_id, trip_idx,
                       temp_mc, hyst_mc)


def encode_set_curve_point(modifier_id: int, point_idx: int,
                           x: int, value0: int, value1: int) -> bytes:
    return struct.pack("<HHHiii",
                       CMD_SET_CURVE_POINT, modifier_id, point_idx,
                       x, value0, value1)


def encode_clear_fault(fault_type: int, target_id: int) -> bytes:
    return struct.pack("<HHH", CMD_CLEAR_FAULT, fault_type, target_id)


# === CMD_ACK / CMD_NACK payload decode ================================

def decode_ack_or_nack(payload: bytes):
    """Returns (request_seq, status, detail_code), or None if length
    doesn't match the 8-byte CMD_ACK/CMD_NACK payload."""
    if len(payload) != 8:
        return None
    return struct.unpack("<HHI", payload)


# === Config-aware name resolution =======================================
#
# Used by tools/thermalcore-tune to map human zone / modifier /
# actuator / sensor / context names to the slot indexes consumed by
# the wire codec.

class Config:
    """Minimal view over a thermalcored JSON config: just the
    (name → slot index) maps the CLI needs.  Slot indexes are the
    position in the config's array, matching how config_jsmn.c
    populates thermal_config_t."""
    def __init__(self, raw: dict):
        self.zones      = [z["name"] for z in raw.get("zones", [])]
        self.modifiers  = [m["name"] for m in raw.get("policy_modifiers", [])]
        self.actuators  = [a["name"] for a in raw.get("actuators", [])]
        self.sensors    = [s["name"] for s in raw.get("sensors", [])]
        self.contexts   = [c["name"] for c in raw.get("context_signals", [])]


def load_config(path) -> Config:
    """Read and parse a thermalcored JSON config into a Config view."""
    with Path(path).open("r", encoding="utf-8") as f:
        return Config(json.load(f))


def resolve_name(name_or_int: str, table: list, kind: str) -> int:
    """Return an integer slot index for the given string.

    Integer strings ("0", "12") pass through unchanged; names look up
    `table` by exact match.  Raises SystemExit on a name miss.
    """
    try:
        return int(name_or_int)
    except (ValueError, TypeError):
        pass
    if name_or_int in table:
        return table.index(name_or_int)
    raise SystemExit(
        f"thermalcore-tune: unknown {kind} '{name_or_int}'; "
        f"known: {table}")
