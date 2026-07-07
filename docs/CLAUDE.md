# CLAUDE.md — Heatstart for a fresh Claude session on `thermal-core`

This file is a one-page brief for a fresh Claude session resuming work
on this repo. It does not duplicate the PRD, the implementation plan, or
the white paper — it points you at them, then captures the **working
conventions**, the **bench-side lessons**, and the **invariants that are
easy to break** that aren't documented elsewhere.

> **Note on path.** Conventional auto-loaded location is `CLAUDE.md`
> at the repo root. This file lives at `docs/CLAUDE.md` per the
> project owner's instruction; future sessions need to be pointed at
> it (or it can be symlinked / moved to the root if auto-loading is
> wanted).

## 1. What this is

`thermal-core` is a portable C99 closed-loop thermal-control core that
links into a Linux daemon (`platform/linux/thermalcored`), an ESP32-C3
firmware in three build modes (`platform/esp32_idf/`), and a CH32V003
STANDALONE port (`platform/ch32v003/`). A white paper at `docs/paper/`
is the public artefact; the PRD + implementation plan are the spec.

**Read these first**, in this order:

| file | purpose |
|---|---|
| `docs/thermal-core-prd.md` (v0.21+) | the spec — wire formats, signal IDs, numeric constants |
| `docs/thermal-core-implementation-plan.md` (v0.22+) | stage-by-stage roll-out + the **Shipped Nx bullets** are the project's narrative |
| `README.md` | quick-start + how to build / test / flash each target |
| `docs/paper/output/thermal-core-spec.pdf` (Draft 0.12+) | the public-facing description |
| `git log --oneline -20` | recent commits — the most current state |

The Stages summary table in §9 of the impl-plan gives current state at
a glance.

## 2. Project state at a glance (as of this writing)

- **All v1 stages 0–16 closed**, plus post-v1 stages **17–20** closed:
  - Stage 17: fan-health detector (advisory, host-on, MCU-off by default).
  - Stage 18: CH32V003 STANDALONE port + tiny profile.
  - Stage 19: CH32 host-command channel + `thermal-telemetry-tool` (C++).
  - Stage 20: fan-health enabled on CH32 with a real measured baseline; two
    reference fan configs (NF-A8 active, Arctic P8 PWM PST alt).
- Two Codex review rounds (v14, v15) consumed and folded in.
- Impl-plan **v0.22**; paper **Draft 0.12**; PRD **v0.21**.

## 3. Workflow conventions (the most important section)

These are the rules of the road. They evolved across the session and
are easy to forget.

- **The project owner (Albert) pushes commits manually.** Claude commits
  locally; Albert reviews `git log` and pushes. **Never push, never
  force-push, never amend a pushed commit.**
- **Stage sub-commits.** Each stage ships in named sub-commits — `19a/3`,
  `19b/3`, `19c/3`, etc. — listed in the impl-plan as "Shipped Nx"
  bullets. Append new bullets *after* the last (this bug has recurred
  twice; see auto-memory).
- **Commit messages**: use HEREDOC; end with the `Co-Authored-By: Claude
  Opus 4.7 (1M context) <noreply@anthropic.com>` line.
- **Codex review loop**: `tmp-review/codex-implementation-review-vN.md`
  files appear regularly. Triage honestly — push back when a finding is
  wrong, accept when right, confirm with the user before applying, and
  fix in a single commit referencing the version (`Fix Codex vN
  findings: ...`). `tmp-review/` is git-ignored — never commit it.
- **The byte-for-byte invariant**. Build-gated CH32 features are supposed
  to leave the shipping default `make build-ch32` firmware byte-for-byte
  identical when their flags are off. Verify by git-stash-comparing the
  `main.bin` sha256. The current default sha after the Fable v2
  core-behavior fixes is
  `706089e8dd4747e0dc5672d74192216262a4e1138c86f49609675897d888049a`
  (13 300 B).
  If a change
  unrelated to default control behavior perturbs the default, gate it
  behind a build flag.
- **Don't commit**: `.claude/`, `tmp*/`, `build/`, `generated/`,
  rendered figure PDFs (`docs/paper/figures/*.pdf` is git-ignored), or
  the `figure_manifest.yaml`'s `git_sha` churn from a local `make
  paper-figures` (revert with `git checkout` — it's not meaningful).
- **No PRD/impl-plan/paper claims you can't ship code for.** "ESP32/MCU:
  build-success ≠ runtime-correct" — never state unverified platform
  assumptions as fact (see auto-memory).
- **Local env diverges from CI.** The dev machine has package versions
  and packages a bare `ubuntu-latest` runner lacks; a green local build
  doesn't predict CI (see auto-memory).

## 4. Where things live

```
core/                  portable C99 core (no heap, no syscalls)
protocol/              binary wire codec + OBD-II
platform/linux/        thermalcored daemon + BSPs
platform/esp32_idf/    ESP-IDF firmware (3 build modes)
platform/ch32v003/     CH32V003 STANDALONE port
  configs/             two reference JSON configs (NF-A8 + Arctic P8 alt)
  ch32fun/             pinned git submodule
tools/                 host tools (Python + one C++ tool)
  thermal-telemetry-tool/   C++ bench driver for the CH32 command channel
  thermalcore-scenario/     plant simulator + scenario runner
  json2static.py            JSON -> static C config codegen
test/
  unit/                test_*.c host unit tests (auto-discovered)
  replay/              C-vs-golden + C-vs-Python-reference checks
  parity/              canonical-CSV serializer (shared by host + MCU)
  reference/           pure-integer Python references
docs/
  paper/               LaTeX paper + figures + captured-artifact data
  thermal-core-prd.md  spec
  thermal-core-implementation-plan.md   stage-by-stage plan
configs/               canonical Linux configs (minimal, demo, etc.)
```

## 5. Build / test entry points

From the repo root:

```bash
make test                  # host unit suite (auto-discovers test_*.c)
make replay                # C + Python-reference replay parity
make verify-portability    # no-heap/no-syscall scan of core/
make test-tiny-profile     # the unit suite under the constrained MCU profile
make scenario              # 11 PRD §9.1 scenarios on the sim plant
make determinism           # scenarios * 2 + gcc-vs-clang SHA-256 parity
make build-esp32           # ESP32-C3 firmware (3 build modes via env)
make build-ch32            # CH32V003 STANDALONE (default, no features)
make build-ch32 CH32_TELEMETRY=1            # + canonical-CSV tap + fan-health
make build-ch32 CH32_COMMAND=1              # + Stage 19 bench command channel
make build-ch32 CH32_CONFIG=configs/ch32v003-standalone-arctic-p8.json
make telemetry-tool        # C++ host tool (tools/thermal-telemetry-tool/)
make -C docs/paper         # build the white paper PDF
make paper-figures         # regenerate all paper figures from data
```

CI matrix legs match these; see `.github/workflows/ci.yml`.

## 6. CH32 bench setup (hard-won)

These caught us out across Stages 18–20 and would catch a fresh session
out the same way:

- **Power the CH32 board from the dongle's VCC pin.** Sharing supply +
  ground is critical for serial-link signal integrity. Separately-
  powered boards joined only by a GND jumper produced an `-err` flood
  (the firmware framed PD5 transmissions back into PD6 as crosstalk).
  Shared supply was the actual fix.
- **Build flags**: `CH32_COMMAND=1` implies `CH32_TELEMETRY=1` at the
  platform Makefile via a single-place resolution block — but never
  forward feature flags as **explicit blanks** from a sub-make (this
  bit us in v14 F-1). The command/bypass image keeps fan-health
  compiled out by default for flash headroom; use the telemetry or
  Arctic legs to exercise fan-health on the chip.
- **Tio baud must match `CH32_TELEMETRY_BAUD`** — the default is
  `9600`; we typically use `=115200`. A mismatch shows as a clean
  repeating garbage pattern.
- **`bypass` mode disables thermal regulation.** `loop off` is bench
  only; the host tool's `pwmsweep` brackets it with `loop off` then
  `loop on`. Don't leave a board in bypass.
- **Two reference fans ship**: NF-A8 (active config) and Arctic P8 PWM
  PST (alt config); swap with `CH32_CONFIG=`. Each fan's sweep CSV is
  committed under `docs/paper/data/`.

## 7. Figure pipeline notes

- The **figure-freshness gate** (`tools/check_figure_freshness.py`)
  iterates `plot_*.py` under `docs/paper/figures/plots/`. Renderers for
  **captured artefacts** (not regenerable from scenarios) must be named
  `render_*.py` to stay outside the gate — `render_ch32_capture.py` and
  `render_ch32_sweep.py` are the existing ones. `make paper-figures`
  invokes them explicitly.
- Don't commit `figure_manifest.yaml` `git_sha`-only churn.

## 8. Memory + auto-loaded context

The user-specific memory lives at
`~/.claude/projects/-home-testpc-git-repos-claude-2026-tmp2-thermal-core/memory/`
and is loaded automatically. `MEMORY.md` is the index; `feedback_*.md`
files describe past corrections (most importantly: don't push,
impl-plan bullet ordering, CI diagnosis surface, firmware assumptions,
local env diverges from CI).

## 9. Possible next directions (if asked "what's next?")

Nothing is in flight. Pickable items, in roughly increasing scope:

- **Stage 17 follow-on remainder**: NVS / file persistence of the
  fan-health baseline, a `thermalcore-fanhealth` CLI, scheduled-probe
  mode, BLOCKED detection, the 2D temperature-compensated baseline,
  bench dust-loading experiments + paper supplement (all deferred and
  documented in the Stage 17 Follow-on bullet).
- **Stage 18 follow-on**: the calibrated benchmark sweep on
  instrumented hardware — controlled ambient, calibrated heat source,
  acoustic SPL measurement, ESP32-C3 per-tick timing — which would
  close the bullet list in §10 *What This Section Does Not Yet Claim*
  in the paper.
- **Paper polish**: §11 Honest limitations, §13 Conclusions, the §12
  Future-work table.
- **Codex follow-ups**: tmp-review reviews triggered by Albert when he
  wants another pass.

## 10. Resuming work — checklist for a fresh session

1. Read this file.
2. Skim `docs/thermal-core-implementation-plan.md` §9 (Stages summary)
   and the most recent "Shipped Nx" bullets.
3. `git log --oneline -20` and read the most recent commit messages.
4. Check `tmp-review/` for any pending Codex review files.
5. Read your auto-memory's `MEMORY.md`.
6. Confirm with Albert what to work on; sub-commit; do not push.
