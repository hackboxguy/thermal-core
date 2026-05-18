# thermal-core — Implementation Plan

**Document status:** Draft v0.12
**Author:** Albert David
**Companion to:** [thermal-core-prd.md](thermal-core-prd.md) (v0.16)

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
| 13 | `build-esp32` | `idf.py -DIDF_TARGET=esp32c3 build` (RISC-V) on the ESP-IDF docker image; size-budget assertions |
| 15 | `replay-parity` | **conditional gate.** Runs a fixed synthetic input stream through host build and ESP32 standalone; asserts byte-identical telemetry SHA. Becomes a PR/merge-queue gate **only when** a stable ESP32-C3 RISC-V QEMU path or a reliable self-hosted ESP32 runner exists. Until that infrastructure lands, this is a release/nightly gate. The plan does not promise an always-on PR gate before the runner strategy is real. |
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
- **Shipped 11d:** `test/integration/test_canbus_busloss.py` — bus-loss module golden.  Drives speed to 100 km/h, lets the IIR converge, SIGTERMs the emulator, waits `timeout_ms + 2 s` grace for the cascade (BSP timeout → filter.valid=0 → modifier fail_safe → effective_value=0), then asserts every `TSIG_CONTEXT_VALUE_0` frame in the post-loss observation window is `0` (the `assume_stationary` fail-safe per PRD §6.3).  Also asserts `daemon.poll() is None` throughout — the daemon must survive the bus loss.  Reuses `canbus-config.json` from 11c so the same daemon config covers both happy path (11c) and fail-safe (11d).  `make integration-can` now chains both scripts back-to-back.

**Exit gate:** all previous + CAN integration test green.  **Stage 11 closed:** unit (25 tests) + replay + property + property-command + portability + smoke + integration + integration-can + asan + clang-tidy + cppcheck + fuzz-json + fuzz-wire + coverage all green on canonical CI; PRD §6 v1 OBD-II surface fully covered.

**Known limitations (deferred to a future commit):**

- **IIR filter asymmetric truncation stall (RESOLVED in Stage 15a).**  `thermal_filter_step()` did `delta = ((int64)alpha_q16 * (sample - filtered)) >> 16`.  The arithmetic right-shift (gcc + clang both floor on negative) gives **exact** convergence on decreasing targets but **stalls positive-direction convergence** at `target - ceil(65536/alpha_q16)`.  For the canbus context (`alpha_q16 = 2048`) the positive stall is 32 km/h, so a setpoint jump from 88 → 200 km/h asymptotes at ~168 km/h instead of 200.  `test_canbus_obd2.py` works around it by **pre-setting the emulator to `PREINIT_KMH = 250` before the daemon starts** (the daemon's first OBD-II response goes through `thermal_filter`'s `initialized=0` shortcut, taking the value directly); subsequent setpoints `[200, 100, 50]` are all decreases, which converge exactly.  Note the emulator's compile-time default is 88 km/h, so without the pre-init the daemon's filter would init at 88 and any positive setpoint > ~103 would fail.  Fixed in Stage 15a, round-half-away-from-zero: `(p>=0) ? ((p+32768)>>16) : -(((-p)+32768)>>16)`.  (The `(product - 32768) >> 16` negative-case form noted in earlier drafts of this paragraph was wrong -- it biased negative deltas an extra -1; the sign-aware form above is correct.)  Regenerating the goldens changed only `filter_sweep.csv` -- the other eight use passthrough alpha or do not exercise the IIR with a fractional coefficient.
- **acoustic_mask scenarios + can_bus_loss scenario.**  `scenarios/acoustic_mask_high_speed.scn` and `scenarios/can_bus_loss.scn` originally asserted `max actuator_pwm 0 <= 200` during windows where the zone is in critical (plant_initial_temp_mc = 92000, above the 90000 trip).  PRD §4.6 lines 598/625/654 explicitly bypass acoustic caps at critical severity ("Affected actuators bypass acoustic caps"); the daemon's `thermal_core.c:1232` correctly skips the cap there.  Assertions dropped.  Both scenarios still exercise their target code paths (speed sweep + emulator startup; bus-loss BSP timeout cascade) and verify the critical-state PWM target (>= 220).  Uniquely exercising the cap-release-with-speed behaviour requires either a new trip schedule with `state_pwm[warn] > cap` (config change) or a daemon-level signal exposing `modifier_pwm_cap` separately from applied PWM (scenario DSL extension).  Deferred.

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

**Stage 12 actual deliverables (in progress):**

- **Shipped 12a:** `tools/thermalcore-scenario/plant.{c,h}` — deterministic first-order thermal-plant simulator (PRD §9.3).  Q16.16 throughout (no float / no `<math.h>`); per-zone state (temp, ambient, load, heat capacity, ambient drift, fan curve, fan max, neighbor coupling) plus a 32-bit LCG PRNG and optional noise amplitude.  `plant_step` advances every zone using a pre-tick neighbor-temperature snapshot so multi-zone coupling is order-independent.  Re-uses `thermal_curve_eval_y0` from `core/thermal_curve.c` for the fan curve (no duplicated interpolation logic).  10 unit scenarios cover the documented invariants — init/read, no-load ambient pull, load-only rise, load + max-fan suppression, fan-curve x=0 endpoint clamp, coupling, order-independence, PRNG determinism without and with noise, PRNG separation across seeds.  Object built with `-fPIC` so 12b's Python runner can link it into a shared library for `ctypes.CDLL`.  No Python runner, no scenario parser, no canonical scenarios yet — those land in 12b/12c.
- **Shipped 12b:** Python orchestration on top of 12a's plant.  `tools/thermalcore-probe` (UDP → CSV, `--log` mode only; `--live` + `--plot` are deferred to the paper build); `tools/thermalcore-scenario/plant_ffi.py` (ctypes binding to `libplant.so`, mirrors the C struct layouts bit-for-bit); `tools/thermalcore-scenario/scenario.py` (a minimal `.scn` parser supporting `plant ambient_mc / initial_temp_mc / duration_ms`, `assert max <kind> <slot> <op> <int> between <t0> <t1>`, `assert no_faults` — every other directive raises `ScenarioParseError` so 12c knows what to extend); `tools/thermalcore-scenario/run.py` (orchestrator that spawns the daemon under `--clock=scenario`, drives the plant + TICK loop, drains UDP telemetry via the probe, evaluates assertions); first canonical PRD §9.1 scenario `scenarios/idle_steady_state.scn` (10 s at 50000 mc ambient, asserts `max actuator_pwm 0 <= 0` + `no_faults`); `test/integration/scenario-config.json` (step_wise zone, no PID/no CAN, telemetry on UDP 9030); new `build/tools/thermalcore-scenario/libplant.so` Makefile rule and `make scenario` PHONY target.  CSV format is byte-stable in preparation for 12d's SHA-256 determinism gate.  Remaining nine PRD §9.1 scenarios, the `scenario-linux` CI job, and the determinism gate land in 12c/12d.
- **Shipped 12c:** 6 non-CAN canonical PRD §9.1 scenarios + parser/runner extensions + `scenario-linux` CI job.  Scenarios: `heat_soak_ramp` (7 stepped freezes 45→92°C, fan should respond at warn + critical trips); `step_load` (50→85°C step at t=5s, fan reaches cooling state 1); `fan_stall_recovery` (warm zone + freeze_tach 0 → stall fault fires within persist_ticks; unfreeze → recovers); `stuck_sensor` (freeze sensor at 50000 mc → stuck-sensor detector fires after window+persist); `multi_zone_coupling` (2-zone amp+tuner plant with coupling, shared fan, max-wins arbitration); `runaway` (plant fan_max=0 + 200 W load → daemon's runaway detector fires).  Parser additions: per-zone `plant zone <z> <field> <int>` directives, `plant zone_count <int>`, timestamped commands (`freeze_input` / `unfreeze_input` / `freeze_tach` / `unfreeze_tach` / `set_plant_load_w_q16` / `set_plant_fan_max_q16`), assertion forms (`assert min`, `assert eventually`, `assert within <ms> fault_active <kind> <slot>`, `assert no_faults except <kind>`), optional `config <path>` directive so `.scn` files are self-contained.  Two new daemon configs: `scenario-config-faults.json` (all 3 fault detectors enabled) and `scenario-config-multizone.json` (amp + tuner zones sharing main_fan).  `make scenario` now iterates `scenarios/*.scn`; new `scenario-linux` CI job runs them on every PR.  **Bug fix from 12b:** `FAULT_EVENT_CODES = {0x1000, 0x1001, 0x1002}` to match `core/thermal_events.h` (12b had `0x4000+`; `no_faults` would have been spuriously true for every fault scenario).  The 3 remaining CAN-dependent scenarios + determinism gate + Stage 12 exit-gate close land in 12d.
- **Shipped 12d:** 3 CAN-dependent canonical PRD §9.1 scenarios + determinism gate + Stage 12 close.  Scenarios: `acoustic_mask_low_speed` (speed = 0 → modifier caps PWM at 180 even under critical-band heat); `acoustic_mask_high_speed` (sweep 0 → 50 → 130 km/h → cap relaxes; PWM rises past 220); `can_bus_loss` (high speed locks in cap=255, kill_emulator at t=10s → after `timeout_ms` modifier reverts to `assume_stationary` and re-caps at 180).  Runner extensions: auto-detects CAN dependency from the daemon config (`canbus:<iface>` source); spawns `car-can-emulator` alongside the daemon when vcan is available; SKIP-cleanly otherwise (mirrors Stage 11 `integration-can` pattern).  Two new scenario commands: `<ms> set_emulator_speed <kmh>` (TCP "speed N" to port 8080) and `<ms> kill_emulator` (SIGTERMs the emulator).  Determinism gate: `tools/thermalcore-scenario/check_determinism.py` runs every deterministic (non-CAN) scenario twice and SHA-256s the captured CSV, then rebuilds the daemon + libplant under both gcc and clang and verifies identical CSV bytes across compilers.  Q16.16 plant + `--clock=scenario` daemon + byte-stable CSV (12b) means there's no compiler-dependent floating-point drift.  New `make determinism` PHONY target and two new CI jobs (`scenario-can-linux`, `determinism-linux`).

**Exit gate:** all previous + scenario + determinism green. **CLOSED.**  unit + replay + property + property-command + portability + smoke + integration + integration-can + scenario-linux + scenario-can-linux + determinism-linux + asan + clang-tidy + cppcheck + fuzz-json + fuzz-wire + coverage all green on canonical CI.  PRD §9 v1 testing surface fully covered.

---

## v1 Linux Release Gate Met

After Stage 12, the Linux v1 release product is complete.  Every CI
job in the impl-plan §3 table passes on `main`; all 10 PRD §9.1
canonical scenarios run on every PR; SHA-256-byte-equal results
across runs and across gcc/clang prove deterministic playback.
Stages 13+ scope ESP32 + HIL + scenario plotting — they extend
the product but the Linux v1 release artefact does not change.

---

### Stage 13 — ESP32 STANDALONE build
**Deliverable:** `platform/esp32_idf/main/*` with thin BSP wrappers around LEDC, PCNT, 1-Wire, I2C, TWAI. `app_main()` ≤ 100 lines. The static `thermal_config_t` is generated by `tools/json2static.py` (which landed in Stage 9 and was round-trip tested there) from the same JSON the Linux daemon uses. ESP32 standalone build runs the full thermal-core in a FreeRTOS task.

**Target detail:** v1 ships **ESP32-C3** (SuperMini board with USB-Serial-JTAG console).  C3 has working bench hardware in `/home/testpc/git-repos/claude/2026/tmp2/esp32-thermal-core`; STANDALONE mode doesn't need TWAI (CAN), so C3 and C6 are equivalent for this stage.  C6 remains a future target should CAN/TWAI move back into the ESP32 in a later stage (one-line `sdkconfig.defaults` flip).  ESP32-S3 / ESP32-WROOM (Xtensa) are not v1 targets.

**Build modes:** the firmware supports three build-time modes selected by `-DTHERMALCORE_MODE=…`:
- `STANDALONE` — full thermal-core running on the bench with real sensors/tach/CAN. Used for white-paper portability demonstrations.
- `HIL_PERIPHERAL` — peripheral concentrator (introduced fully in Stage 14).
- `REPLAY_STANDALONE` — **new in Stage 13.** Real sensors and CAN are stubbed out; the firmware reads a synthetic `thermal_input_snapshot_t` stream from a fixture (flashed into a `.rodata` section, or fed over USB-CDC by the host harness) and emits canonical telemetry over USB-CDC. This is the mode Stage 15 runs to compare host-vs-target byte-for-byte. Keeps deterministic replay parity orthogonal to the physical sensor path.

**Tests added:**
- **Build (required, PR-gating):** `idf.py -DIDF_TARGET=esp32c3 build` succeeds in CI under the ESP-IDF docker image. Size-budget assertions: `.text <= 64 KB`, `.bss <= 16 KB` (per PRD §9.2).
- **Cross-platform unit replay (target-instruction, optional):** the same `test/unit/` binary cross-compiled for `qemu-system-riscv32` (with the ESP32-C3 emulation target, when stable in ESP-IDF) runs replay tests; output CSVs must match the host build byte-for-byte. If a stable C3 QEMU path is not yet available, this step is a **nightly job, not a PR gate**, and uses real hardware via a self-hosted runner instead.
- **Manual:** ESP32-C3 flashed on the bench, runs `idle_steady_state` scenario, observed via probe over USB-CDC. Not in PR-gating CI.

**CI rigor added:** build-esp32 (required); target-replay (nightly, not gating).

**Stage 13 actual deliverables (in progress):**

- **Shipped 13a:** `platform/esp32_idf/` scaffold + 3 BSP wrappers (LEDC PWM, GPIO-ISR tach with 1 ms inter-edge filter, DS18B20 over RMT-based 1-Wire via the `espressif/onewire_bus` managed component) + minimal `app_main` that reproduces the existing `esp32-thermal-core` behaviour (linear temp → duty + 3 % hysteresis) but in Q16.16 integer math + the canonical project layout.  No `core/` link yet; 13b wires `thermal_core_step()` and switches to a `json2static`-generated `thermal_config_t`.  `idf.py set-target esp32c3 && idf.py build` succeeds locally with ESP-IDF v5.5.2.
- **Shipped 13b:** ESP32 firmware now runs `thermal_core_step()` against a `json2static`-generated `thermal_config_t`.  New `platform/esp32_idf/configs/esp32-c3-standalone.json` (single step_wise zone, trips at 30/45/60°C, no modifiers/CAN, no control listener) is converted at IDF build time by `tools/json2static.py` (CMake `add_custom_command` guarded by `NOT CMAKE_BUILD_EARLY_EXPANSION` per IDF requirements-pass rules).  `main/CMakeLists.txt` globs `core/*.c` + `protocol/*.c` into the IDF component — same portable C99 source as the Linux daemon, no platform-side reimplementation.  `protocol/` is needed because `core/thermal_commands.h` `_Static_assert`s pin against `protocol/thermal_wire_opcodes.h`; runtime encoders/decoders dead-strip.  New `main/esp32_callbacks.{c,h}` wires `log_event` + `telemetry_emit` to `printf`, caching `TSIG_ZONE_TEMP_*` and `TSIG_ACTUATOR_DUTY_*` so `app_main` can print one human-readable line per second.  Sensor reads stay at the DS18B20's natural 1 Hz cadence; the core ticks every 100 ms re-using the most recent valid sample.  Binary grew from `0x317e0` (204 KB) in 13a to `0x34e70` (216 KB) in 13b — only +12 KB delta for the entire `core/`+`protocol/` link thanks to LTO dead-strip; 79% of the 1 MB app partition free.  TC binary frame emission over USB-Serial-JTAG remains deferred to Stage 14 (HIL_PERIPHERAL); REPLAY_STANDALONE + CI matrix + size-budget land in 13c / 13d.
- **Shipped 13c:** ESP32 firmware gains a compile-time `THERMALCORE_MODE_REPLAY_STANDALONE` build mode (selected by `idf.py -DTHERMALCORE_REPLAY_STANDALONE=ON build`).  Replaces real BSP reads with a fixture-driven synthetic `thermal_input_snapshot_t` stream baked into `.rodata` (`main/replay_fixture.{c,h}`; initial fixture: 100 ticks at 50000 mc / 0 RPM, mirroring `scenarios/idle_steady_state.scn`) and switches the `telemetry_emit_cb` / `log_event_cb` bodies to emit byte-stable CSV rows matching `tools/thermalcore_probe.py` (sample row `ts_ms,sample,signal_id,value,,,,\n`; event row `ts_ms,event,code,a1,a1,a2,a3,a4\n` — duplicating `a1` into the `value` column to mirror the probe's `value=a1` projection on `tools/thermalcore_probe.py:124`).  `main.c` splits into `app_main_replay` / `app_main_standalone` under `#ifdef`; the 13b STANDALONE body is preserved verbatim.  After walking the fixture the replay path prints `END\n` and parks via `vTaskDelay(portMAX_DELAY)`.  The CMake option lives in `main/CMakeLists.txt`; `target_compile_definitions(${COMPONENT_LIB} PRIVATE ...)` keeps the flag component-scoped.  Both `idf.py build` (STANDALONE, `0x34e80` / 216 KB) and `idf.py -DTHERMALCORE_REPLAY_STANDALONE=ON build` (REPLAY, `0x28ed0` / 167 KB) SUCCEED locally with ESP-IDF v5.5.2.  The REPLAY image is ~49 KB smaller: LTO + `--gc-sections` actually dead-strip the BSP code path (LEDC, GPIO ISR, RMT 1-Wire driver, onewire_bus / DS18B20 components) once nothing references it, which is more aggressive stripping than I expected.  TC binary framing over USB-CDC + CI build matrix + size-budget assertions remain in Stages 14 / 13d; on-target byte-for-byte SHA-256 vs. host CSV lands in Stage 15.
- **Shipped 13d:** ESP32-C3 firmware lands on GitHub Actions.  New `build-esp32` job in `.github/workflows/ci.yml` builds both modes (`STANDALONE` + `REPLAY_STANDALONE`) inside the pinned ESP-IDF docker image via `espressif/esp-idf-ci-action@v1` (`esp_idf_version: v5.5.2`, `target: esp32c3`).  The STANDALONE leg additionally enforces the PRD §9.2 size budget (`.text ≤ 64 KB`, `.bss ≤ 16 KB`) on the `core/` + `protocol/` contribution to the linked firmware via a new pure-stdlib aggregator (`tools/check_esp32_size_budget.py`) that walks `core/*.c` + `protocol/*.c` to build the basename set (auto-picks up new sources), aggregates `.flash.text + .iram0.text + .dram0.bss` from `idf.py size-files --format json`, and exits non-zero on budget violation.  REPLAY just builds (its `--gc-sections` dead-strip drops drivers + BSPs, which would understate the real footprint).  New `make build-esp32` PHONY target mirrors what CI runs for local dev; SKIPs cleanly if `~/esp/esp-idf/export.sh` is missing so Linux-only devs don't pay for IDF setup.  Reconciled two stale lines in `ci/tool-versions.md`: ESP-IDF pin `v5.4 → v5.5.2` (the version that actually built locally in 13a-13c) and reference target `ESP32-C6 → ESP32-C3` (selected during impl-plan-review-v3).  Measured against the local STANDALONE build: `core/` + `protocol/` contribution is **11728 B `.text` / 0 B `.bss`** — 82 % `.text` headroom and 100 % `.bss` headroom under the PRD limits.  (Surprise: `protocol/` contributes 0 bytes to STANDALONE too, because `thermal_wire.c` + `obd2.c` are only referenced via `_Static_assert` pins in `core/thermal_commands.h`; nothing in the firmware calls the encoders / decoders at runtime, so LTO dead-strips them entirely.  Stage 14's TC binary framing over USB-CDC will pull them back in.)  **Stage 13 exit gate (line 471 below) is now green.**  TC binary frame emission over USB-CDC, ESP32-C6 cross-build, and target-replay parity all stay deferred to Stages 14 / 15.

**Exit gate:** all previous + build-esp32 green. Target-replay parity is enforced in nightly, not as a PR gate.

---

### Stage 14 — ESP32 HIL_PERIPHERAL build
**Deliverable:** Same firmware compiled in HIL mode (per PRD §8.3): ESP32 reports tach/temp/CAN to Linux over USB-CDC as `TELEM_SAMPLE` frames; Linux runs the thermal-core; Linux sends actuator commands back as `CMD_REQUEST` frames with platform-private command IDs.

**Tests added:**
- **Build:** HIL-mode firmware builds with size budgets met. PR-gating.
- **Integration (manual / nightly self-hosted, not PR-gating):** ESP32 + Linux on bench. Same canonical scenarios run, same assertions. HIL telemetry is compared against the standalone telemetry using **behavioral tolerance bands** (settling time within ±10%, max overshoot within ±5%, fault latency within the documented latency envelope), not SHA equality — physical tach jitter, USB-CDC latency, and CAN frame timing make byte-equal SHA infeasible. See Stage 15 for how the two flavors of parity are defined.

**Exit gate:** HIL build green; bench integration run succeeds when executed (manual or nightly).

**Stage 14 actual deliverables (in progress):**

- **Shipped 14a:** ESP32-C3 firmware gains a third compile-time build mode `THERMALCORE_MODE_HIL_PERIPHERAL` (selected by `idf.py -DTHERMALCORE_HIL_PERIPHERAL=ON build`).  In HIL mode the ESP32 owns the BSPs (PWM + tach + DS18B20) and the Linux daemon owns `thermal_core_step()`; 14a brings up the **outbound** half — read sensor + tach, encode `TELEM_SAMPLE` TC frames via `protocol/thermal_wire.c`, write to USB-Serial-JTAG.  Inbound `CMD_REQUEST` + PWM apply land in 14b; daemon-side `serial:` transport in 14c; CI HIL leg + size budget + Stage 14 close in 14d.  New `main/esp32_usb_cdc.{c,h}` wraps the C3's USB-Serial-JTAG ROM peripheral for binary I/O — no TinyUSB dependency, the IDF already binds stdio there.  `init` disables stdout line buffering (`setvbuf(stdout, NULL, _IONBF, 0)`) and mutes `ESP_LOG` entirely (`esp_log_level_set("*", ESP_LOG_NONE)`); binary frames cannot share the channel with ASCII log lines without a synchronizer, and 14a does not implement one.  `main.c` now dispatches `app_main` three-ways via `#if defined / #elif / #else` (HIL > REPLAY > STANDALONE); STANDALONE + REPLAY bodies preserved verbatim.  Provisional HIL signal IDs `HIL_SIG_SENSOR_TEMP_0 = 0x0701` + `HIL_SIG_TACH_RPM_0 = 0x0702` (in the 0x0700+ gap above the existing TSIG_ZONE/ACTUATOR/PID/CONTEXT/MODIFIER/FAULT 0x0100..0x06FF ranges); 14c will promote them into `core/thermal_signals.h`.  CRC is always on per PRD §7.2 (USB-Serial-JTAG isn't lossless).  CMake guards `REPLAY_STANDALONE` + `HIL_PERIPHERAL` as mutually exclusive — both flags ON triggers `FATAL_ERROR` during configure.  Sizes (local measurement): HIL binary `0x31D80` (199 KB), STANDALONE `0x34E80` (216 KB), REPLAY `0x28ED0` (167 KB) — HIL is **smaller than STANDALONE** because the whole `core/` is dead-stripped (daemon runs it), only `thermal_wire.c` encoder is pulled in (456 B `.text`, vs. 11728 B for STANDALONE's `core/`).  CI HIL leg + size-gate assertion remain deferred to 14d.
- **Shipped 14b:** Closes the inbound half of the HIL_PERIPHERAL data plane.  New `esp32_usb_cdc_read(buf, cap)` wraps `usb_serial_jtag_read_bytes(.., 0 ticks)` for non-blocking reads (the IDF auto-installs the USB-Serial-JTAG driver via `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` from 13a, so no explicit `driver_install` call).  `main.c` extends the HIL path with: a static 270-byte rx buffer (`THERMAL_WIRE_MAX_MCU + HEADER_LEN + CRC_LEN`); a sliding-window frame accumulator `hil_drain_commands()` that calls `thermal_wire_decode_frame()` repeatedly and drops 1 byte on any decoder error (`BAD_MAGIC` / `BAD_CRC` / `BAD_VERSION` / etc.) so the stream is self-healing; a `hil_dispatch_frame()` helper that open-codes the LE `command_id` unpack at `payload[0..1]` (the existing `thermal_wire_decode_cmd_request` rejects every `command_id` outside `0x0001..0x0005`, so platform-private IDs must be parsed directly).  New platform-private command ID `HIL_CMD_SET_PWM_DUTY = 0x8001` (PRD §8.3's `0x8000..0xFFFF` range): 3-byte payload (`u16 command_id LE + u8 duty`), applies via `bsp_esp32_pwm_set_duty(duty)`, ACKs with `status=0` and `detail=duty` (so the host can sanity-check that the firmware applied the exact value).  Unknown command ID or length mismatch → NACK with `THERMAL_WIRE_STATUS_BAD_PAYLOAD (0x8001)` and `detail=command_id`; wrong opcode → NACK with `BAD_OPCODE (0x8002)`.  The HIL loop now calls `hil_drain_commands()` **before** outbound TELEM_SAMPLE emission each tick so a freshly-arrived duty command takes effect on the same tick.  Single `seq` counter shared across ACK/NACK and TELEM_SAMPLE streams.  Local sizes: HIL binary `0x32510` (205 KB; +1.7 KB vs. 14a's `0x31D80` from the new dispatch + accumulator code); STANDALONE and REPLAY unchanged.  CI HIL leg + size-gate assertion still deferred to 14d; daemon-side `serial:` transport in 14c.
- **Shipped 14c:** Closes the HIL_PERIPHERAL loop end-to-end on the Linux side.  New `platform/linux/bsp_hil_serial.{c,h}` BSP (sibling of `bsp_mock_tmpfs` + `bsp_socketcan`): termios `cfmakeraw` + 115200 baud + `O_NONBLOCK` open of `/dev/ttyACM0`-shaped devices; sliding-window TC frame accumulator (lifted from 14b's ESP32 code with `THERMAL_WIRE_MAX_LINUX` buffer); per-signal sample cache keyed by `TSIG_HIL_SENSOR_TEMP(slot)` / `TSIG_HIL_TACH_RPM(slot)` populated on each `bsp_hil_serial_drain()`; ACK/NACK counters with NACK logged to stderr.  Hand-rolled `bsp_hil_serial_write_actuator()` builds a 17-byte CMD_REQUEST frame around `HIL_CMD_SET_PWM_DUTY = 0x8001` because the standard `thermal_wire_encode_cmd_request()` rejects every command_id outside `0x0001..0x0005`; `thermal_wire_crc16` is reused (already public from Stage 10).  `runtime_global_cfg_t` grows a `hil_transport[THERMAL_PATH_MAX]` field; `config_jsmn.c` parses a new top-level `"hil": { "transport": "serial:..." }` object sibling to `"telemetry"` / `"control"`.  `thermalcored.c` adds `g_hil_active` / `g_hil_serial_fd` globals + per-tick branches at three call sites: Step 1.6 drains inbound TC frames; Step 2 sensor + tach reads dispatch to `bsp_hil_serial_read_*` when HIL is active; Step 4 actuator apply emits CMD_REQUEST(0x8001) frames instead of writing to tmpfs.  UDP telemetry egress + UDP control listener stay unchanged.  Provisional HIL signal IDs were promoted into `core/thermal_signals.h` as `TSIG_HIL_BASE = 0x0700` with `TSIG_HIL_SUB_SENSOR_TEMP = 0x00` / `TSIG_HIL_SUB_TACH_RPM = 0x10` and `TSIG_HIL_SENSOR_TEMP(slot)` / `TSIG_HIL_TACH_RPM(slot)` macros following the existing `TSIG_ZONE_TEMP` / `TSIG_ACTUATOR_RPM` style.  **Wire-format change** between 14b and 14c: slot-0 IDs move from `0x0701/0x0702` to `0x0700/0x0710` — ESP32 firmware updated in lock-step (`TSIG_HIL_SENSOR_TEMP(0)` / `TSIG_HIL_TACH_RPM(0)`).  New `test/integration/hil-config.json` daemon config (1 sensor, 1 actuator, step_wise zone with the same 30/45/60°C trips as the ESP32 STANDALONE config; UDP telemetry stays on `127.0.0.1:9030` for the probe).  HIL binary `0x32510` → `0x32510` essentially flat after macro rename; STANDALONE and REPLAY unchanged.  CI HIL leg + size-gate assertion + PTY-driven integration test all still deferred to 14d.
- **Shipped 14d:** Locks Stage 14's HIL_PERIPHERAL mode into PR-gating CI and closes the stage.  `build-esp32` matrix gains a third leg (`mode: [STANDALONE, REPLAY_STANDALONE, HIL_PERIPHERAL]`); the size-budget gate's `if:` flipped from `matrix.mode == 'STANDALONE'` to `matrix.mode != 'REPLAY_STANDALONE'` so STANDALONE + HIL both enforce PRD §9.2.  HIL build measures 946 B `.text` for the `core/`+`protocol/` contribution (`thermal_wire.c` only — `core/` dead-strips since the daemon runs the core; the +490 B vs. 14a's 456 B reflects the inbound decoder + ACK/NACK encoders that 14b added).  `make build-esp32` now mirrors CI by building all three modes locally + running the size gate on STANDALONE + HIL.  New `test/integration/test_hil_serial.py` (~270 LoC) closes the `bsp_hil_serial.c` coverage gap from 14c: opens a Unix PTY pair via `pty.openpty()`, runs the daemon with `hil.transport = "serial:<pty-slave>"`, ramps a synthetic sensor temp from 25 °C → 75 °C over 50 ticks under `--clock=scenario` + DONE handshake, injects `TELEM_SAMPLE` frames on the master side, reads back the daemon's `CMD_REQUEST(HIL_CMD_SET_PWM_DUTY)` frames, asserts the duty visits all three cooling-state plateaus (100 / 160 / 220) as temp crosses 30 / 45 / 60 °C and that no NACK frames are emitted.  Pure stdlib — runs in the existing `integration-linux` CI job alongside `test_thermalcore_tune.py`, no kernel modules or privileged setup.  Local: `make integration` PASSES (50 CMD_REQUEST frames, duty range 0..220).  All 16 canonical jobs green on CI.  **Stage 14 exit gate met:** HIL build green; bench-integration manual / nightly procedure documented in [`docs/getting-started/hil-peripheral.md`](getting-started/hil-peripheral.md) for user-side execution.

**Post-Stage-14 refactor (JSON-driven ESP32 pin map).**  Not a numbered stage — a standalone refactor landed between Stages 14 and 15 so adding a second fan / DS18B20 to the bench rig is a JSON-only change.  Drops the hardcoded `#define PIN_*` constants in `main.c`; pin assignments now come from a new optional top-level `mcu_pinmap` JSON section (target-agnostic key; `json2static.py` emits a platform-scoped `G_ESP32_PINMAP` struct).  `main.c` + the three ESP32 BSPs (`bsp_esp32_pwm` / `_tach` / `_sensor`) become slot-indexed and loop over `cfg->sensor_count` / `actuator_count`; `bsp_esp32_sensor` handles multi-device DS18B20 on one shared 1-Wire bus (discovery-order ROM → slot mapping).  HIL `CMD_REQUEST(HIL_CMD_SET_PWM_DUTY)` payload extended 3→4 bytes (added a `u8 slot` byte) so the daemon can address per-fan duty; `bsp_hil_serial.c` + `test_hil_serial.py` updated in lock-step.  The dead daemon-shape fields (`sensors[].source`, `actuators[].pwm` / `.tach` / `.pwm_freq_hz` / `.tach_pulses_per_rev`) were removed from the ESP32 config — `mcu_pinmap` is the single source of truth for pin/hardware config on MCU builds.  See [`docs/getting-started/esp32-standalone.md`](getting-started/esp32-standalone.md) "Adding a second fan or sensor".

---

### Stage 15 — Cross-platform parity + benchmarks
**Two flavors of parity, deliberately separated:**

1. **Deterministic replay parity — conditional PR gate.** Host build and ESP32-C3 standalone build are fed *identical synthetic input streams* (no real sensors, no real CAN — just `thermal_input_snapshot_t` arrays played from a fixture file). Both rigs are deterministic; the SHA-256 of the resulting telemetry CSV must be byte-equal.

**Infrastructure precondition (be explicit about what we're promising):**
- If a stable ESP32-C3 RISC-V QEMU path exists in ESP-IDF *or* a reliable self-hosted ESP32-C3 runner is connected to the repo, `replay-parity` is a per-PR / merge-queue gate.
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

**CI rigor added:** `replay-parity` (**conditional gate** — PR/merge-queue when ESP32-C3 QEMU or a self-hosted runner exists; release/nightly otherwise); `hil-tolerance` (nightly or `[hil]`-labeled, never PR-gating).

**Exit gate:** all previous green, plus `replay-parity` green **on the gating tier the repo is currently in**. If `ci/runner-strategy.md` records "QEMU available" or "self-hosted ESP32-C3 connected," `replay-parity` is a PR gate. Otherwise the Stage 15 exit gate is "release-tag run of `replay-parity` green" — i.e., before any `v*` tag is cut, the parity job must have passed on a recent nightly. `hil-tolerance` is informational on PRs, blocking only on release tags.

- **Shipped 15a:** IIR round-to-nearest fix. `thermal_filter_step()` rounds the Q16.16 delta half-away-from-zero instead of flooring via a bare `>> 16`, so up- and down-convergence are symmetric (positive convergence no longer stalls `ceil(2^16/alpha)` short of target). `test/reference/iir.py` + `zone.py` updated in lock-step; `make regen-replay-goldens` rewrote the goldens — only `filter_sweep.csv` actually changed. The now-redundant `test_canbus_obd2.py` `PREINIT_KMH` workaround is documented for removal once a `vcan0` run can re-validate it.
- **Shipped 15b:** the C cross-platform-parity path. New `test/parity/` module — `canonical.{c,h}` (the pinned 9-column canonical telemetry CSV projection), `replay_fixture.{c,h}` (moved out of `platform/esp32_idf/main/`, rewritten as a deterministic 25→95→25 °C ramp crossing all four trips), `replay_run.{c,h}` (the replay loop, shared so the host and the ESP32 cannot drift in glue code), and `replay_host.c` → `build/parity/replay_host` (host port of `app_main_replay`, run against the same `json2static` `G_THERMAL_CFG`). The ESP32 REPLAY callbacks format through `canonical.c`.
- **Shipped 15c:** the replay-parity gate. `test/parity/canonical.py` mirrors `canonical.c`; `tools/thermalcore_probe.py` migrated onto it, so Stage 12 determinism and Stage 15 parity hash one projection. `test/parity/replay_parity.csv` is the committed golden. `make replay-parity-host` (host binary == golden — wired into CI as the per-PR `replay-parity-host` job) plus `make replay-parity` (bench: build + flash the ESP32 REPLAY firmware, capture its canonical CSV, byte-compare). The ESP32 REPLAY firmware loops the canonical CSV so the bench capture is race-free. `ci/runner-strategy.md` records the tier.
- **Shipped 15d:** Stage 15 infrastructure close. `test/bench/tolerances.yaml` seeds the physical-HIL behavioral bands (settling ±10%, overshoot ±5%, fault latency 3 s envelope, PWM-seconds ±15%) at the impl-plan spec defaults. The `build-esp32` CI job uploads each mode's `size-files.json` as an artifact for footprint tracking.

**Stage 15 status:** the deterministic replay-parity substance (15a–c) — the plan's highest-value test — is shipped and PR-gated via `replay-parity-host`; full host-vs-target `replay-parity` is the release-tag bench discipline per `ci/runner-strategy.md`. The physical-HIL behavioral characterization — the `make hil-tolerance` harness, the empirical tightening of `tolerances.yaml`, and the PRD §9.2 benchmark-table capture — remains **on-bench follow-on work**: the impl-plan budgets "several nightly runs" for the band characterization and the harness is bench-design-coupled. Stage 15 is closed on its parity substance plus infrastructure; the HIL-behavioral effort is tracked as a separate bench task.

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

- **Shipped 16a:** the white-paper figure-regeneration pipeline. New `docs/paper/figures/plots/` — a shared `_render.py` (parses the canonical 9-column telemetry CSV, renders the dual-axis temperature/duty figure with fault-event markers) and one thin `plot_<scenario>.py` per figure (PRD §12.3). A root-Makefile `paper-figures` target runs the canonical host scenarios through the existing scenario runner, then the plot scripts; `make -C docs/paper figures` delegates to it. First batch: `heat_soak_ramp`, `step_load`, `fan_stall_recovery`, `runaway` — the non-CAN, host-deterministic scenarios. The CAN-driven `acoustic_mask_*` pair is deferred with the acoustic benchmark work. matplotlib PDFs are made byte-stable (`savefig` metadata `CreationDate=None`).
- **Shipped 16b:** the benchmark manifest + figure-freshness gate. `docs/paper/figures/manifest.yaml` (written by `paper-figures` via `tools/figure_manifest.py`) records per figure: scenario, `csv_sha256`, config path + `config_sha256`, plus `git_sha` and `tool_versions`. The `\datasha{<8hex>}` caption macro cites the data SHA, not the commit. `tools/check_figure_freshness.py` regenerates the scenario CSVs, asserts every manifest hash still matches and that every `\datasha{}` prefixes a manifest entry; wired into CI as the per-PR `figure-freshness` job (daemon build, no texlive). `config_sha256` is a plain file-bytes SHA-256, not the canonical config hash — no Python canonical encoder exists and `csv_sha256` is the actual freshness signal. The rendered figure PDFs are gitignored: local matplotlib is off the `requirements-dev.txt` pin, so committed PDF bytes would not match the release build.
- **Shipped 16c:** `release.yml` — on `v*` tags only, never PR-gating. Installs the dev Python deps + TeX Live, regenerates the figures with the pinned matplotlib (`make -C docs/paper figures`), builds the PDF (`make -C docs/paper`), uploads it as a workflow artifact. Figure freshness is already gated per-PR by `figure-freshness`, so the release workflow does not re-run that check; it regenerates the figures from scratch.
- **Shipped 16d:** the four scenario plots embedded in the Evaluation section's new "Closed-Loop Scenario Behavior" subsection, each with a `\datasha`-tagged caption; `\graphicspath` extended to `../figures/`. Stage-16 prose pass: abstract, draft-status, and summary refreshed from "Stage 14" to the complete v1 plan; the stale "not yet wired up" / "To Fill In Later" notes in `05-curves`, `03-human-vision`, and `appendices` retired or corrected. White paper to Draft 0.3 (title, footer, revision row).

**Stage 16 status:** the figure-regeneration pipeline, the benchmark manifest, the per-PR freshness gate, the `v*`-tag release build, and the white-paper Draft 0.3 are shipped. Stage 16 is closed, which **completes the v1 implementation plan (Stages 0–16)**. Two items remain explicitly deferred as the documented on-bench follow-on (the same bucket as Stage 15d): the formal hardware benchmark sweep — calibrated acoustic and timing measurement, feeding the PRD §9.2 benchmark table and the CAN-driven `acoustic_mask_*` plots — and the bench-rig photograph. The paper stays an honest draft pending that data. Post-v1 work resumes at Stage 17 (fan-health detector) and Stage 18 (CH32V003 port).

---

### Stage 17 — Fan-health detector (post-v1)
**First post-v1 stage. Do not start until Stage 16 is closed and the v1 white paper has shipped.** Specified in PRD Appendix C.

**Deliverable:** `core/thermal_fan_health.{c,h}` — the minimal core-worthy slice of the fan-health detector. A compile-optional (`THERMALCORE_ENABLE_FAN_HEALTH`), advisory-only module that consumes the same `(commanded_pwm, tach_rpm, tach_valid)` triple as the stall detector, compares it against a per-actuator `const` PWM-to-RPM baseline tagged with its provenance (`field` / `factory` / `model`), and emits a graded `delta_pct`, a `severity` (HEALTHY / AGING / DEGRADED / FAILING), and the `baseline_source` through the telemetry callback. The severity ladder's resolution is gated by provenance — a model-generic baseline asserts only `DEGRADED` / `FAILING` and suppresses `AGING` (PRD Appendix C). It never commands an actuator. `tools/json2static.py` and the Linux JSON loader gain the per-actuator `fan_health` block (baseline table + source, stability windows, severity thresholds); the baseline is hand-authored config, not captured. No baseline capture, no NVS/file persistence, no CLI — those are deferred (see below).

**Tests added:**
- **Unit:** stability-gate windows (PWM/RPM stable-tick counting at threshold boundaries); `delta_pct` computation against a fabricated baseline; weighted `health_pct` aggregation; `severity` classification at exact threshold and ±1; provenance gating (a `model` baseline suppresses `AGING`, a `field`/`factory` baseline does not, on identical inputs).
- **Golden replay:** a fan-degradation fixture — a synthetic `(pwm, rpm)` snapshot stream that drifts progressively below baseline — captured as `test/replay/golden/fan_health_drift.csv`, asserting the expected `severity` escalation HEALTHY → AGING → DEGRADED → FAILING.
- **Reference cross-check:** a pure-integer Python reference (`test/reference/fan_health.py`), bit-exact against the C module like the other core modules.
- **Portability:** the `no-heap-no-syscall` gate stays green with `THERMALCORE_ENABLE_FAN_HEALTH=1` — the module adds no heap or syscall dependency.

**Regression value:** Locks the delta/severity math. The load-bearing invariant is the **advisory-only contract** — the module must never appear in an actuator command path; a unit test asserts the output actuator frame is byte-identical with the detector enabled vs. disabled on the same inputs.

**Exit gate:** all previous green, plus the new unit + replay + reference cross-check, and `no-heap-no-syscall` green with the feature compiled in.

**Follow-on (not yet staged):** the full predictive-maintenance feature — baseline capture sweep plus scenario directives, NVS/JSON baseline persistence, the `thermalcore-fanhealth` CLI, scheduled-probe mode, BLOCKED detection (positive-delta cross-correlated with rising zone temperature), the 2D temperature-compensated baseline, and the bench dust-loading experiments plus white-paper supplement — is a separate effort, scoped when Stage 17 lands.

---

### Stage 18 — CH32V003 STANDALONE port (post-v1)
**Post-v1 stage. Do not start until Stage 16 is closed. Independent of Stage 17 — both touch `core/` config, but in either order.** Specified in PRD Appendix D.

**Deliverable:** a self-contained thermal-core regulator on the WCH CH32V003 (RV32EC, 16 KB flash / 2 KB SRAM) — a real DS18B20 feeds `thermal_core_step()`, which drives a real fan over PWM with tach readback, on the chip, with no host. Two pieces:

1. **The tiny profile.** `#ifndef`-guard the `THERMAL_MAX_*` constants in `core/thermal_config.h` (currently plain `#define`s) so a profile header can override them; add an MCU profile (1 zone / 1 sensor / 1 actuator / 4 faults / 2 samples-per-snapshot) and cut `THERMAL_CORE_T_RESERVED_BYTES` from 4096 to ~1024. Target-agnostic — any future constrained-MCU port reuses it.
2. **`platform/ch32v003/`** — mirrors `platform/esp32_idf/`: `bsp_ch32_{pwm,tach,sensor}.c` (TIM2 PWM / EXTI tach / bit-banged DS18B20, adapted from the bench firmware), `mcu_pinmap`-driven `json2static` config, `ch32fun` vendored as a pinned git submodule at `platform/ch32v003/ch32fun/`, a plain Makefile, and a `make build-ch32` target. STANDALONE only; no on-device display (it does not co-fit 16 KB — PRD §D.2).

**Step 1 (do first):** a skeleton link — tiny-profile `core/` plus stub BSPs, cross-compiled for RV32EC — to confirm the PRD §D.2 ~15 KB flash budget on real `riscv64-unknown-elf-gcc` before the BSPs are built out. If the skeleton blows the budget, the BSP work does not start.

**Tests added:**
- **Build gate:** a `build-ch32` CI job — cross-compiles the firmware and asserts a `.text` / `.bss` size budget (mirrors `build-esp32`); SKIPs cleanly when `riscv64-unknown-elf-gcc` is absent, so Linux-only devs are unaffected.
- **Tiny-profile unit run:** the existing `core/` unit suite must pass compiled under the tiny profile — compiling at `maxima = 1` is not the same as behaving correctly at `maxima = 1`.

**Regression value:** Locks the tiny profile and proves the portability claim at the smallest target — a ~10-cent MCU running the identical `core/` source as the Linux daemon. The `#ifndef`-guarded maxima also unblock any future constrained port.

**Exit gate:** all previous green, plus `build-ch32` green and the `core/` unit suite green under the tiny profile.

- **Shipped 18a:** the tiny build profile. Every `THERMAL_MAX_*` (plus `THERMAL_CORE_T_RESERVED_BYTES` and `THERMAL_FAULT_RUNAWAY_WINDOW_MAX`) in `core/thermal_config.h` is now `#ifndef`-guarded; `core/thermal_profile_tiny.h` overrides them for a constrained MCU (1 zone / 1 sensor / 1 actuator; trips, cooling states, and curve points kept since one zone still runs the full staircase; reserved bytes 4096 → 1024, with the `thermal_core_t_fits` assertion confirming the internal struct fits). `make test-tiny-profile` clean-builds and runs the unit suite under the profile (force-included via `-include`); the resource-count-dependent sub-scenarios across five test files are `#if`-guarded to a no-op pass under maxima=1 — one (`test_config_hash` scenario 5) was a latent intra-struct out-of-bounds read. The default profile is byte-unchanged.
- **Shipped 18b:** the `platform/ch32v003/` skeleton + the flash-budget gate. ch32fun added as a pinned git submodule (`c29e297`, the commit the bench firmware was validated against). The platform dir is a flat ch32fun project mirroring `platform/esp32_idf/`: a Makefile (includes `ch32fun.mk`, force-includes the tiny profile, no `protocol/`), `configs/ch32v003-standalone.json`, `ch32_pinmap.h`, a STANDALONE `main.c`, and skeleton-stub BSPs. `json2static.py` gained `--pinmap-prefix` so it emits `ch32_pinmap_t G_CH32_PINMAP` (default stays `esp32`). `make build-ch32` cross-compiles and runs `tools/check_ch32_size_budget.py`. The skeleton linked at flash 9548 B of 16 KB — the budget gate passed with clear BSP headroom, so the BSP work proceeded.
- **Shipped 18c:** the real CH32V003 BSPs + control loop, adapted from Albert's validated bench firmware. `bsp_ch32_pwm.c` (TIM2 channel 1, ATRLR from the pin map's frequency), `bsp_ch32_tach.c` (EXTI line-0 edge count + 8 ms inter-edge filter), `bsp_ch32_sensor.c` (bit-banged DS18B20 over ch32fun's `static_onewire.h`, bus pin taken at runtime from the pin map). `main.c` gained SysTick-windowed pacing (the ~800 ms DS18B20 conversion absorbed within the 1 s control period) and an optional compile-gated SWIO status line. The full firmware cross-builds at flash 12 056 B of 16 KB (74 %), SRAM 1044 B of 2 KB (51 %).
- **Shipped 18d:** the CI gates + Stage close. A `build-ch32` CI job (apt-installs `gcc-riscv64-unknown-elf`, builds through the ch32fun submodule, enforces the budget, uploads the linker map) and a `unit-tiny-profile` job (the unit suite under the tiny profile). `ci/tool-versions.md` records the RISC-V toolchain and the ch32fun submodule pin. The white paper's Evaluation section gains a measured CH32V003 footprint paragraph (Draft 0.4).
- **Shipped 18e:** a canonical-CSV telemetry tap (post-close enhancement). `bsp_ch32_uart.{c,h}` is a direct-register USART1 driver (PD5 TX / PD6 RX, 8N1) in the BSP style; `ch32_callbacks.{c,h}` routes the core's `telemetry_emit` / `log_event` through `test/parity/canonical.c` — the same serializer the scenario runner and determinism gate use — and out the UART, so a bench capture is a canonical CSV. Compile-gated by `THERMALCORE_CH32_TELEMETRY` (`make build-ch32 CH32_TELEMETRY=1`), off by default and independent of the SWIO status line; `build-ch32` is now a 2-leg matrix so both variants stay built + size-budgeted. No `protocol/` — the canonical projection is plain text. Cross-build footprint: flash 12 220 B (default) / 12 688 B (telemetry) of 16 KB; on-hardware UART capture is bench follow-on.

**Stage 18 status:** the tiny profile, the `platform/ch32v003/` STANDALONE port, and the `build-ch32` + `unit-tiny-profile` CI gates are shipped; Stage 18 is closed. The CH32V003 firmware is verified to cross-compile, link, and fit the part (flash 74 %, SRAM 51 %). **On-hardware regulation has not been verified** — the BSP drivers are lifted from Albert's proven bench firmware, but their integration with `thermal_core_step()` is exercised only on the bench; on-device bring-up is the documented follow-on. Remaining post-v1 work is Stage 17 (fan-health detector), independent of Stage 18 and not yet started.

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
| Stage 13 | §6 Portability strategy — measured ESP32-C3 `.text`/`.bss` numbers in the budget table | ESP32-C3 build green + size budgets |
| Stage 15 | §10 Evaluation — cross-platform parity tables, full benchmark table, per-platform comparison | Replay parity green; HIL tolerance bands characterized |
| Stage 16 | §1 Abstract, §11 Honest limitations, §13 Conclusions, §12 Future work; tighten + final prose pass | Everything else; written when the paper knows what it's saying |
| Stage 17 (post-v1) | A new fan-health subsection — predictive-maintenance concept + degradation-drift results — as a post-v1 paper supplement | Fan-health detector module + golden replay green |
| Stage 18 (post-v1) | §6 Portability strategy — a 10-cent-MCU paragraph: the core running self-contained on the CH32V003, with the measured size budget | `build-ch32` green + measured budget |

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
The assertion text says what was expected. The scenario's telemetry CSV is in CI artifacts; open it to see the trajectory (the `docs/paper/figures/plots/` scripts render the canonical projection). Most scenario failures are simulator-plant interaction bugs that unit tests can't catch.

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
| 13 | ESP32-C3 STANDALONE + REPLAY_STANDALONE builds | build-esp32 | C3 RISC-V build + REPLAY_STANDALONE build + size budgets green |
| 14 | ESP32 HIL_PERIPHERAL build | — | HIL build green; bench run on demand |
| 15 | Deterministic replay parity + benchmarks | replay-parity (conditional), hil-tolerance (nightly) | host vs ESP32 standalone byte-equal replay green when infra exists; otherwise release-gate |
| 16 | White paper figures + benchmark manifest | (release workflow) | PDF builds + manifest consistent |
| 17 | **(post-v1)** Fan-health detector — advisory PWM→RPM drift module | — | fan-health module golden + reference cross-check green |
| 18 | **(post-v1)** CH32V003 STANDALONE port — tiny profile + platform/ch32v003/ | build-ch32 | tiny-profile build + unit suite + size budget green |

---

## 10. Open decisions for later

Items deliberately deferred from this plan; resolve when the relevant stage starts:

- **Coverage gate threshold.** Stage 9 introduces lcov as visibility only. A future decision: should coverage become a gate (e.g., new code must keep total coverage ≥ 80%)? Defer until coverage baseline is established.
- **ESP32-C3 emulator in CI.** Stage 13 cross-compiles for ESP32-C3 (RISC-V) but doesn't run the binary in CI by default. Once `qemu-system-riscv32` with ESP32-C3 emulation, ESP-IDF's C3 QEMU image, or a stable self-hosted ESP32-C3 runner becomes available, the cross-platform unit replay can move to a real instruction-accurate run in CI. Until then, target-instruction replay stays nightly or hardware-driven. Defer until infrastructure exists.
- **Self-hosted runner for HW-in-CI.** v1 keeps bench scenarios manual. If bench failures start landing in `main` repeatedly, promote the bench Pi 4 to a self-hosted runner gated on a `[hil]` PR label.
- **Property test budget.** Stage 4 introduces property testing with a 100-case budget per PR. If shrinkage costs become noticeable, drop to 20 cases per PR + 1000 cases nightly.

---

*End of implementation plan v0.12*
