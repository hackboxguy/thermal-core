# thermal-core Paper

The LaTeX source for the thermal-core technical paper, currently
**Draft 0.6**. The conceptual and implementation sections are written
against the shipped code; the Evaluation section is preliminary and
reports measured results only (see the document's own Draft Status).

## Layout

- `src/thermal-core-spec.tex` — the root document; it `\input`s the
  numbered section files under `src/`.
- `figures/` — scenario plots (`*.pdf`) rendered by the figure
  pipeline, plus `manifest.yaml`, which records each figure's
  scenario, telemetry-CSV SHA-256, config-file SHA-256, source git
  SHA, and tool versions. The generated PDFs are git-ignored: a local
  matplotlib off the pinned version would not byte-match the release
  build.
- `images/` — hand-drawn and static assets.
- `output/` — generated PDF and LaTeX build artifacts; git-ignored.

## Build

```sh
make -C docs/paper            # build output/thermal-core-spec.pdf
make -C docs/paper figures    # regenerate the scenario plots + manifest
make -C docs/paper watch      # rebuild on source change
make -C docs/paper clean
```

## Gates

- `figure-freshness` (per-PR CI job) runs `tools/check_figure_freshness.py`,
  which regenerates the scenario CSVs and asserts every `manifest.yaml`
  hash and every `\datasha{}` caption tag still matches.
- `release.yml` builds the PDF on `v*` tags with the pinned toolchain
  and uploads it as a workflow artifact.
