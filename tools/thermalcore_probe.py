#!/usr/bin/env python3
"""tools/thermalcore-probe — UDP telemetry recorder + CSV logger.

Per PRD section 7.4 the probe ships three modes (--log, --live,
--plot).  v1 (Stage 12 12b) ships only `--log` (CSV) since that's
what the scenario runner needs.  --live (matplotlib) and --plot
(white-paper figure generation) land alongside the paper build
post-Stage 12.

CSV format (byte-stable so the Stage 12 12d determinism gate can
SHA-256 it):

    ts_ms,kind,id,value,a1,a2,a3,a4         # header
    0,sample,256,75000,,,,                  # TELEM_SAMPLE row
    100,event,4608,1,0,0,0                  # TELEM_EVENT row

Decimal integers only, `\\n` line endings, no trailing whitespace,
no scientific notation.  Sample rows pad with four empty trailing
fields so every row is exactly 8 columns wide.

Usage:
    thermalcore-probe --listen udp:127.0.0.1:9030 --log out.csv
                      [--duration SECONDS]

When run as a library, the scenario runner imports
ProbeRecorder directly to avoid a second Python interpreter.
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

# Make the shared wire module importable both when run as a script
# (from anywhere) and when imported as a library.
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "test" / "parity"))

import thermalcore_wire as w   # noqa: E402
from canonical import (CANONICAL_HEADER, canonical_event,   # noqa: E402
                       canonical_sample)


@dataclass
class ProbeRecord:
    ts_ms: int
    kind: str            # "sample" or "event"
    id: int              # signal_id for sample, event_code for event
    value: int           # signal value or event arg0
    a1: int = 0
    a2: int = 0
    a3: int = 0
    a4: int = 0
    flags: int = 0       # telemetry flags (sample rows only)

    def csv_row(self) -> str:
        if self.kind == "sample":
            return canonical_sample(self.ts_ms, self.id,
                                    self.value, self.flags)
        return canonical_event(self.ts_ms, self.id,
                               self.a1, self.a2, self.a3, self.a4)


class ProbeRecorder:
    """In-process telemetry recorder.

    The scenario runner constructs a ProbeRecorder around a bound
    UDP socket, calls drain() between ticks, and at the end of the
    scenario calls write_csv() + reads `records` for assertion
    evaluation."""

    def __init__(self, udp_sock: socket.socket):
        self._sock = udp_sock
        self.records: list[ProbeRecord] = []

    def drain(self) -> None:
        """Read every frame currently waiting on the socket.

        Non-blocking; safe to call between ticks.  Frames that
        fail to decode are silently dropped (corrupt CRC, wrong
        magic, etc. — defensive against future fuzzing of the
        wire stream)."""
        self._sock.setblocking(False)
        while True:
            try:
                data, _ = self._sock.recvfrom(512)
            except (BlockingIOError, socket.error):
                break
            self._consume(data)

    def collect_until(self, deadline_s: float) -> None:
        """Block-with-timeout until `deadline_s` (monotonic
        seconds).  Used by the standalone probe CLI for
        --duration mode."""
        while True:
            remaining = deadline_s - time.time()
            if remaining <= 0:
                break
            self._sock.settimeout(remaining)
            try:
                data, _ = self._sock.recvfrom(512)
            except socket.timeout:
                break
            self._consume(data)

    def _consume(self, data: bytes) -> None:
        dec = w.decode_tc(data, crc=True)
        if dec is None:
            return
        opcode, _seq, ts_ms, payload = dec
        if opcode == w.OP_TELEM_SAMPLE and len(payload) == 8:
            sig, flags, val = struct.unpack("<HHi", payload)
            self.records.append(
                ProbeRecord(ts_ms=ts_ms, kind="sample",
                            id=sig, value=val, flags=flags))
        elif opcode == w.OP_TELEM_EVENT and len(payload) == 18:
            # event payload: u16 code, u32 a1, u32 a2, u32 a3, u32 a4
            code, a1, a2, a3, a4 = struct.unpack("<HIIII", payload)
            self.records.append(
                ProbeRecord(ts_ms=ts_ms, kind="event",
                            id=code, value=a1, a1=a1, a2=a2,
                            a3=a3, a4=a4))
        # Other opcodes (CMD_*) are ignored; the probe only logs
        # device->host frames.

    def write_csv(self, path) -> None:
        with Path(path).open("w", encoding="ascii", newline="") as f:
            f.write(CANONICAL_HEADER)
            for rec in self.records:
                f.write(rec.csv_row())


# === Standalone CLI ====================================================

def _parse_listen_uri(uri: str) -> tuple[str, int]:
    if not uri.startswith("udp:"):
        raise SystemExit(f"thermalcore-probe: --listen must start with "
                         f"'udp:'; got {uri!r}")
    rest = uri[4:]
    host, _, port = rest.rpartition(":")
    if not host or not port:
        raise SystemExit(f"thermalcore-probe: invalid --listen {uri!r}; "
                         f"expected 'udp:HOST:PORT'")
    return host, int(port)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="thermalcore-probe")
    parser.add_argument("--listen", required=True,
                        help="udp:HOST:PORT (e.g. udp:127.0.0.1:9001)")
    parser.add_argument("--log", required=True,
                        help="output CSV path")
    parser.add_argument("--duration", type=float, default=None,
                        help="stop after N seconds (default: until "
                             "SIGTERM)")
    args = parser.parse_args(argv)

    host, port = _parse_listen_uri(args.listen)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))

    recorder = ProbeRecorder(sock)
    try:
        if args.duration is not None:
            recorder.collect_until(time.time() + args.duration)
        else:
            # No duration: block in 1 s windows until SIGTERM.
            while True:
                recorder.collect_until(time.time() + 1.0)
    except KeyboardInterrupt:
        pass
    finally:
        recorder.write_csv(args.log)
        sock.close()

    print(f"thermalcore-probe: wrote {len(recorder.records)} records "
          f"to {args.log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
