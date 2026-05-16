#!/usr/bin/env python3
"""test/parity/capture_esp32.py

Capture one canonical replay CSV block from the ESP32-C3 REPLAY
firmware over USB-Serial-JTAG, for `make replay-parity`.

The REPLAY firmware emits the canonical CSV on a loop -- header,
data rows, `END`, a short pause, repeat -- so this script can
attach to the serial port at any time and still grab one complete
header..END block (any partial block seen first is discarded).

CRLF from the ESP-IDF console is normalized to LF: the line
ending is a transport-layer artifact, not part of the canonical
telemetry projection, so normalizing it is correct, not cheating.
"""
import argparse
import sys
import time

import serial  # pyserial (ships with the ESP-IDF Python environment)

HEADER = "ts_ms,row_type,id,value,flags_or_status,a1,a2,a3,a4"


def main() -> int:
    ap = argparse.ArgumentParser(prog="capture_esp32")
    ap.add_argument("--port", required=True,
                    help="serial port, e.g. /dev/ttyACM0")
    ap.add_argument("--out", required=True, help="output CSV path")
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="give up after N seconds (default 20)")
    args = ap.parse_args()

    deadline = time.time() + args.timeout
    block: list[str] = []
    capturing = False

    with serial.Serial(args.port, 115200, timeout=1.0) as ser:
        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("ascii", errors="replace").rstrip("\r\n")
            if not capturing:
                # Discard boot logs and any partial block until a
                # fresh canonical header line appears.
                if line == HEADER:
                    capturing = True
                    block = [line]
                continue
            block.append(line)
            if line == "END":
                with open(args.out, "w", encoding="ascii",
                          newline="") as f:
                    f.write("\n".join(block) + "\n")
                print(f"capture_esp32: wrote {len(block)} lines "
                      f"to {args.out}")
                return 0

    print(f"capture_esp32: TIMEOUT after {args.timeout}s -- no "
          f"complete header..END block on {args.port}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
