"""test/parity/canonical.py -- Python mirror of test/parity/canonical.c.

The canonical telemetry CSV projection used by Stage 15.  This
module must stay byte-for-byte identical to canonical.c: the host
UDP probe (Stage 12 determinism) formats through here, the C
replay rigs (Stage 15 host-vs-target parity) format through
canonical.c, and both must hash the same projection.

Row layout, one row per core callback:

    ts_ms,row_type,id,value,flags_or_status,a1,a2,a3,a4

  - `S` (sample) rows -- id = signal_id, value = value,
    flags_or_status = flags, a1..a4 = 0.
  - `E` (event) rows  -- id = event_code, value = 0,
    flags_or_status = 0, a1..a4 = the four event args.

Decimal integers only, no whitespace, '\\n' line endings.
"""

CANONICAL_HEADER = "ts_ms,row_type,id,value,flags_or_status,a1,a2,a3,a4\n"


def canonical_sample(ts_ms, signal_id, value, flags=0):
    """Format one telemetry sample as a canonical `S` row."""
    return f"{ts_ms},S,{signal_id},{value},{flags},0,0,0,0\n"


def canonical_event(ts_ms, code, a1, a2, a3, a4):
    """Format one event as a canonical `E` row."""
    return f"{ts_ms},E,{code},0,0,{a1},{a2},{a3},{a4}\n"
