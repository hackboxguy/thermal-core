"""Render the CH32V003 NF-A8 PWM-to-RPM sweep figure.

Stage 20 reads docs/paper/data/ch32v003-nf-a8-sweep.csv -- the raw
100-point sweep captured on the deployed fan via
thermal-telemetry-tool --action=pwmsweep -- and renders a duty-vs-RPM
plot, highlighting the 8 baseline points that are authored into the
firmware's fan_health block. Like the other ch32v003-*.pdf figures,
the input CSV is a recorded bench artifact (not regenerable
scenario data), so this script sits outside the `plot_*.py` glob the
figure-freshness gate iterates -- `make paper-figures` invokes it
explicitly.
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

# The 8 baseline points authored into platform/ch32v003/configs/
# ch32v003-standalone.json. The spin-up threshold + four governor-state
# duties (100 / 160 / 220 / 255) -- keep these in sync with the JSON.
BASELINE_POINTS = [(26, 210), (51, 435), (77, 705), (100, 915),
                   (128, 1170), (160, 1440), (220, 1980), (255, 2250)]


def load(path):
    """Parse a `pwm_pct,duty_0_255,rpm` CSV. Returns (duties, rpms)
    arrays for all 100 sweep rows."""
    duties, rpms = [], []
    with open(path, newline="") as f:
        reader = csv.reader(f)
        next(reader, None)                # skip header row
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
    duties, rpms = load(os.path.join(DATA_DIR, "ch32v003-nf-a8-sweep.csv"))

    fig, ax = plt.subplots(figsize=(7.2, 3.6))

    # Mark the spin-up dead zone (first run of zero RPMs) for context.
    spin_up = next((d for d, r in zip(duties, rpms) if r > 0), duties[0])
    ax.axvspan(0, spin_up, color="0.85", alpha=0.6, zorder=0,
               label="fan does not spin")

    ax.plot(duties, rpms, color="#2c3e50", linewidth=1.2,
            marker=".", markersize=3, label="100-point sweep")
    bx = [p[0] for p in BASELINE_POINTS]
    by = [p[1] for p in BASELINE_POINTS]
    ax.plot(bx, by, color="#c0392b", marker="o", linestyle="",
            markersize=7, markerfacecolor="white",
            markeredgewidth=1.6,
            label="firmware fan\\_health baseline (8 points)")

    ax.set_xlabel("PWM duty (0--255)")
    ax.set_ylabel("fan RPM")
    ax.set_xlim(0, 260)
    ax.set_ylim(bottom=0)
    ax.grid(True, alpha=0.3)
    ax.set_title("CH32V003 STANDALONE: NF-A8 PWM-to-RPM sweep")
    ax.legend(loc="upper left", fontsize=8)

    fig.tight_layout()
    out_path = os.path.join(FIG_DIR, "ch32v003-sweep.pdf")
    os.makedirs(FIG_DIR, exist_ok=True)
    fig.savefig(out_path, metadata={"CreationDate": None})
    plt.close(fig)
    print(f"figures: wrote {out_path}")


if __name__ == "__main__":
    render()
