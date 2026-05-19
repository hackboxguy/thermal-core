"""Render the CH32V003 on-hardware capture figure.

Unlike the scenario plot scripts (`plot_<scenario>.py`), this renders
a *recorded* bench capture --- docs/paper/data/ch32v003-capture.csv,
the canonical telemetry CSV logged over the CH32V003 UART telemetry
tap during a hand-applied heat/cool test. It is not a regenerable
scenario, so it carries no manifest entry and no `\\datasha{}`
caption, and it sits outside the figure-freshness gate; the committed
CSV is its provenance.

The leading non-`plot_` name keeps this off the `plot_*.py` glob that
figure_manifest.py / check_figure_freshness.py iterate -- those govern
the regenerable scenario figures only. `make paper-figures` invokes
this script explicitly.
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")            # headless: no display, CI-safe
import matplotlib.pyplot as plt  # noqa: E402

# Telemetry signal IDs / event codes -- core/thermal_signals.h,
# core/thermal_events.h.
SIG_ZONE_TEMP_0     = 0x0100
SIG_ACTUATOR_DUTY_0 = 0x0200
SIG_ACTUATOR_RPM_0  = 0x0210

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
CSV_PATH = os.path.join(REPO_ROOT, "docs", "paper", "data",
                        "ch32v003-capture.csv")
OUT_PATH = os.path.join(REPO_ROOT, "docs", "paper", "figures",
                        "ch32v003-capture.pdf")


def load(path):
    """Parse a canonical 9-column telemetry CSV. Tolerates an optional
    header row and blank lines. Returns (samples, events)."""
    samples, events = {}, []
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if len(row) < 4 or row[1] not in ("S", "E"):
                continue                  # header / blank line
            try:
                ts, sid, val = int(row[0]), int(row[2]), int(row[3])
            except ValueError:
                continue
            if row[1] == "S":
                samples.setdefault(sid, []).append((ts, val))
            else:
                events.append((ts, sid))
    return samples, events


def main():
    samples, events = load(CSV_PATH)
    temp = samples.get(SIG_ZONE_TEMP_0, [])
    duty = samples.get(SIG_ACTUATOR_DUTY_0, [])
    rpm  = samples.get(SIG_ACTUATOR_RPM_0, [])
    t0 = temp[0][0] if temp else 0        # normalise x to start at 0

    fig, ax_t = plt.subplots(figsize=(7.2, 3.6))
    ax_d = ax_t.twinx()
    ax_r = ax_t.twinx()
    ax_r.spines["right"].set_position(("outward", 54))

    lt, = ax_t.plot([(t - t0) / 1000.0 for t, _ in temp],
                    [v / 1000.0 for _, v in temp],
                    color="#c0392b", linewidth=1.4,
                    label="zone temperature")
    ld, = ax_d.plot([(t - t0) / 1000.0 for t, _ in duty],
                    [v for _, v in duty],
                    color="#2c3e50", linewidth=1.3, label="fan duty")
    lr, = ax_r.plot([(t - t0) / 1000.0 for t, _ in rpm],
                    [v for _, v in rpm],
                    color="#27ae60", linewidth=1.1, label="fan RPM")

    for ts_ms, _code in events:
        ax_t.axvline((ts_ms - t0) / 1000.0, color="#7f8c8d",
                     linestyle="--", linewidth=0.8)

    ax_t.set_xlabel("time (s)")
    ax_t.set_ylabel("zone temperature (°C)", color="#c0392b")
    ax_d.set_ylabel("fan duty (0–255)", color="#2c3e50")
    ax_r.set_ylabel("fan RPM", color="#27ae60")
    ax_d.set_ylim(0, 270)
    ax_t.set_title("CH32V003 STANDALONE: on-hardware heat/cool capture")
    ax_t.grid(True, alpha=0.3)

    handles = [lt, ld, lr]
    if events:
        handles.append(plt.Line2D([], [], color="#7f8c8d",
                                  linestyle="--", linewidth=0.8,
                                  label="safety-override event"))
    ax_t.legend(handles=handles, loc="upper left", fontsize=8)

    fig.tight_layout()
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    fig.savefig(OUT_PATH, metadata={"CreationDate": None})
    plt.close(fig)
    print(f"figures: wrote {OUT_PATH}")


if __name__ == "__main__":
    main()
