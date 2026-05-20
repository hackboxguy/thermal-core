"""Render the CH32V003 PWM-to-RPM sweep figure for the two reference
fans shipped in the repo.

Each fan was swept on the deployed CH32V003 via thermal-telemetry-tool
--action=pwmsweep (1 % steps, 1 .. 100 %, 4 s settle per step). Each
fan also has an 8-point fan_health.baseline authored into a CH32
config; this figure overlays the raw sweeps with their baselines so
the same firmware/core can be seen handling fans with substantially
different curves.

Like the other ch32v003-*.pdf figures, the input CSVs are recorded
bench artifacts (not regenerable scenario data), so this script sits
outside the `plot_*.py` glob the figure-freshness gate iterates --
`make paper-figures` invokes it explicitly.
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt        # noqa: E402

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
DATA_DIR = os.path.join(REPO_ROOT, "docs", "paper", "data")
FIG_DIR  = os.path.join(REPO_ROOT, "docs", "paper", "figures")

# Each entry: csv (run 1 -- the others are repeatability-only),
# baseline (mirrors the firmware fan_health.baseline JSON block),
# and a colour. Keep the baselines in sync with platform/ch32v003/
# configs/ch32v003-standalone*.json.
FANS = [
    {
        "name":     "Noctua NF-A8",
        "csv":      "ch32v003-nf-a8-sweep.csv",
        "color":    "#2c3e50",
        "baseline": [(26,  210), (51,  435), (77,  705), (100,  915),
                     (128, 1170), (160, 1440), (220, 1980), (255, 2250)],
    },
    {
        "name":     "Arctic P8 PWM PST",
        "csv":      "ch32v003-arctic-p8-pst-sweep-run1.csv",
        "color":    "#d35400",
        "baseline": [(26,  370), (51,  500), (77,  850), (100, 1170),
                     (128, 1540), (160, 1950), (220, 2670), (255, 3070)],
    },
]


def load(path):
    duties, rpms = [], []
    with open(path, newline="") as f:
        reader = csv.reader(f)
        next(reader, None)                # skip header
        for row in reader:
            if len(row) < 3:
                continue
            try:
                duties.append(int(row[1]))
                rpms.append(int(row[2]))
            except ValueError:
                continue
    return duties, rpms


def render():
    fig, ax = plt.subplots(figsize=(7.2, 4.0))

    # Both fans share the same spin-up dead zone (the first run of
    # zero RPMs in each sweep ends at duty 23).
    ax.axvspan(0, 23, color="0.88", alpha=0.6, zorder=0,
               label="dead zone (fan does not spin)")

    for f in FANS:
        duties, rpms = load(os.path.join(DATA_DIR, f["csv"]))
        ax.plot(duties, rpms, color=f["color"], linewidth=1.1,
                marker=".", markersize=2.5,
                label=f"{f['name']} -- sweep")
        bx = [p[0] for p in f["baseline"]]
        by = [p[1] for p in f["baseline"]]
        ax.plot(bx, by, color=f["color"], marker="o", linestyle="",
                markersize=7, markerfacecolor="white",
                markeredgewidth=1.6,
                label=f"{f['name']} -- fan\\_health baseline")

    ax.set_xlabel("PWM duty (0--255)")
    ax.set_ylabel("fan RPM")
    ax.set_xlim(0, 260)
    ax.set_ylim(bottom=0)
    ax.grid(True, alpha=0.3)
    ax.set_title("CH32V003 STANDALONE: PWM-to-RPM sweep, two reference fans")
    ax.legend(loc="upper left", fontsize=8)

    fig.tight_layout()
    out_path = os.path.join(FIG_DIR, "ch32v003-sweep.pdf")
    os.makedirs(FIG_DIR, exist_ok=True)
    fig.savefig(out_path, metadata={"CreationDate": None})
    plt.close(fig)
    print(f"figures: wrote {out_path}")


if __name__ == "__main__":
    render()
