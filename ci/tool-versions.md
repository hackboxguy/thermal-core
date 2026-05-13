# Tool versions of record

This file is the canonical inventory of toolchain versions for thermal-core CI and developer environments. The implementation plan (`docs/thermal-core-implementation-plan.md` §5 Stage 0) names this file as the place to look when CI uses a tool whose version matters.

Pin granularity:
- **Python deps**: pinned in `requirements-dev.txt`. This file references that file but does not duplicate its pins.
- **OS packages (gcc, clang, clang-tidy, cppcheck, lcov)**: pinned in the workflow `.yml` and documented here.
- **Docker images (ESP-IDF)**: pinned by `image:tag@sha256:…` and documented here.

## Python

| Tool | Version | Source | Consumed by |
|---|---|---|---|
| Python | 3.12 | `actions/setup-python@v5` | Stage 0 commit 2 onward (CI dep install proves the file parses) |
| Python deps | see `requirements-dev.txt` | pip | Stage 2 onward |

## C compilers

**Policy: floating, with ubuntu-latest.** CI builds against whatever gcc/clang versions GitHub's `ubuntu-latest` runners provide. We accept drift risk in exchange for early warning when a compiler upgrade introduces a new `-Wextra` diagnostic that would otherwise creep up on us at release time.

If drift becomes a recurring noise source (i.e., more than one PR per quarter has to fight an upstream compiler change unrelated to the PR's intent), this policy moves to a pinned `ubuntu-22.04` or `ubuntu-24.04` image plus an explicit `clang-NN` install from the LLVM apt repo.

| Tool | Version | Source | Consumed by |
|---|---|---|---|
| gcc | floating | `ubuntu-latest` | Stage 0 |
| clang | floating | `ubuntu-latest` apt | Stage 0 |

## Static analysis

Both tools land in CI in the commit that introduces the job that runs them. The intended pin is recorded here so the consuming PR has a target to verify against.

| Tool | Intended version | Source | Consumed by |
|---|---|---|---|
| clang-tidy | clang-tidy-17 (or current LTS at consumption time) | LLVM apt repo | Stage 7 |
| cppcheck | 2.13.x | Ubuntu apt | Stage 8 |

## Coverage

| Tool | Intended version | Source | Consumed by |
|---|---|---|---|
| lcov | 2.0.x | Ubuntu apt | Stage 9 (visibility only, no gate) |

## Fuzzing

libFuzzer ships with clang. The fuzzer version is implicitly the clang version above. The seed corpora live under `test/fuzz/seeds/`.

| Tool | Source | Consumed by |
|---|---|---|
| libFuzzer | `clang -fsanitize=fuzzer` | Stage 9 (JSON), Stage 10 (wire) |

## Embedded toolchain

Pinned by docker image tag. The image is run in CI for the `build-esp32` job (`.github/workflows/ci.yml`, Stage 13d) via `espressif/esp-idf-ci-action@v1`; locally, `idf.py` against a matching IDF install also works (see `make build-esp32`).

| Tool | Intended pin | Consumed by |
|---|---|---|
| ESP-IDF | `espressif/idf:v5.5.2` (SHA pin deferred until image drift causes CI flakes) | Stage 13 |

The reference target is **ESP32-C3 (RISC-V)** — selected over the PRD §4.1 ESP32-C6 nomination during impl-plan-review-v3 because Albert's existing bench rig is C3-based. The canonical Kconfig pin lives in `platform/esp32_idf/sdkconfig.defaults` (`CONFIG_IDF_TARGET="esp32c3"`); the canonical daemon config consumed by CI is `platform/esp32_idf/configs/esp32-c3-standalone.json`. C6 remains a future-portability claim; the portable C99 `core/` is target-agnostic and a C6 matrix leg lands if/when the hardware does.

## Bump policy

A toolchain bump is its own PR. Required:

1. The PR touches only `requirements-dev.txt`, `ci/tool-versions.md`, and the workflow file(s) that consume the pinned version. No feature code, no unrelated cleanup.
2. The PR description states the rationale (security fix? new feature we want? CI green-fix?).
3. The PR's CI must be green on its own merits — a bump that requires "and also fix this one warning" is two PRs, not one.
4. If a bump introduces a new `-Werror`-tripping diagnostic, the fix is either (a) the new diagnostic is correct and we fix the code, or (b) the new diagnostic is wrong and we revert the bump and file an upstream report. Suppressing new diagnostics with `#pragma` is not the answer.
