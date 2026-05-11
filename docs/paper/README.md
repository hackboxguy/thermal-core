# thermal-core Paper

This folder is the early migration of the LaTeX technical-document template from `docs/tmp-latex-artifacts-template/`.

Current status:

- `src/thermal-core-spec.tex` is the active root file used by the Makefile.
- The numbered section files contain thermal-core scaffold text to fill in as implementation results land.
- Legacy template entrypoints and unused image assets are ignored locally and are not part of the committed paper scaffold.
- Generated PDFs and LaTeX build artifacts go under `output/` and are ignored.

Build commands:

```sh
make -C docs/paper
make -C docs/paper figures
make -C docs/paper watch
make -C docs/paper clean
```
