# thermal-core — Implementation Plan

**Document status:** Draft v0.6
**Author:** Albert David
**Companion to:** [thermal-core-prd.md](thermal-core-prd.md) (v0.13)

This document describes how to build `thermal-core` incrementally, stage by stage, with the test automation that prevents regressions evolving alongside the code. Stages are ordered by dependency, not by calendar time — each stage closes with a green CI gate, and the next stage starts from that green main.

The guiding bet is simple: **every feature lands in the same PR as the automated tests that prove it works and the golden artifacts that will detect its future regression.** If a feature can't be exercised by an automated test, it's not done.

---

## 1. Guiding principles

1. **Stage-gated, not date-gated.** A stage ends when its CI gate is green on `main`. Time per stage varies; effort per stage is bounded by the size of the deliverable, not by the calendar.
2. **Tests and code ship together.** A PR that adds a feature must also add the tests that exercise it. CI rejects PRs whose new code is not covered by new tests of an appropriate type.
3. **Lean CI grows.** Stage 0 enables unit + build only. Replay joins at Stage 2 (the first stage with deterministic output to compare). Each subsequent milestone adds at most one new rigor layer (sanitizers, then static analysis, then coverage, then fuzz, then cross-build matrix) so noise stays manageable.
4. **Golden replays are the canary.** Once a feature has deterministic behavior, its output is captured to a checked-in golden file. Future changes that alter that output fail CI loudly until the golden is consciously re-recorded.
5. **Reproducibility is testable.** A determinism job re-runs the canonical scenarios and asserts the SHA-256 of the telemetry stream is unchanged. Catches accidental nondeterminism (uninitialized memory, time-dependent math, host floating-point drift) before it ships.
6. **No third-party C unit-test framework.** The C harness lives in `test/unit/harness.h` as ~50 lines of C99 macros. Same harness compiles for ESP32 unit tests later. No Unity, no CMock, no Greatest. This principle constrains the C test runner only — Python-side reference math (numpy, scipy), static analyzers (clang-tidy, cppcheck), coverage tools (lcov), and fuzzers (libFuzzer) are explicitly in scope and pinned in a dev-tools requirements file.
7. **Hardware-in-the-loop is manual.** Bench-rig scenarios run on demand or in a nightly workflow; they don't gate PRs. The simulator (§4.3) is the release-gate plant.
8. **Daemons run deterministically under test.** Anything that gates on byte-equal telemetry (scenario tests, determinism job, replay parity) requires the daemon to use an *injectable* clock and a fixed loop order — drain commands for tick N, build snapshot N, step core, publish telemetry/output N — rather than wall-clock + UDP-arrival-order. The deterministic mode is itself a deliverable, landing no later than Stage 9 (`thermalcored --clock=scenario` or equivalent) so the Stage 12 determinism gate has something deterministic to gate on.
9. **Paper work interleaves with code.** Conceptual paper sections that don't depend on results (motivation, background, architecture prose, acoustic-thermal math, bench rig BOM) are drafted in parallel with Stage 0–7, not deferred to the end. Results-bearing sections (evaluation tables, scenario plots, honest-limitations content) are written only after the relevant code lands and benchmarks have been captured. The abstract and conclusions are written last, when the paper actually knows what it's saying. §6 (Paper update cadence) maps stages to paper sections explicitly.

---

## 2. Test taxonomy

Eight test types, ordered by when they appear in the plan. Every PR runs the ones that exist at that stage; no PR opts out.

### 2.1 Unit tests (Stage 0 onward)
Pure-function tests of one module. No I/O, no allocation beyond stack. Driven by the hand-rolled harness:
- `TEST_CASE(name)` declares a test.
- `EXPECT_EQ`, `EXPECT_NEAR_Q16_16`, `EXPECT_STATUS_OK` etc. assert.
- A single `ctest` runner invokes every test binary.

**Catches:** logic bugs in isolated functions, arithmetic overflow, off-by-one errors, invariant violations under specific inputs.

### 2.2 Build matrix (Stage 0 onward)
CI compiles the core, the daemon, and (once it exists) the ESP32 firmware on every PR. Flags: `-Wall -Wextra -Werror -std=c99 -pedantic` plus target-specific ones.

**Catches:** portability regressions, warnings creeping into hot files, accidental syscall/heap usage in `core/`.

### 2.3 Golden replay tests (Stage 2 onward)
A CSV of inputs is fed through a deterministic function; the output is captured to a CSV; the result is `diff`-compared against a checked-in golden CSV under `test/replay/golden/`. Byte-equal or test fails. Two flavors:

- **Module goldens (Stages 2–6):** the input is a sweep over one module's interface (curve evaluation, IIR filter, governor stepping in isolation) — used while the full control loop doesn't exist yet.
- **Full-step goldens (Stage 7 onward):** the input is a stream of `thermal_input_snapshot_t`; the output is the actuator frame plus full telemetry stream from `thermal_core_step()`.

When a behavior change is intentional, the developer runs `make regen-replay-goldens`, eyeballs the diff (now visible in the PR), and commits the new goldens. Reviewers see exactly what changed.

**Catches:** any unintended change to math, ordering, filter coefficients, governor output, fault timing, or telemetry contents. The most powerful single regression detector in the plan.

### 2.4 Scenario tests (Stage 12 onward)
`.scn` files (per PRD §7.6) run against the deterministic thermal-plant simulator. Assertions in the scenario file (`assert eventually …`, `assert max … <= …`) decide pass/fail. The scenario runner is its own CI job.

**Catches:** integration-level regressions — closed-loop behavior, multi-zone arbitration, acoustic-mask end-to-end, fault recovery.

### 2.5 Determinism tests (Stage 12 onward, once scenarios exist)
Every canonical scenario is run twice with the same config and the same scenario seed; the SHA-256 of the telemetry CSV must match. Then the same scenario is run on a different host (Linux clang vs Linux gcc); the SHA-256 must still match.

**Catches:** uninitialized memory, host-FP drift, hash-table iteration order, any source of accidental nondeterminism.

### 2.6 Smoke tests (Stage 9 onward, once daemon exists)
The Linux daemon is started under CI with a minimal config, given 5 seconds, and expected to exit cleanly on SIGTERM with exit code 0. A separate smoke test starts the daemon and a UDP listener and verifies at least one `TELEM_SAMPLE` frame arrives within 2 seconds.

**Catches:** init-time bugs, signal-handling bugs, never-emits-telemetry bugs that unit tests don't see.

### 2.7 Property tests (Stage 4 onward)
Randomly generated configs (within compile-time maxima) run through `thermal_core_validate_config()`. The validator must either return `THERMAL_OK` or one of the documented error codes — never crash, never use uninitialized memory, never deadlock. Generated via simple QuickCheck-style helpers in `test/property/`. Also used in Stage 8 to fuzz typed `thermal_command_t` values into `thermal_core_apply_command()`.

**Catches:** validator and command-apply crashes on inputs the unit tests didn't think of.

### 2.8 Fuzz tests (Stage 9 onward for JSON, Stage 10 onward for wire decoder)
libFuzzer-driven fuzz of the JSON loader and the wire-frame decoder. Seed corpus = the example configs in `configs/` and a recorded set of valid frames. Run for 60 seconds per CI job; longer in nightly.

**Catches:** parser crashes on adversarial input, OOB reads on truncated frames, length-field handling bugs.

---

## 3. CI growth plan

The CI workflow is `.github/workflows/ci.yml`. Each row below is a `job:` added in the listed stage. Once added, a job runs on every PR.

| Stage added | Job | What it does |
|---|---|---|
| 0 | `build-linux` | gcc + clang, `-Werror`, builds core + harness |
| 0 | `unit` | runs all unit tests under `ctest` |
| 0 | `no-heap-no-syscall` | links a guarded unit-test binary that wraps `malloc`/`free`/`open`/`read`/`write`/`mmap`/`sleep` etc. and fails if any are called from `core/`. Pairs with an `nm -u` symbol allowlist over the compiled `core/` objects. Defends the PRD §4.2 portability promise. |
| 2 | `replay` | runs module/full-step golden replay tests; diff goldens on failure |
| 4 | `property-config` | randomly generated configs through `thermal_core_validate_config()`; no crashes, no undocumented status |
| 6 | `asan-ubsan` | rebuilds with `-fsanitize=address,undefined` and re-runs unit + replay |
| 7 | `clang-tidy` | `clang-tidy` on `core/` initially; expands to include `platform/linux/` when the daemon lands at Stage 9; new warnings fail |
| 8 | `cppcheck` | `cppcheck --error-exitcode=1 core/`; expands to include `platform/linux/` at Stage 9 |
| 8 | `property-command` | extends property testing: random typed `thermal_command_t` values through `thermal_core_apply_command()`; no crashes, only documented statuses |
| 9 | `coverage` | lcov over unit + replay; uploads HTML to the PR as an artifact; **no gate** in v1, visibility only |
| 9 | `smoke-linux` | starts the daemon (in `--clock=scenario` deterministic mode), asserts it boots and emits telemetry |
| 9 | `fuzz-json` | libFuzzer over the JSON loader, 60 s per PR, 30 min nightly |
| 10 | `fuzz-wire` | libFuzzer over the `protocol/` wire decoder, same shape |
| 12 | `scenario` | runs all canonical scenarios; assertions decide pass/fail |
| 12 | `determinism` | reruns scenarios twice, compares SHA-256 of telemetry |
| 13 | `build-esp32` | `idf.py -DIDF_TARGET=esp32c6 build` (RISC-V) on the ESP-IDF docker image; size-budget assertions |
| 15 | `replay-parity` | **conditional gate.** Runs a fixed synthetic input stream through host build and ESP32 standalone; asserts byte-identical telemetry SHA. Becomes a PR/merge-queue gate **only when** a stable ESP32-C6 RISC-V QEMU path or a reliable self-hosted ESP32 runner exists. Until that infrastructure lands, this is a release/nightly gate. The plan does not promise an always-on PR gate before the runner strategy is real. |
| nightly | `hil-tolerance` | runs canonical scenarios on the bench (host + ESP32 HIL, or ESP32 standalone) and asserts behavioral bands rather than SHA equality; gated by `[hil]` PR label or scheduled run |

Three workflows in total:

- **`ci.yml`** — runs on every push and PR. Fast (target: under 10 minutes).
- **`nightly.yml`** — runs on cron. Longer fuzz, full scenario sweep, coverage trend, bench-rig scenarios (when self-hosted runner is available).
- **`release.yml`** — runs on `v*` tags. Builds white paper PDF, attaches release artifacts.

PR-gating CI is filtered to skip the heavy matrix when a change touches only documentation. Desired behavior:

- Docs-only PRs (changes only under `docs/**` or to root-level `*.md` files like `README.md`) trigger a fast `docs-lint` job — markdown link check + table formatting — and skip the build/test matrix.
- Code-only PRs trigger the heavy matrix and skip `docs-lint`.
- Mixed code+docs PRs trigger both. The default GitHub Actions `paths`/`paths-ignore` logic is OR-based per-job rather than per-workflow, so mixed PRs naturally run all jobs that match — exactly what we want.

The YAML can be tuned as we learn what stays noisy; the principle is "text-only iteration cheap, code-touching iteration always fully exercised."

---

## 4. Mock and emulation strategy

The "automated system that verifies runtime behaviour using emulated sensors and data points" lives at four progressively realistic layers. Each layer's tests are owned by the stage that introduces it.

### 4.1 In-process stub (Stage 0 onward)
For unit tests. The test calls `thermal_core_step()` directly with a hand-built `thermal_input_snapshot_t` and reads the output frame. No filesystem, no syscalls, no time. Determinism is automatic.

### 4.2 `bsp_mock_tmpfs` (Stage 9 onward)
Emulates `hwmon`-style sysfs against a tmpfs tree. Daemon-level tests write `/tmp/thermalcore-mock/temp1_input = 75000`, run the daemon for 1 second, and assert the daemon wrote the expected `pwm1` value back. Tests run as a normal user, no root needed.

### 4.3 Deterministic thermal-plant simulator (Stage 12 onward)
Per PRD §9.3. Reusable from `tools/thermalcore-scenario`, `test/replay/`, and `test/unit/`. Q16.16 throughout, deterministic PRNG, bit-for-bit reproducible. This is the closed-loop test bed: the simulator generates temperature evolution from the daemon's PWM commands; the daemon's response is what gets asserted.

### 4.4 `car-can-emulator` over `vcan0` (Stage 11 onward)
Real OBD-II frames generated by the upstream emulator, consumed via SocketCAN on `vcan0`. Tests configure a speed via the emulator's TCP control port (`echo speed 120 | nc localhost 8080`) and assert the daemon's `vehicle_speed` context sample reflects it within `timeout_ms`. No physical CAN hardware needed in CI.

### 4.5 Real hardware (Stage 13 onward, manual / nightly)
Bench-rig scenarios run on demand against the ESP32 standalone build and the ESP32 HIL peripheral build. Same `.scn` files, same assertions, same `thermalcore-probe` collection. Failures surface as nightly workflow failures; not gating PR merges in v1.

---

## 5. The stages

Each stage lists: **Deliverable** (the feature), **Tests added** (the new automated coverage that ships with it), **Regression value** (what about this stage will keep future PRs honest), and **Exit gate** (the CI condition that says "stage done").

### Stage 0 — Scaffolding
**Deliverable:** Repo skeleton per PRD §10 (now including the `protocol/` directory placeholder), but only the directories that will be filled in stage 1. `core/thermal_config.h` with compile-time maxima from PRD §4.2. `test/unit/harness.h` with the assertion macros. `.github/workflows/ci.yml` running an empty test binary that prints "no tests yet" and exits 0. `Makefile` at `platform/linux/Makefile` and a top-level `Makefile` that delegates to `test/`.

Stage 0 also delivers the **tool-version pinning** that later stages depend on:
- `requirements-dev.txt` — Python deps for reference math, scenario runner, and `thermalcore-probe` (numpy/scipy/pyyaml/python-can/jsmin/etc., each pinned).
- `ci/tool-versions.md` — version-of-record for clang-tidy, cppcheck, lcov, libFuzzer/clang, ESP-IDF docker image tag. Pinned by docker image SHA where possible; otherwise by version string.
- CI installs from these files before running any job; bumping a tool is a deliberate PR.

**Tests added:**
- One trivial unit test (`TEST_CASE(harness_works) { EXPECT_EQ(1, 1); }`) to prove the harness compiles and links.
- **No-heap / no-syscall guard:** two complementary mechanisms, both gating from Stage 0.
  - **Static (authoritative):** `nm -u core/libthermal_core.a | grep -Ef ci/core-symbol-denylist.txt` must produce no output. The denylist contains `malloc`, `calloc`, `realloc`, `free`, `open`, `read`, `write`, `mmap`, `munmap`, `sleep`, `usleep`, `nanosleep`, `printf`, `fopen`, `pthread_*`, etc. This is the canonical check — it answers "did `core/` reach for any of these symbols?" without depending on call-graph attribution.
  - **Runtime defense:** a separate *core-only* unit-test binary (`test/unit/core_only_runner`) links **only** `core/` objects plus the harness, with `-Wl,--wrap=malloc,--wrap=...` wrappers that `abort()` on entry. Because the binary contains no platform/test code, any wrapped call is necessarily a core call — no caller-attribution magic required.
  - Both run on every PR. The static check catches dead-code or conditional paths the runtime test doesn't exercise; the runtime check catches dynamic-dispatch cases the static check might miss. Together they pin the PRD §4.2 portability promise.

**Heads-up.** If hand-rolled C99 test harnesses are unfamiliar territory, debug `harness.h` against this trivial assertion before opening Stage 1 — `EXPECT_EQ` macro behavior, ctest registration, and the build wire-up are easier to diagnose against a one-line test than against a failing real test. Better half a day here than half a day mid-Stage 5.

**Regression value:** Establishes the CI gate exists. Any future PR that breaks the build is blocked from merging. The no-heap/no-syscall guard activates from PR #1 onward, so a stray `malloc` introduced at Stage 5 fails CI immediately rather than at MCU port time.

**Exit gate:** `build-linux`, `unit`, and `no-heap-no-syscall` all green on `main`.

---

### Stage 1 — Core types compile
**Deliverable:** All public C types from PRD §4.3 declared as headers under `core/`: `thermal_types.h`, `thermal_signals.h`, `thermal_events.h`, `thermal_commands.h`. No implementation yet — just structs, enums, and function prototypes. `core/thermal_core.h` declares the v1 public API with the bodies still empty (`return THERMAL_ERR_UNAVAILABLE;`).

**Tests added:** A compile-time test that includes every public header and references each typedef once. Verifies size budgets against named constants in the test (e.g., `THERMAL_TEST_STATE_SNAPSHOT_BUDGET_BYTES`, `THERMAL_TEST_CONFIG_BUDGET_BYTES`) so thresholds are adjusted deliberately, not in passing. Initial values track the actual `sizeof` at v1 defaults plus a documented headroom.

**Regression value:** Pins the public ABI for tools and prevents accidental size blowups when fields are added later.

**Exit gate:** Everything compiles under `-Werror -pedantic`; the size-budget test passes.

---

### Stage 2 — Curve interpolation (first real testable function)
**Deliverable:** `core/thermal_curve.c` implementing the integer linear interpolation from PRD §4.8. Two functions: `thermal_curve_eval_y0(curve, x)` and `thermal_curve_eval_y1(curve, x)` (the acoustic_mask curve has two outputs).

**Tests added:**
- **Unit:** monotonicity, endpoint clamping, mid-point interpolation, two-point degenerate case, single-segment, eight-segment, exact-on-x-knot behavior. Twenty or so cases.
- **Golden replay (first one):** a "sweep speed from 0 to 200 km/h in 1 km/h steps" CSV; output CSV captured as `test/replay/golden/curve_sweep.csv`. Adds the `make regen-replay-goldens` helper and the `replay` CI job.
- **Reference cross-check:** a pure-integer Python reference (`test/reference/curve.py`) implements the PRD §4.8 formula in Python without numpy — same integer arithmetic, same C99 truncation. The C output must match the Python output exactly (byte-equal). Pure-integer reference avoids float rounding entirely and catches drift from the documented formula even when goldens are re-baselined.

**Regression value:** Any future change to interpolation math, integer-division rounding, or endpoint clamping breaks the golden diff and the test must be re-baselined consciously. Locks down the formula from PRD §4.8.

**CI rigor added:** `replay` job.

**Exit gate:** unit + replay green.

---

### Stage 3 — IIR filter + sensor pipeline
**Deliverable:** `core/thermal_filter.c` (Q16.16 IIR), and the sensor-side of the input-snapshot processing in `core/thermal_core.c`. Builds the bridge between `thermal_input_snapshot_t.samples[]` and per-sensor filtered values.

**IIR formula and implementation form (pinned in PRD §4.5 + decision 32):**

```
filtered_next = filtered_prev + alpha_q16 * (sample - filtered_prev)        // math form
filtered_next = prev + (int32_t)(((int64_t)alpha_q16 * (sample - prev)) >> 16)   // implementation form, saturating
```

With this convention:
- `alpha_q16 = 0` holds the previous value (no update).
- `alpha_q16 = Q16_ONE` passes the new sample through unchanged.
- Intermediate values produce a first-order low-pass; smaller `alpha_q16` is heavier filtering.

The multiplication is performed in `int64_t`; the shift back is by 16; the final cast to `int32_t` saturates. Stage 3 just implements this form; test expectations, goldens, and Python reference all derive from the PRD formula.

**Filter validity lifecycle (also pinned in PRD §4.5 + decision 33):**

- Pre-init: `valid = 0`, `filtered_value` undefined.
- First valid sample: filter initializes `filtered_value = sample` directly (no IIR step from zero), sets `valid = 1`.
- Subsequent valid samples: IIR equation advances `filtered_value`; `valid` stays 1.
- Invalid samples: no arithmetic update; `filtered_value` held; `valid` set to 0.
- Aggregation skips sensors with filter `valid = 0`.
- Context signals respect the fail-safe mode for whether a held value remains policy-active.

**Tests added:**
- **Unit:** step response, impulse response, `alpha = 0` (no update — output holds previous), `alpha = Q16_ONE` (passthrough — output equals input), intermediate alphas converging to the input asymptotically, Q16.16 saturation under extreme values.
- **Lifecycle unit tests:** pre-init read of `filtered_value` is forbidden (asserted via `valid` flag); first-valid-sample initializes filter directly to sample; sequence of valid → invalid → invalid → valid preserves held value but resets `valid` correctly; aggregation skips a filter with `valid = 0` regardless of its held numeric value; context `hold_last` mode keeps a held value policy-active while `assume_stationary` substitutes the failsafe.
- **Module golden:** a noisy-sensor CSV (1000 samples with simulated jitter, including injected invalid runs) → filtered CSV golden. Captures both numeric trajectory and validity stream.
- **Reference cross-check:** a small integer-only Python reference (`test/reference/iir.py`) computes the same outputs on the same input using Q16.16 arithmetic; the C output must match exactly. Pure-integer reference avoids float rounding entirely; no scipy dependency needed for this stage.

**Regression value:** Filter math is small but easy to break with off-by-one shifts or accidental int promotion. Module golden catches that immediately; the integer reference catches drift the golden would happily re-baseline.

**Exit gate:** unit + replay green.

---

### Stage 4 — Zone aggregation + step-wise governor
**Deliverable:** `core/thermal_zone.c` (sensor aggregation: max, avg, weighted), `core/thermal_governor.c` (step-wise governor with hysteresis), trip-point evaluation. Active-trip-mask computation. PRD §4.7 partial-validity rules.

**Tests added:**
- **Unit:** aggregation modes including partial-invalid sensors, weighted with edge weights (one weight zero, one weight dominant), trip enter/exit with hysteresis, multiple-active-trip "highest state wins" logic.
- **Module golden:** a heat-soak ramp CSV (45°C → 90°C → 45°C over 600 ticks) through a multi-trip zone; zone-state output captured to golden.
- **Property (new layer, configs only):** generated configs with 1–8 sensors, randomized trip points, run through `thermal_core_validate_config()`; must never crash or return undocumented status. Random *commands* through `apply_command()` come later, at Stage 8, since `apply_command()` doesn't exist yet.

**CI rigor added:** `property-config` job.

**Regression value:** The step-wise governor is the simplest reference behavior; once locked, it's the cross-check for PID and modifier work that follows.

**Exit gate:** unit + replay + property green.

---

### Stage 5 — PID governor (Q16.16)
**Deliverable:** `core/thermal_pid.c`. Anti-windup per PRD §4.8. dt clamping. Derivative on measurement, optional first-order filter. Saturation telemetry on overflow.

**Tests added:**
- **Unit:** step response, settling time, anti-windup under sustained saturation, `dt_min_ms`/`dt_max_ms` clamp on missing or extremely late tick, non-monotonic `now_ms` jump (per PRD §4.8 — should clamp via dt, not crash; diagnostic event emitted), gain-change resets integral+derivative (per `CMD_SET_PID` contract), PID-trip-floor interaction (critical trip floors output via `state_pwm[cooling_state]`).
- **Module golden:** step-load CSV (50°C → 85°C step), full PID-term telemetry captured. Becomes the golden for tuning regressions.
- **Reference cross-check:** a pure-integer Q16.16 Python reference (`test/reference/pid.py`) computes the same outputs; the C output must match the Python output exactly. No float, no scipy — same arithmetic in both. An *optional* scipy-based float-reference comparison runs in nightly only, with a documented tolerance, to confirm the Q16.16 design is close enough to the textbook PID it implements.

**Regression value:** PID is the most-tuned single module. Module golden + integer reference catches math regressions immediately; the optional nightly scipy check sanity-checks the Q16.16 design itself.

**Exit gate:** unit + replay + property green.

---

### Stage 6 — Fault detectors (all four)
**Deliverable:** `core/thermal_fault.c` — stall, stuck-sensor, runaway, stale-context detectors. State machines per PRD §4.7. `THERMAL_FAULT_ACTION_*` handlers. Spin-up grace window. Latching only; the typed `CMD_CLEAR_FAULT` API lands in Stage 8 (`thermal_core_apply_command`); its wire transport lands in Stage 10. Stage 6 implements the state machine and the "is clear allowed yet?" check; later stages plug in the command paths.

**Tests added:**
- **Unit per detector:** entry conditions, persist_ticks behavior, recovery_ticks behavior, LATCHED requires explicit clear, stuck-sensor advisory mode without correlated context.
- **Runaway formula (per PRD §4.7):** active when `zone_temp_mc[N] - zone_temp_mc[M] >= rise_mc_threshold` with `M = N - persist_ticks`, AND `min(commanded_pwm[M..N]) >= cooling_pwm_threshold` for the affected actuator set. Test cases: rising temp under high PWM (fires), rising temp under low PWM (doesn't fire), flat temp under high PWM (doesn't fire), oscillating PWM that dips below threshold during the window (doesn't fire because of `min`).
- **Module golden:** each fault scenario as a CSV input + golden output: stall raise + recover, stuck sensor with correlated load, runaway under high-PWM rising-temp window.
- **Regression hook:** the stall scenario includes the spin-up window — catches future regressions where someone "fixes" stall logic and breaks spin-up grace.

**CI rigor added:** ASan/UBSan join CI here. From this stage on, sanitizer-clean is required for merge.

**Exit gate:** unit + replay + property + asan green.

---

### Stage 7 — Acoustic modifier + arbitration + slew + end-to-end step
**Deliverable:** `core/thermal_modifier.c` (acoustic_mask, pre + post stages), `core/thermal_arbitrator.c` (max-wins), slew-rate limiter, `core/thermal_core.c` wiring per the control-loop diagram in PRD §4.6. `thermal_core_step()` is now a complete end-to-end function.

**Tests added:**
- **Unit:** modifier pre-stage trip offset, modifier post-stage pwm-cap, modifier active-flag semantics, modifier output-domain clamping (interpolated `pwm_cap` clipped into `0..255` after curve eval, per PRD §4.3), stale-context fail_safe behavior, arbitrator under three-zone-one-actuator load.
- **Safety-override unit tests (explicit):** critical-severity trips bypass acoustic caps for affected actuators; shutdown-severity trips bypass acoustic caps **and** request max PWM **and** emit `TEVENT_SHUTDOWN_REQUEST`; slew limiter bypassed for upward safety overrides but obeyed for downward/recovery transitions. Each covered as its own named test case so a future refactor can't accidentally lose one.
- **Full-step golden (first one):** the full closed loop — a 60-second `thermal_input_snapshot_t` stream driving every module in `thermal_core_step()`. Output frame + telemetry captured. This is the canonical "did anything change anywhere in the loop" canary.

**CI rigor added:** clang-tidy on `core/` only.

**Exit gate:** unit + replay + property + asan + clang-tidy green.

---

### Stage 8 — Runtime command + state inspection
**Deliverable:** `thermal_core_apply_command()` for all five v1 commands (`SET_PID`, `SET_SETPOINT`, `SET_TRIP`, `SET_CURVE_POINT`, `CLEAR_FAULT`). `thermal_core_get_state()`. PID-integrator reset on gain change. Curve-edit monotonicity check per PRD §5.3.

**Scope boundary:** Stage 8 tests the *typed* `thermal_command_t` API only. The wire encoder/decoder, CRC handling, and frame-level fuzz live in Stage 10. This keeps the boundary clean — semantic command validation here, frame integrity there.

**Tests added:**
- **Unit:** each command, valid + each error path; bounds enforcement; monotonicity rejection on curve edit; LATCHED clear gating; `TEVENT_COMMAND_APPLIED` / `_REJECTED` emission with the right `now_ms`.
- **`INVALID_ARG` vs `BOUNDS` split (per PRD §7.5):** unknown `command_id` → `INVALID_ARG`; unknown target ID → `INVALID_ARG`; `trip_idx >= trip_count` → `BOUNDS`; `point_idx >= curve_count` → `BOUNDS`; `kp > kp_max` → `BOUNDS`; curve edit breaking monotonicity → `BOUNDS`. Every documented path covered.
- **Full-step golden:** a "step response with mid-experiment kp change" sequence. Captures both the gain-change ack event and the resulting PWM response.
- **Property-command (new):** randomly generated *typed* `thermal_command_t` values (any command_id, any payload value) through `apply_command()` — must always return a documented status, never crash, never invoke a callback with malformed state. Wire-byte fuzzing into the codec is *not* here — that's Stage 10 once `protocol/` exists.

**CI rigor added:** cppcheck (extends to `platform/linux/` at Stage 9); `property-command` job extending property testing to the typed command API.

**Exit gate:** unit + replay + property + asan + clang-tidy + cppcheck green.

---

### Stage 9 — Linux daemon: `bsp_mock_tmpfs` + JSON loader + `json2static.py` + telemetry UDP + probe + deterministic clock
**Deliverable:** `platform/linux/thermalcored.c`, `bsp_mock_tmpfs.c`, `bsp_telemetry_udp.c`, `config_jsmn.c`, **`tools/json2static.py`**. The daemon boots from a JSON file, polls a tmpfs hwmon mock, runs `thermal_core_step()` every 100 ms, emits telemetry UDP frames. `tools/thermalcore-probe` (Python) reads UDP and writes CSV. `json2static.py` lands here (not at ESP32 bring-up) so the loader-to-C-struct mapping is round-trip tested at the same stage it's written.

**Deterministic mode (required deliverable, per §1 principle 8):**
- `thermalcored --clock=wall` (default) uses real time.
- `thermalcored --clock=scenario --scenario-clock-uri=<uri>` reads ticks from a **dedicated scenario clock channel** — a Unix-domain socket or pipe owned by the scenario runner, not the UDP command listener. Default URI: `unix:/tmp/thermalcored-clock.sock`. The runner sends `TICK now_ms` messages; the daemon advances internal time only on receipt. No wall-clock sleeps. Keeping this channel separate from the Stage 10 UDP control plane means runtime commands and scenario ticks can never be confused at the transport layer.
- The control loop has a fixed order per tick: (1) drain queued commands and apply them with the tick's `now_ms`; (2) build the input snapshot from current mock-tmpfs state; (3) call `thermal_core_step()`; (4) emit telemetry frames; (5) write actuator outputs to the mock.
- `TEVENT_COMMAND_APPLIED` events emitted during step (1) use that tick's `now_ms`.
- Stage 9 lands deterministic mode even if Stage 12 is what depends on it — pulling this work into Stage 9 catches scheduler-related determinism bugs early instead of mid-scenario-test debugging.

**Canonical config hashing (per PRD §7.6, decision 7):**
- The hash function is a **field-by-field canonical encoder** in `tools/config_hash.py` (Python) and a matching C function in `support/thermal_config_hash.c`. **It lives in `support/`, not `core/`,** because SHA-256 + canonical encoding is a reproducibility concern, not control-loop policy — embedded targets that don't care about hashing don't link it. (Per PRD §10 layout: `support/` is portable C99 with no platform/heap deps, depends only on `core/` types.)
- Encoding rules: little-endian fixed-width integers for every numeric field; unused trailing array slots zero-filled to the compile-time maximum; strings null-terminated and padded to `THERMAL_NAME_MAX` with zeros. Result hashed with SHA-256. Never a `memcpy(&cfg, sizeof cfg)` over raw struct bytes.
- A deliberate **padding-poison test** in Stage 9 fills `thermal_config_t` struct padding with `0xAA` between two otherwise-identical configs, hashes both, and asserts equality. Catches any future regression that introduces a `memcpy`-based hash.

**Tests added:**
- **Smoke (new test type):** daemon starts with `configs/minimal-1zone-1fan.json` in `--clock=scenario` mode, runs 100 ticks driven by the scenario channel, exits cleanly on SIGTERM, produced at least one telemetry frame on UDP per tick.
- **JSON loader unit tests:** valid configs accepted; each documented invalid case rejected with the right status code and a clear error message.
- **Platform-only vs core config separation:** the JSON loader splits fields into `thermal_config_t` (deterministic policy) and `thermalcored_runtime_cfg_t` (platform-only — `source`, `pwm_freq_hz`, `tach_pulses_per_rev`, `telemetry.transport`, `telemetry.signals`, `control.listen`). A unit test loads the reference config and asserts each field landed on the right side of that line.
- **`json2static.py` round-trip:** parse `configs/minimal-1zone-1fan.json` → emit static `const thermal_config_t` C file → compile it into a tiny test binary → call `thermal_core_validate_config()` on the static config → assert `THERMAL_OK` and that the canonical config hash matches the hash computed by the JSON loader. Proves the two config paths reach the same in-memory representation.
- **Padding-poison test:** as described above.
- **Fuzz (new test type):** libFuzzer over the JSON loader, seed corpus = `configs/*.json`. 60 s/PR.
- **Daemon-level full-step replay:** in `--clock=scenario` mode, write a known sequence of temp values to the tmpfs mock at scripted tick boundaries, capture the telemetry UDP stream, diff against golden. Boundary cases include commands arriving exactly on a tick edge.
- **Probe parity:** the same UDP stream replayed through `thermalcore-probe --log` produces the same CSV every time.

**CI rigor added:** smoke-linux, coverage (visibility only, no gate), fuzz-json. `clang-tidy` and `cppcheck` extend their scope to `platform/linux/` starting this stage.

**Stage 9 actual deliverables vs deferrals (post codex-v4 review):**

- **Shipped 9a–9d:** JSON loader (`platform/linux/config_jsmn.c` backed by vendored `jsmn`), runtime cfg sibling struct (`platform/linux/runtime_cfg.h`), `bsp_mock_tmpfs` for sensor/actuator file I/O, `thermalcored` daemon with wall + scenario clocks, syslog-based `log_event`, UDP telemetry via an in-daemon `telem_wire.c` encoder, canonical SHA-256 config hash in `support/`, padding-poison test, `tools/json2static.py`, libFuzzer `fuzz-json` job, and a Python smoke harness. PRD §5.1 `fault_detection` schema (descriptive thresholds + `correlated_context: null` advisory mode) landed in the codex-v4 carryover commit.
- **Deferred to Stage 10:** `platform/linux/bsp_telemetry_udp.c` and the wire-encoder hoist into `protocol/thermal_wire.c` (with CRC, sequence numbers, and the opcode registry). Stage 9 ships the encoder inline as `platform/linux/telem_wire.c`, explicitly labelled a stopgap; the daemon emits observable frames today, but the encoder is the only place that knows the v1 layout. Stage 10 replaces it.
- **Deferred to Stage 10 / 11:** `tools/thermalcore-probe`. The smoke test currently decodes the stopgap frame directly; once the probe lands the smoke harness will consume the same decoder as the user-facing tool.
- **Deferred to Stage 12:** `tools/config_hash.py` (Python-side canonical encoder). Lands when scenario-determinism actually needs cross-language hash parity. The C hash is verified against itself today by the `json2static` round-trip test.
- **Smoke scope:** the current harness drives five ticks against `test/smoke/smoke-config.json` (a tmpfs-pathed copy of the reference config, so it runs as a normal user) and asserts ≥ 3 telemetry frames plus exit code 0. The originally-planned per-tick frame audit + reference-config use will land alongside the probe so the smoke decoder is shared with `thermalcore-probe`.
- **Stuck-sensor `correlated_load_changing` simplification:** the v1 full-loop step in `core/thermal_core.c` (step 9, comments around `cfg->faults.stuck_sensor_defaults`) treats any valid configured context as "load changing". Proper delta-over-window tracking is future work tracked alongside scenario-injection support; the detector itself takes a correct boolean input, and advisory mode (PRD §4.7 line 632) is now expressible via `"correlated_context": null` thanks to the codex-v4 fix.

**Exit gate:** unit + replay + property + asan + clang-tidy + cppcheck + smoke + fuzz-json green.

---

### Stage 10 — Control plane: command listener + `thermalcore-tune` + `protocol/` wire codec
**Deliverable:** UDP command listener on the daemon (`127.0.0.1:9002`), the wire encode/decode helpers in `protocol/thermal_wire.c` (CRC-16/CCITT-FALSE, packed encoding per PRD §7.2 — **lives in `protocol/`, not `core/`,** per PRD decision 34), `tools/thermalcore-tune` CLI.

The `protocol/` module is portable C99 with no heap and no platform deps. It links to `core/` types but `core/` does not depend on `protocol/`. The daemon, ESP32 firmware, and tools all link `core/` + `protocol/`. A hypothetical embedder that uses `core/` via direct C calls does not need to link `protocol/`.

**Single source of truth for command IDs.** `thermal_command_id_t` in `core/thermal_commands.h` is the only place that defines numeric command IDs. `protocol/thermal_wire_opcodes.h` covers frame opcodes (`TELEM_SAMPLE`, `TELEM_EVENT`, `CMD_REQUEST`, `CMD_ACK`, `CMD_NACK`), transport status codes (above 0x8000), and per-platform receive caps — **not** command IDs. The header includes `core/thermal_commands.h` and adds `_Static_assert` checks (e.g., `_Static_assert(THERMAL_CMD_SET_PID == 0x0001, "wire encoder expects SET_PID = 0x0001")`) so wire encoders that hardcoded a number would fail the build.

**Tests added:**
- **Unit:** wire encoder/decoder round-trip for every opcode + command_id; CRC validation; sequence-number tracking; payload-cap rejection. All in `test/unit/protocol/`, exercising `protocol/` alone.
- **Sequence-number wraparound:** generate 65 537 commands so `seq` wraps modulo 65536; verify outstanding-window matching still correctly pairs `CMD_ACK` to the right `CMD_REQUEST` and that expired outstanding entries are evicted by timeout rather than wraparound. PRD §7.2 defines ACK matching by `(transport, seq)` — wraparound regression would be silent without this test.
- **Daemon-level:** `thermalcore-tune set-pid soc 5000 400 0` produces a `CMD_ACK` with the request seq, and the next telemetry frames carrying `TSIG_PID_*` reflect the new gains. (v1 has no `CMD_GET_STATE` wire command — verification flows through the ACK + the existing telemetry stream, not a synchronous state pull.)
- **Timestamp propagation:** `thermalcore-tune set-setpoint soc 75000` at host wall time `t` produces a `TEVENT_COMMAND_APPLIED` whose `ts_ms` equals the `now_ms` the daemon passed to `thermal_core_apply_command()`. Catches regressions where command-apply events accidentally carry a stale or zero timestamp.
- **Fuzz:** libFuzzer over the `protocol/` wire decoder, seed corpus = recorded valid frames.

**CI rigor added:** fuzz-wire (against `protocol/`, not `core/`).

**Stage 10 actual deliverables vs deferrals (post 10d + v7 carryover):**

- **Shipped 10a–10d:** `protocol/thermal_wire.{c,h}` + opcodes (10a); daemon hoist onto the canonical codec + UDP control listener (10b); `tools/thermalcore-tune` + Python wire module + 5-subcommand integration test (10c); libFuzzer harness over the decoder, u16-seq-boundary unit-test scenario, smoke-side timestamp-propagation assertion (10d).
- **Shipped in the codex-v7 carryover commit:** PRD §7.2 frame shape (trailing `u16 crc16` always on the wire; `0x0000` for CRC-disabled) across C, Python, and the no-CRC unit test; transport status codes `≥ 0x8000` (`THERMAL_WIRE_STATUS_BAD_PAYLOAD` / `BAD_OPCODE` / `OVER_CAP` / `BAD_VERSION`) with daemon NACKs on ackable transport errors; `thermalcore-tune --config <path>` name resolution (zone / modifier / sensor / actuator / context); PID-positive tuning integration test (`set-pid soc 0 0 0` → ACK + integral reset + zero output); reference config switched to `udp:127.0.0.1:9002` + `control.enable` schema (default off; invalid non-empty URI is a startup error).
- **Wire-codec u16 seq boundary** is covered by `test_thermal_wire` scenario 18 (round-trip at seq=65535 and seq=0 post-wrap).  The originally-planned **full async outstanding-window matching test** (65,537 commands, host-side ACK-pairing under wraparound) needs an asynchronous client that v1 doesn't have — `thermalcore-tune` is one-shot synchronous.  Lands when the Stage 12 scenario runner orchestrates timed commands.
- **`thermalcore-probe`** (PRD §7.4) — deferred to Stage 11/12; smoke harness keeps decoding TC frames in-place until then.

**Exit gate:** all previous + fuzz-wire green.

---

### Stage 11 — SocketCAN + OBD-II + `car-can-emulator` integration
**Deliverable:** `bsp_socketcan.c`, OBD-II PID 0x0D decode. `car-can-emulator` integrated as a git submodule and built by CI on `vcan0`. The acoustic_mask modifier is now driven by real CAN traffic in the test rig.

**Setup details to pin in this stage:**
- `.gitmodules` pins `tools/car-can-emulator` to a specific commit SHA on the `v2-improvements` branch (per PRD decision 3). Submodule update is a deliberate PR, not a floating dependency.
- CI provisions `vcan0`: a setup step runs `sudo modprobe vcan` and `sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0` on the GitHub Actions Linux runner. The standard `ubuntu-latest` image grants this. On forks or local CI without `CAP_NET_ADMIN` the integration job degrades to a clearly-logged skip; **the canonical repository CI used to complete Stage 11 must run the integration test on a capable runner** — a skip on the canonical repo blocks the Stage 11 exit gate.
- The emulator's TCP control port is used *only* by scenario and integration tooling (`tools/thermalcore-scenario`, this stage's integration test). The core and the daemon never open the TCP port — they consume CAN frames only, per PRD §6.4.

**Tests added:**
- **Unit:** OBD-II frame encode/decode, response timeout handling, fail_safe fallback after `timeout_ms`.
- **Integration:** CI starts `car-can-emulator` on `vcan0`, starts `thermalcored`, sets speed via the emulator's TCP control port at `t=0, 5, 10, 15 s`, asserts the daemon's telemetry stream shows the expected speed values within the configured filter time constant.
- **Module golden:** a CAN bus-loss scenario (emulator killed mid-run) producing `assume_stationary` fallback.

**Stage 11 actual deliverables (in progress):**

- **Shipped 11a:** `protocol/obd2.{c,h}` — portable C99 codec for OBD-II Service 01 single-byte PIDs.  Two entry points (`obd2_encode_request_byte`, `obd2_decode_response_byte`); 11 unit scenarios cover the request byte pattern, three response values (0 / 120 / 255 km/h), and five distinct failure modes (`BAD_SHAPE`, `WRONG_PCI_LEN`, `NEGATIVE_RESPONSE`, `WRONG_SERVICE`, `WRONG_PID`).  No socket I/O, no submodule — that lands in 11b / 11c.
- **Shipped 11b:** `platform/linux/bsp_socketcan.{c,h}` — wraps the 11a codec around `AF_CAN/SOCK_RAW` plumbing on a configurable interface.  Daemon scans `runtime.contexts[]` at startup for a source beginning with `"canbus:"` and opens the matching SocketCAN socket; the tick loop polls at 1 Hz (PRD §6.2) and dispatches snapshot context reads per-slot (`canbus:` → bsp_socketcan, else → bsp_mock_tmpfs).  PRD §6.3 fail-safe falls out for free: stale CAN data flips the sample's `valid` bit to 0, which `acoustic_mask` consumes as `assume_stationary`.  7 unit scenarios drive the state-pure handler with synthetic frames (valid 120 km/h, wrong CAN ID, NRC, cold-start staleness, post-timeout staleness, wrong-PID, truncated frame).  No vcan0 integration test in CI yet — that lands in 11c with the `car-can-emulator` submodule.
- **Shipped 11c:** `tools/car-can-emulator` submodule (pinned to `e62fc7d` on `v2-improvements` per PRD §6.1 line 825).  New `test/integration/test_canbus_obd2.py` drives three vehicle-speed setpoints (100 / 50 / 200 km/h, 8 s hold each) via the emulator's TCP port 8080 and asserts `TSIG_CONTEXT_VALUE_0` telemetry converges to each value within ±15 km/h.  New `make integration-can` PHONY target skips cleanly when `vcan0` is unavailable (so the existing local-dev gates stay green); the new `integration-can` CI job on `ubuntu-latest` provisions `vcan0` via `modprobe vcan` + `ip link add` and runs the hardware loop for real.  A SKIP on the canonical-repo CI blocks the Stage 11 exit gate.  No bus-loss module golden yet — that's 11d.

**Exit gate:** all previous + CAN integration test green.

---

### Stage 12 — Scenario runner + thermal-plant simulator + canonical scenarios
**Deliverable:** Two artifacts with a clean split:
- **Plant math (C)**: `tools/thermalcore-scenario/plant.c` + `plant.h`, pure Q16.16, no Python. Linked into the scenario runner CLI, `test/replay/`, and `test/unit/` (per §8.5). Single source of truth for the simulator.
- **Scenario runner (Python)**: `tools/thermalcore-scenario/run.py`, orchestrates `.scn` file parsing, drives the C plant through a small CFFI/ctypes binding, talks to `thermalcored` over UDP, evaluates assertions, writes telemetry CSVs.

There is **no duplicate Python plant model**. If the Python orchestrator ever needs a plant value, it calls into the C library. This guarantees that scenario runs and unit/replay tests share bit-for-bit-identical plant arithmetic.

**Scenario-to-daemon I/O paths (pinned):** scenarios run `thermalcored --clock=scenario` (delivered in Stage 9). The orchestrator drives the daemon through:

- **Tick clock channel:** the orchestrator sends `TICK now_ms` messages on a local socket; the daemon advances `now_ms` only on receipt. No wall-clock sleeps inside the daemon during scenarios.
- **Sensor inputs:** the plant simulator advances temperature evolution in C; the orchestrator writes simulated values into the `bsp_mock_tmpfs` tree at each tick before the `TICK` advance.
- **Actuator outputs:** the daemon writes PWM commands to the same tmpfs tree; the orchestrator reads them after each tick and feeds them back into the plant as the cooling input for the next step.
- **Commands:** sent over the existing UDP control channel (`127.0.0.1:9002`), drained at the start of the next tick by the deterministic loop order in Stage 9.
- **Telemetry:** captured from the existing UDP telemetry channel (`127.0.0.1:9001`) by `thermalcore-probe --log`; the SHA-256 over the resulting CSV is the determinism gate.

All ten canonical scenarios from PRD §9.1 implemented. Assertion grammar per PRD §7.6.

**Tests added:**
- **Scenario (new test type):** every canonical `.scn` file runs in CI, assertions decide pass/fail.
- **Determinism (new test type):** every scenario runs twice; SHA-256 of the telemetry CSV must match. Then the gcc-built daemon and the clang-built daemon must produce identical SHA-256.
- **Simulator unit tests:** plant math (heat in, cooling gain, coupling), deterministic PRNG, zone-to-zone coupling.

**CI rigor added:** scenario, determinism.

**Exit gate:** all previous + scenario + determinism green. **This is the v1 Linux release-gate.**

---

### Stage 13 — ESP32 STANDALONE build
**Deliverable:** `platform/esp32_idf/main/*` with thin BSP wrappers around LEDC, PCNT, 1-Wire, I2C, TWAI. `app_main()` ≤ 100 lines. The static `thermal_config_t` is generated by `tools/json2static.py` (which landed in Stage 9 and was round-trip tested there) from the same JSON the Linux daemon uses. ESP32 standalone build runs the full thermal-core in a FreeRTOS task.

**Target detail:** the reference target is **ESP32-C6**, which is **RISC-V** (not Xtensa). The build matrix targets `esp32c6` only in v1; ESP32-S3 / ESP32-WROOM (Xtensa) are not v1 targets.

**Build modes:** the firmware supports three build-time modes selected by `-DTHERMALCORE_MODE=…`:
- `STANDALONE` — full thermal-core running on the bench with real sensors/tach/CAN. Used for white-paper portability demonstrations.
- `HIL_PERIPHERAL` — peripheral concentrator (introduced fully in Stage 14).
- `REPLAY_STANDALONE` — **new in Stage 13.** Real sensors and CAN are stubbed out; the firmware reads a synthetic `thermal_input_snapshot_t` stream from a fixture (flashed into a `.rodata` section, or fed over USB-CDC by the host harness) and emits canonical telemetry over USB-CDC. This is the mode Stage 15 runs to compare host-vs-target byte-for-byte. Keeps deterministic replay parity orthogonal to the physical sensor path.

**Tests added:**
- **Build (required, PR-gating):** `idf.py -DIDF_TARGET=esp32c6 build` succeeds in CI under the ESP-IDF docker image. Size-budget assertions: `.text <= 64 KB`, `.bss <= 16 KB` (per PRD §9.2).
- **Cross-platform unit replay (target-instruction, optional):** the same `test/unit/` binary cross-compiled for `qemu-system-riscv32` (with the ESP32-C6 emulation target, when stable in ESP-IDF) runs replay tests; output CSVs must match the host build byte-for-byte. If a stable C6 QEMU path is not yet available, this step is a **nightly job, not a PR gate**, and uses real hardware via a self-hosted runner instead.
- **Manual:** ESP32-C6 flashed on the bench, runs `idle_steady_state` scenario, observed via probe over USB-CDC. Not in PR-gating CI.

**CI rigor added:** build-esp32 (required); target-replay (nightly, not gating).

**Exit gate:** all previous + build-esp32 green. Target-replay parity is enforced in nightly, not as a PR gate.

---

### Stage 14 — ESP32 HIL_PERIPHERAL build
**Deliverable:** Same firmware compiled in HIL mode (per PRD §8.3): ESP32 reports tach/temp/CAN to Linux over USB-CDC as `TELEM_SAMPLE` frames; Linux runs the thermal-core; Linux sends actuator commands back as `CMD_REQUEST` frames with platform-private command IDs.

**Tests added:**
- **Build:** HIL-mode firmware builds with size budgets met. PR-gating.
- **Integration (manual / nightly self-hosted, not PR-gating):** ESP32 + Linux on bench. Same canonical scenarios run, same assertions. HIL telemetry is compared against the standalone telemetry using **behavioral tolerance bands** (settling time within ±10%, max overshoot within ±5%, fault latency within the documented latency envelope), not SHA equality — physical tach jitter, USB-CDC latency, and CAN frame timing make byte-equal SHA infeasible. See Stage 15 for how the two flavors of parity are defined.

**Exit gate:** HIL build green; bench integration run succeeds when executed (manual or nightly).

---

### Stage 15 — Cross-platform parity + benchmarks
**Two flavors of parity, deliberately separated:**

1. **Deterministic replay parity — conditional PR gate.** Host build and ESP32-C6 standalone build are fed *identical synthetic input streams* (no real sensors, no real CAN — just `thermal_input_snapshot_t` arrays played from a fixture file). Both rigs are deterministic; the SHA-256 of the resulting telemetry CSV must be byte-equal.

**Infrastructure precondition (be explicit about what we're promising):**
- If a stable ESP32-C6 RISC-V QEMU path exists in ESP-IDF *or* a reliable self-hosted ESP32-C6 runner is connected to the repo, `replay-parity` is a per-PR / merge-queue gate.
- If neither exists, Stage 15 cannot claim PR-gated replay parity. The gate becomes a **release-tag gate** (must pass before a `v*` tag is cut) and a nightly job; PR merges are not blocked by it. The plan does not promise an always-on PR gate before the runner strategy is real.
- The repo `README.md` records which mode is currently active.

**Telemetry SHA composition (pinned).** The SHA-256 covers a fixed canonical projection of the telemetry stream — narrow enough to be deterministic, wide enough that real behavior changes show up. One row per callback, columns:

```
ts_ms, row_type, id, value, flags_or_status, a1, a2, a3, a4
```

- `row_type` ∈ {`S` for sample, `E` for event} — included so sample and event rows are never collapsed.
- For `S` rows: `id` = `signal_id`, `value` from `telemetry_emit()`, `flags_or_status` = `flags`, `a1..a4` = 0.
- For `E` rows: `id` = `event_code`, `value` = 0, `flags_or_status` = 0, `a1..a4` from `log_event()`. Catches event-arg changes (detail codes, target IDs) that earlier drafts of this contract would have hashed past silently.
- Wall-clock-derived columns (host receive-time, queue depth) and non-deterministic diagnostics (uptime, free heap) are excluded.

**Intra-tick emission order (also pinned).** The core emits all callbacks for one tick in a deterministic order, so two rigs that compute the same values produce them in the same row order:

1. Zone state, by zone slot (0..N-1).
2. Actuator state, by actuator slot.
3. Context state, by context slot.
4. Modifier outputs, by modifier slot.
5. Fault detector telemetry, by detector slot.
6. Events generated this tick, in core's deterministic generation order (fault enter/leave before command-applied before shutdown-request).

The canonical column projection and the row-ordering contract live in `tools/thermalcore-probe/canonical.py` and a matching C-side serializer; both Stage 12 (host determinism) and Stage 15 (host-vs-target parity) hash from the same projection.

2. **Physical HIL tolerance (nightly / on-demand, never PR-gating).** All ten canonical scenarios on real hardware (ESP32 standalone OR Linux + ESP32-HIL). Telemetry is compared against the host-simulator reference using **behavioral tolerance bands**, not SHA equality:
   - settling time within ±10% of reference
   - max overshoot within ±5% of reference
   - fault detection latency within the documented envelope (e.g., stall raised within 3 s)
   - PWM-seconds integral within ±15% of reference (acoustic proxy)
   
   Tolerance values are characterized empirically in the first nightly runs and pinned in `test/bench/tolerances.yaml`. Tightening or loosening a tolerance is a deliberate PR. Hardware tests run nightly on a self-hosted runner if available; otherwise on-demand via `workflow_dispatch` or a `[hil]` PR label.

**Why split:** physical hardware cannot produce byte-equal telemetry to a host simulator (tach jitter, USB-CDC latency, CAN frame timing all vary by microseconds tick-to-tick), so a SHA-equality gate against hardware would be either flaky or set so loose it catches nothing. Deterministic replay parity catches code-path divergence cleanly; tolerance bands catch hardware/closed-loop regressions credibly.

**Heads-up on effort budget.** Even the deterministic replay parity is the single most valuable test in the plan and the hardest to set up correctly. Expect to spend significant effort characterizing what "identical synthetic input" means across rigs (snapshot generation, sample ordering, timestamp alignment) and tightening any source of nondeterminism the determinism job in Stage 12 didn't already catch. When it works, it eliminates 90% of "works on Linux, breaks on ESP32" risk forever. The physical HIL tolerance bands take additional empirical work — plan for several nightly runs to settle them.

**Other Stage 15 deliverables:**
- Bench-rig benchmark table from PRD §9.2 captured (step time, memory, settling, overshoot, PWM-seconds, fault latency).
- Memory-footprint check: ESP32 binary size tracked over time as a CI artifact; trend visible in PR comments.

**CI rigor added:** `replay-parity` (**conditional gate** — PR/merge-queue when ESP32-C6 QEMU or a self-hosted runner exists; release/nightly otherwise); `hil-tolerance` (nightly or `[hil]`-labeled, never PR-gating).

**Exit gate:** all previous green, plus `replay-parity` green **on the gating tier the repo is currently in**. If `ci/runner-strategy.md` records "QEMU available" or "self-hosted ESP32-C6 connected," `replay-parity` is a PR gate. Otherwise the Stage 15 exit gate is "release-tag run of `replay-parity` green" — i.e., before any `v*` tag is cut, the parity job must have passed on a recent nightly. `hil-tolerance` is informational on PRs, blocking only on release tags.

---

### Stage 16 — White paper integration
**Deliverable:** Figure regeneration pipeline. `make -C docs/paper figures` runs the canonical scenarios, regenerates every matplotlib plot from fresh telemetry CSVs, rebuilds the PDF. Bench-rig photo, BOM table, scenario plots, benchmark tables.

**Benchmark manifest** (`docs/paper/figures/manifest.yaml`): the load-bearing record of what each figure depends on. Each entry contains: scenario name, telemetry-CSV SHA-256, config canonical-hash, source git SHA used to generate the underlying data, and tool versions. The figure caption cites the *data SHA* (from the manifest), not the current commit. This way:
- text-only paper edits don't make every figure look stale.
- benchmark regeneration produces a manifest update committed alongside the new data.
- the release workflow verifies caption SHAs match manifest entries (not the current commit).
- reviewers can tell at a glance whether a paper PR changes prose only or re-baselines benchmarks.

**Tests added:**
- **Paper build:** `make -C docs/paper` produces a PDF without manual intervention. Job in `release.yml`, not blocking PR merges.
- **Figure freshness:** CI verifies every figure caption's data SHA matches an entry in `manifest.yaml` and that the referenced telemetry CSV's hash matches. Stale or hand-edited figures fail loudly.

**Exit gate:** `release.yml` produces the PDF; manifest is internally consistent; caption SHAs map to manifest entries.

---

## 6. Paper update cadence

The white paper is not a Stage-16-only deliverable. Conceptual sections draft in parallel with code; results-bearing sections fill in as benchmarks land. This table maps stages to paper sections that can credibly advance once that stage lands.

The PRD §12.2 paper structure is the reference for section numbers. PRD §12.4 maps those numbers to the `01-…`–`13-…` source files under `docs/paper/src/`.

| Stage that lands | Paper sections that can advance | Trigger / outcome used |
|---|---|---|
| Pre-Stage 0, in parallel with 0–7 | §2 Introduction and motivation, §3 Background and related work, §4 Architecture (prose around block diagrams), §5 Acoustic-thermal tradeoff math, §7 Implementation notes, §8 Bench rig (BOM + wiring + photo when ready) | Conceptual content sourced from PRD; no code outcome required |
| Stage 2 | §5 Curves — interpolation figure and formula commentary | Curve module golden + integer-reference cross-check |
| Stage 3 | §5 Curves — filter-response figure for sensor IIR | Filter module golden |
| Stage 5 | §6 Response time — PID step-response prose, shape only (no quantitative settling-time numbers yet) | PID module golden |
| Stage 7 | §9 Zone controller — full-loop block diagram, governor/modifier/arbitration prose | First full-step golden |
| Stage 9 | §10 JSON config — schema examples locked from the actual loader; appendix's reference config | JSON loader + json2static round-trip green |
| Stage 11 | §12 Deployment — CAN/OBD-II integration narrative | `car-can-emulator` integration green on `vcan0` |
| Stage 12 | §10 Evaluation — first batch of scenario plots (heat soak, step load, fan stall, acoustic mask on/off) regenerated from real telemetry CSVs | All canonical scenarios green + determinism stable |
| Stage 13 | §6 Portability strategy — measured ESP32-C6 `.text`/`.bss` numbers in the budget table | ESP32-C6 build green + size budgets |
| Stage 15 | §10 Evaluation — cross-platform parity tables, full benchmark table, per-platform comparison | Replay parity green; HIL tolerance bands characterized |
| Stage 16 | §1 Abstract, §11 Honest limitations, §13 Conclusions, §12 Future work; tighten + final prose pass | Everything else; written when the paper knows what it's saying |

**Rules of thumb:**

- A paper PR that touches only conceptual prose is a docs-only PR (runs the lightweight `docs-lint` job, not the heavy matrix).
- A paper PR that touches a results table or scenario plot must also update `docs/paper/figures/manifest.yaml`; CI's figure-freshness check (Stage 16 deliverable) blocks merge if the manifest entry doesn't match.
- An "Honest limitations" sentence that didn't survive contact with implementation gets rewritten in Stage 16, not patched late. Better to delete an overclaim than to apologize for it.

---

## 7. Regression-finding workflow

The plan is built so that when something breaks, finding it is mechanical:

### 7.1 A unit test failed
The test names the module. The PR's diff names the change. Use `git bisect` only if a long-dormant test fails after many merges — usually a single PR is the culprit.

### 7.2 A golden replay diff is non-empty
Open `test/replay/golden/<name>.csv.diff` (CI uploads the diff as a job artifact). The diff is a precise behavioral delta. Two outcomes:
- **Unintended:** find the bug, fix it, golden stays unchanged.
- **Intended:** run `make regen-replay-goldens`, review the new goldens (they're committed in the same PR), reviewer explicitly approves the behavior change.

This is the principal regression-detection lever — the moment a math change has any unintended ripple, the diff makes it visible in the PR.

### 7.3 A scenario assertion failed
The assertion text says what was expected. The scenario's telemetry CSV is in CI artifacts; open it in `thermalcore-probe --plot` to see the trajectory. Most scenario failures are simulator-plant interaction bugs that unit tests can't catch.

### 7.4 A determinism test failed
The two telemetry CSVs have different SHA-256. Diff them with `csvdiff`. The first row where they diverge is the tick where nondeterminism crept in. Common causes: uninitialized memory (caught by ASan in a later CI job), accidental float math, hash-table iteration order.

### 7.5 Fuzz crashed
The fuzzer dumps a reproducer to `crash-<hex>.bin`. Reproduce locally with `./fuzz_json crash-<hex>.bin`. Add the reproducer to the seed corpus as a regression sentinel — even after the fix, future PRs run it.

### 7.6 Sanitizer caught something
Stack trace in the CI log. Fix and add a focused unit test that exercises the path the sanitizer flagged.

### 7.7 Cross-platform parity failed
The Linux telemetry SHA and the ESP32 telemetry SHA differ. Run both rigs side-by-side, diff the CSVs at the first divergence. This is the test that catches "works on Linux, breaks on MCU" subtly.

---

## 8. Conventions

### 8.1 PR hygiene
- One logical change per PR. A feature + its tests + its golden updates is one PR.
- Goldens change explicitly: a PR that updates `test/replay/golden/*.csv` without a stated reason in the PR description should be questioned in review.
- New error codes / new event codes / new signal IDs added to `core/thermal_*.h` are mentioned in the PR description so the wire-protocol surface is reviewable.

### 8.2 Commit hygiene
- Subject line: `<area>: <short summary>` (e.g., `pid: clamp integral on saturation`).
- Body explains the why; the diff explains the what.
- Tests live in the same commit as the code they exercise.

### 8.3 Branching
- `main` is always green. CI gates block direct pushes to `main`; merges happen via PR only.
- Feature branches off `main`. Rebase, don't merge, before opening a PR.

### 8.4 Naming
- C functions: `thermal_<module>_<verb>` — `thermal_pid_step`, `thermal_zone_aggregate`.
- Test names: `TEST_CASE(<module>_<scenario>)` — `TEST_CASE(pid_anti_windup_under_saturation)`.
- Goldens: `test/replay/golden/<module>_<scenario>.csv` — `pid_step_load.csv`.

### 8.5 Where the simulator lives
The deterministic thermal-plant simulator (PRD §9.3) is implemented once in `tools/thermalcore-scenario/plant.c` and `plant.h`, then linked into three places:
- `test/replay/` for replay tests (deterministic seed, scripted load/ambient).
- `test/unit/` for unit-level simulator tests.
- The scenario runner CLI.

This is the load-bearing simulator code; it gets the same review scrutiny as the core.

---

## 9. Stages summary

| Stage | Deliverable | New CI layer | Exit gate |
|---|---|---|---|
| 0 | Scaffolding + tool pins + no-heap guard | unit, build-linux, no-heap-no-syscall | dummy test + static denylist + core-only runtime guard all green |
| 1 | Core types declared | size-budget test | headers compile |
| 2 | Curve interpolation | replay | curve module golden + integer reference green |
| 3 | IIR + sensor pipeline + validity lifecycle | — | filter module golden + integer reference green |
| 4 | Zone agg + step-wise governor | property-config | zone module golden + config property green |
| 5 | PID (Q16.16) | — | PID module golden + integer reference green |
| 6 | Fault detectors | asan-ubsan | per-detector module golden green |
| 7 | Modifier + arbitration + slew + full loop | clang-tidy | full-step golden + safety-override tests green |
| 8 | apply_command + get_state (typed API) | cppcheck, property-command | typed command tests + property green |
| 9 | Linux daemon + JSON + json2static + telemetry UDP + deterministic clock | smoke, coverage, fuzz-json | daemon smoke + json round-trip + padding-poison hash green |
| 10 | Control plane + tune CLI + **`protocol/` wire codec** | fuzz-wire | wire round-trip + ack + timestamp + seq-wrap green |
| 11 | SocketCAN + OBD-II + emulator | — | vcan0 integration green on canonical CI |
| 12 | Scenario runner + C plant simulator + 10 scenarios | scenario, determinism | all canonical scenarios + determinism green |
| 13 | ESP32-C6 STANDALONE + REPLAY_STANDALONE builds | build-esp32 | C6 RISC-V build + REPLAY_STANDALONE build + size budgets green |
| 14 | ESP32 HIL_PERIPHERAL build | — | HIL build green; bench run on demand |
| 15 | Deterministic replay parity + benchmarks | replay-parity (conditional), hil-tolerance (nightly) | host vs ESP32 standalone byte-equal replay green when infra exists; otherwise release-gate |
| 16 | White paper figures + benchmark manifest | (release workflow) | PDF builds + manifest consistent |

---

## 10. Open decisions for later

Items deliberately deferred from this plan; resolve when the relevant stage starts:

- **Coverage gate threshold.** Stage 9 introduces lcov as visibility only. A future decision: should coverage become a gate (e.g., new code must keep total coverage ≥ 80%)? Defer until coverage baseline is established.
- **ESP32-C6 emulator in CI.** Stage 13 cross-compiles for ESP32-C6 (RISC-V) but doesn't run the binary in CI by default. Once `qemu-system-riscv32` with ESP32-C6 emulation, ESP-IDF's C6 QEMU image, or a stable self-hosted ESP32-C6 runner becomes available, the cross-platform unit replay can move to a real instruction-accurate run in CI. Until then, target-instruction replay stays nightly or hardware-driven. Defer until infrastructure exists.
- **Self-hosted runner for HW-in-CI.** v1 keeps bench scenarios manual. If bench failures start landing in `main` repeatedly, promote the bench Pi 4 to a self-hosted runner gated on a `[hil]` PR label.
- **Property test budget.** Stage 4 introduces property testing with a 100-case budget per PR. If shrinkage costs become noticeable, drop to 20 cases per PR + 1000 cases nightly.

---

*End of implementation plan v0.6*
