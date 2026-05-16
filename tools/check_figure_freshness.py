"""tools/check_figure_freshness.py

Stage 16 figure-freshness gate.

Regenerates the canonical scenario CSVs that back the white-paper
figures, re-hashes them, and asserts the committed manifest and the
paper's \\datasha{} captions still match the data:

  (a) every docs/paper/figures/manifest.yaml csv_sha256 (and
      config_sha256) equals a freshly regenerated value, and the
      manifest covers exactly the plot-script set;
  (b) every \\datasha{<hex>} in docs/paper/src/*.tex is the prefix
      of some manifest entry's csv_sha256.

Stale or hand-edited figures fail loudly.  Run by the
`figure-freshness` CI job -- needs the daemon build + scenario
runner, not texlive.
"""
import glob
import os
import re
import subprocess
import sys

import yaml

import figure_manifest as fm  # same tools/ dir, on sys.path[0]

SRC_DIR = os.path.join(fm.REPO_ROOT, "docs", "paper", "src")
RUN_PY = os.path.join(fm.REPO_ROOT, "tools", "thermalcore-scenario",
                      "run.py")
_DATASHA_RE = re.compile(r"\\datasha\{([0-9a-f]+)\}")


def regenerate(scenario):
    """Run the scenario runner so /tmp/scenario-<scenario>.csv is fresh."""
    subprocess.run(
        [sys.executable, RUN_PY,
         os.path.join("scenarios", f"{scenario}.scn")],
        cwd=fm.REPO_ROOT, check=True, stdout=subprocess.DEVNULL)


def main():
    errors = []

    with open(fm.MANIFEST_PATH) as f:
        manifest = yaml.safe_load(f)
    entries = {e["scenario"]: e for e in manifest.get("figures", [])}

    # The manifest must cover exactly the plot-script set.
    plot_set = set(fm.figure_scenarios())
    manifest_set = set(entries)
    if plot_set != manifest_set:
        errors.append(
            f"manifest scenarios {sorted(manifest_set)} != plot-script "
            f"set {sorted(plot_set)} -- run `make paper-figures`")

    # (a) each csv_sha256 / config_sha256 matches a fresh regeneration.
    for scenario in sorted(plot_set & manifest_set):
        entry = entries[scenario]
        regenerate(scenario)
        fresh_csv = fm.sha256_file(fm.csv_path(scenario))
        if fresh_csv != entry.get("csv_sha256"):
            errors.append(
                f"{scenario}: csv_sha256 stale -- manifest "
                f"{entry.get('csv_sha256')}, regenerated {fresh_csv}")
        fresh_cfg = fm.sha256_file(fm.config_path(scenario))
        if fresh_cfg != entry.get("config_sha256"):
            errors.append(
                f"{scenario}: config_sha256 stale -- manifest "
                f"{entry.get('config_sha256')}, file {fresh_cfg}")

    # (b) every \datasha{} in the paper prefixes a manifest csv_sha256.
    all_shas = [e.get("csv_sha256", "") for e in
                manifest.get("figures", [])]
    datasha_count = 0
    for tex in sorted(glob.glob(os.path.join(SRC_DIR, "*.tex"))):
        with open(tex) as f:
            text = f.read()
        for m in _DATASHA_RE.finditer(text):
            datasha_count += 1
            tag = m.group(1)
            if not any(sha.startswith(tag) for sha in all_shas):
                errors.append(
                    f"{os.path.basename(tex)}: \\datasha{{{tag}}} "
                    f"matches no manifest csv_sha256")

    if errors:
        print("figure-freshness: FAIL")
        for e in errors:
            print(f"  - {e}")
        return 1
    print(f"figure-freshness: OK -- {len(plot_set)} figures, "
          f"{datasha_count} caption tag(s), manifest consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
