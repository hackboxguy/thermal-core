"""Render the CH32V003 on-hardware capture figures.

Unlike the scenario plot scripts (`plot_<scenario>.py`), this renders
*recorded* bench captures under docs/paper/data/ -- canonical
telemetry CSVs logged over the CH32V003 UART telemetry tap. They are
not regenerable scenarios, so they carry no manifest entry and no
`\\datasha{}` caption, and sit outside the figure-freshness gate; the
committed CSVs are their provenance.

Two figures:
  ch32v003-capture.csv       -> ch32v003-capture.pdf  (heat/cool)
  ch32v003-stall-capture.csv -> ch32v003-stall.pdf    (fan-stall fault)

The leading non-`plot_` name keeps this off the `plot_*.py` glob that
figure_manifest.py / check_figure_freshness.py iterate -- those govern
the regenerable scenario figures only. `make paper-figures` invokes
this script explicitly.
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")            # headless: no display, CI-safe
import matplotlib.pyplot as plt        # noqa: E402
import matplotlib.patches as mpatches  # noqa: E402

# Telemetry signal IDs / event codes -- core/thermal_signals.h,
# core/thermal_events.h.
SIG_ZONE_TEMP_0     = 0x0100
SIG_ACTUATOR_DUTY_0 = 0x0200
SIG_ACTUATOR_RPM_0  = 0x0210
EV_FAULT_ENTER      = 0x1000
EV_FAULT_CLEAR      = 0x1002
EV_SAFETY_OVERRIDE  = 0x1100

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
DATA_DIR = os.path.join(REPO_ROOT, "docs", "paper", "data")
FIG_DIR  = os.path.join(REPO_ROOT, "docs", "paper", "figures")


def load(path):
    """Parse a canonical 9-column telemetry CSV. Tolerates an optional
    header row, blank lines, and `#`-comment diagnostic lines (the
    per-tick step-timing probe). Returns (samples, events)."""
    samples, events = {}, []
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if len(row) < 4 or row[1] not in ("S", "E"):
                continue                  # header / blank / `#` comment
            try:
                ts, sid, val = int(row[0]), int(row[2]), int(row[3])
            except ValueError:
                continue
            if row[1] == "S":
                samples.setdefault(sid, []).append((ts, val))
            else:
                events.append((ts, sid))
    return samples, events


def render(csv_name, out_name, title):
    samples, events = load(os.path.join(DATA_DIR, csv_name))
    temp = samples.get(SIG_ZONE_TEMP_0, [])
    duty = samples.get(SIG_ACTUATOR_DUTY_0, [])
    rpm  = samples.get(SIG_ACTUATOR_RPM_0, [])
    t0 = temp[0][0] if temp else 0        # normalise x to start at 0

    fig, ax_t = plt.subplots(figsize=(7.2, 3.6))
    ax_d = ax_t.twinx()
    ax_r = ax_t.twinx()
    ax_r.spines["right"].set_position(("outward", 54))

    # Shade each stall-fault window: FAULT_ENTER -> the next FAULT_CLEAR.
    enters = sorted(t for t, c in events if c == EV_FAULT_ENTER)
    clears = sorted(t for t, c in events if c == EV_FAULT_CLEAR)
    shaded = False
    for e in enters:
        c = next((x for x in clears if x >= e), None)
        if c is not None:
            ax_t.axvspan((e - t0) / 1000.0, (c - t0) / 1000.0,
                         color="#c0392b", alpha=0.12, zorder=0)
            shaded = True

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

    overrides = [t for t, c in events if c == EV_SAFETY_OVERRIDE]
    for o in overrides:
        ax_t.axvline((o - t0) / 1000.0, color="#7f8c8d",
                     linestyle="--", linewidth=0.8)

    ax_t.set_xlabel("time (s)")
    ax_t.set_ylabel("zone temperature (°C)", color="#c0392b")
    ax_d.set_ylabel("fan duty (0–255)", color="#2c3e50")
    ax_r.set_ylabel("fan RPM", color="#27ae60")
    ax_d.set_ylim(0, 270)
    ax_t.set_title(title)
    ax_t.grid(True, alpha=0.3)

    handles = [lt, ld, lr]
    if shaded:
        handles.append(mpatches.Patch(facecolor="#c0392b", alpha=0.25,
                                      label="stall fault active"))
    if overrides:
        handles.append(plt.Line2D([], [], color="#7f8c8d",
                                  linestyle="--", linewidth=0.8,
                                  label="safety-override event"))
    ax_t.legend(handles=handles, loc="upper left", fontsize=8)

    fig.tight_layout()
    out_path = os.path.join(FIG_DIR, out_name)
    os.makedirs(FIG_DIR, exist_ok=True)
    fig.savefig(out_path, metadata={"CreationDate": None})
    plt.close(fig)
    print(f"figures: wrote {out_path}")


def main():
    render("ch32v003-capture.csv", "ch32v003-capture.pdf",
           "CH32V003 STANDALONE: on-hardware heat/cool capture")
    render("ch32v003-stall-capture.csv", "ch32v003-stall.pdf",
           "CH32V003 STANDALONE: fan-stall fault on hardware")


if __name__ == "__main__":
    main()
