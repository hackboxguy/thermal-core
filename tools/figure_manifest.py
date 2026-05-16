"""tools/figure_manifest.py

Shared helpers for the white-paper figure manifest (Stage 16) and,
run as a script, the manifest writer itself.

docs/paper/figures/manifest.yaml is the load-bearing record of what
each generated figure depends on.  `make paper-figures` runs this
module to (re)write it; tools/check_figure_freshness.py imports the
helpers below to verify it.

The figure set is derived from the plot scripts -- one figure per
docs/paper/figures/plots/plot_<scenario>.py -- so the scripts are
the single source of truth and the manifest can never silently
drift from them (check_figure_freshness.py asserts the two agree).
"""
import glob
import hashlib
import os
import subprocess
import sys

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), ".."))
PLOTS_DIR = os.path.join(REPO_ROOT, "docs", "paper", "figures", "plots")
MANIFEST_PATH = os.path.join(REPO_ROOT, "docs", "paper", "figures",
                             "manifest.yaml")

_MANIFEST_HEADER = """\
# docs/paper/figures/manifest.yaml -- GENERATED, do not hand-edit.
#
# Written by `make paper-figures` (tools/figure_manifest.py).  The
# load-bearing record of what each white-paper figure depends on;
# the figure-freshness CI gate (tools/check_figure_freshness.py)
# regenerates the scenario CSVs and asserts these hashes still hold.
#
# config_sha256 is the SHA-256 of the config JSON file bytes -- a
# plain content hash, not the canonical C config hash.  No Python
# canonical encoder exists (impl-plan Stage 12 deferral) and the
# csv_sha256 is the actual freshness signal; the config hash is
# provenance only.
"""


def figure_scenarios():
    """The figure set: one scenario per plots/plot_<scenario>.py."""
    names = []
    prefix, suffix = "plot_", ".py"
    for path in sorted(glob.glob(os.path.join(PLOTS_DIR, "plot_*.py"))):
        stem = os.path.basename(path)
        names.append(stem[len(prefix):-len(suffix)])
    return names


def csv_path(scenario):
    """The telemetry CSV the scenario runner leaves for `scenario`."""
    return os.path.join("/tmp", f"scenario-{scenario}.csv")


def scn_path(scenario):
    return os.path.join(REPO_ROOT, "scenarios", f"{scenario}.scn")


def config_path(scenario):
    """Absolute path to the config JSON a scenario's .scn points at."""
    with open(scn_path(scenario)) as f:
        for line in f:
            line = line.strip()
            if line.startswith("config "):
                rel = line.split(None, 1)[1].strip()
                return os.path.join(REPO_ROOT, rel)
    raise SystemExit(
        f"figure-manifest: {scenario}.scn has no `config` directive")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def git_sha():
    return subprocess.run(
        ["git", "-C", REPO_ROOT, "rev-parse", "HEAD"],
        capture_output=True, text=True, check=True).stdout.strip()


def tool_versions():
    """Versions of the tools that produced the figures + manifest."""
    import matplotlib
    import yaml
    return {
        "python": sys.version.split()[0],
        "matplotlib": matplotlib.__version__,
        "pyyaml": yaml.__version__,
    }


def build_manifest():
    """Assemble the manifest dict from the current /tmp CSVs."""
    figures = []
    for scenario in figure_scenarios():
        cp = csv_path(scenario)
        if not os.path.exists(cp):
            raise SystemExit(
                f"figure-manifest: {cp} missing -- "
                f"run `make paper-figures` to generate it first")
        cfg = config_path(scenario)
        figures.append({
            "scenario": scenario,
            "csv_sha256": sha256_file(cp),
            "config": os.path.relpath(cfg, REPO_ROOT),
            "config_sha256": sha256_file(cfg),
        })
    return {
        "git_sha": git_sha(),
        "tool_versions": tool_versions(),
        "figures": figures,
    }


def write_manifest():
    import yaml
    manifest = build_manifest()
    with open(MANIFEST_PATH, "w") as f:
        f.write(_MANIFEST_HEADER)
        yaml.safe_dump(manifest, f, sort_keys=False,
                       default_flow_style=False)
    print(f"figure-manifest: wrote {MANIFEST_PATH} "
          f"({len(manifest['figures'])} figures)")


if __name__ == "__main__":
    write_manifest()
