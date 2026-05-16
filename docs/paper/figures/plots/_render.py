"""docs/paper/figures/plots/_render.py

Shared helper for the white-paper scenario-plot scripts (Stage 16).
Parses the canonical 9-column telemetry CSV produced by the
scenario runner and renders the standard temperature / fan-duty
figure with fault-event markers.  One thin `plot_<scenario>.py`
script per figure (PRD section 12.3) delegates here.

The leading underscore keeps this off the `plot_*.py` glob the
pipeline iterates -- it is a helper, not a figure script.
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")            # headless: no display, CI-safe
import matplotlib.pyplot as plt  # noqa: E402

# Telemetry signal IDs -- core/thermal_signals.h.
SIG_ZONE_TEMP_0     = 0x0100
SIG_ACTUATOR_DUTY_0 = 0x0200

# Repo root, four levels up from docs/paper/figures/plots/.
REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))


def load(csv_path):
    """Parse a canonical telemetry CSV.

    Returns (samples, events):
      samples -- dict signal_id -> list of (ts_ms, value)
      events  -- list of (ts_ms, code)
    """
    samples, events = {}, []
    with open(csv_path, newline="") as f:
        rdr = csv.reader(f)
        next(rdr, None)                       # discard header row
        for row in rdr:
            if len(row) < 9:
                continue
            ts, row_type, sid, val = (int(row[0]), row[1],
                                      int(row[2]), int(row[3]))
            if row_type == "S":
                samples.setdefault(sid, []).append((ts, val))
            elif row_type == "E":
                events.append((ts, sid))
    return samples, events


def render(scenario, title):
    """Render the standard temp/duty figure for `scenario`.

    Reads /tmp/scenario-<scenario>.csv (left by the scenario
    runner) and writes docs/paper/figures/<scenario>.pdf.  The PDF
    is emitted without a CreationDate so re-running the pipeline on
    unchanged data produces byte-stable output.
    """
    csv_path = os.path.join("/tmp", f"scenario-{scenario}.csv")
    out_path = os.path.join(REPO_ROOT, "docs", "paper", "figures",
                            f"{scenario}.pdf")
    samples, events = load(csv_path)
    temp = samples.get(SIG_ZONE_TEMP_0, [])
    duty = samples.get(SIG_ACTUATOR_DUTY_0, [])

    fig, ax_temp = plt.subplots(figsize=(6.0, 3.4))
    ax_duty = ax_temp.twinx()

    if temp:
        ax_temp.plot([t / 1000.0 for t, _ in temp],
                     [v / 1000.0 for _, v in temp],
                     color="#c0392b", linewidth=1.4,
                     label="zone temperature")
    if duty:
        ax_duty.plot([t / 1000.0 for t, _ in duty],
                     [v for _, v in duty],
                     color="#2c3e50", linewidth=1.4,
                     label="fan duty")

    for ts_ms, _code in events:
        ax_temp.axvline(ts_ms / 1000.0, color="#7f8c8d",
                        linestyle="--", linewidth=0.8)

    ax_temp.set_xlabel("time (s)")
    ax_temp.set_ylabel("zone temperature (°C)", color="#c0392b")
    ax_duty.set_ylabel("fan duty (0–255)", color="#2c3e50")
    ax_duty.set_ylim(0, 270)
    ax_temp.set_title(title)
    ax_temp.grid(True, alpha=0.3)

    lines = ax_temp.get_lines()[:1] + ax_duty.get_lines()[:1]
    if events:
        lines.append(plt.Line2D([], [], color="#7f8c8d",
                                 linestyle="--", linewidth=0.8,
                                 label="fault / safety event"))
    ax_temp.legend(handles=lines, loc="upper left", fontsize=8)

    fig.tight_layout()
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    fig.savefig(out_path, metadata={"CreationDate": None})
    plt.close(fig)
    print(f"figures: wrote {out_path}")
