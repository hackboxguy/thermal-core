# thermal-core — Implementation Plan

**Document status:** Draft v0.3
**Author:** Albert David
**Companion to:** [thermal-core-prd.md](thermal-core-prd.md) (v0.10)

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
| 2 | `replay` | runs module/full-step golden replay tests; diff goldens on failure |
| 4 | `property` | randomly generated configs/commands through validator and `apply_command`; no crashes, no undocumented status |
| 6 | `asan-ubsan` | rebuilds with `-fsanitize=address,undefined` and re-runs unit + replay |
| 7 | `clang-tidy` | `clang-tidy` on `core/` and `platform/linux/`; new warnings fail |
| 8 | `cppcheck` | `cppcheck --error-exitcode=1 core/ platform/linux/` |
| 9 | `coverage` | lcov over unit + replay; uploads HTML to the PR as an artifact; **no gate** in v1, visibility only |
| 9 | `smoke-linux` | starts the daemon, asserts it boots and emits telemetry |
| 9 | `fuzz-json` | libFuzzer over the JSON loader, 60 s per PR, 30 min nightly |
| 10 | `fuzz-wire` | libFuzzer over the wire decoder, same shape |
| 12 | `scenario` | runs all canonical scenarios; assertions decide pass/fail |
| 12 | `determinism` | reruns scenarios twice, compares SHA-256 of telemetry |
| 13 | `build-esp32` | `idf.py build` for ESP32-C6 (RISC-V) on the ESP-IDF docker image; size-budget assertions |
| 15 | `replay-parity` | runs a fixed synthetic input stream through host build and ESP32 standalone (over QEMU when available, otherwise nightly via hardware); asserts byte-identical telemetry SHA between host and ESP32 standalone |
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
**Deliverable:** Repo skeleton per PRD §10, but only the directories that will be filled in stage 1. `core/thermal_config.h` with compile-time maxima from PRD §4.2. `test/unit/harness.h` with the assertion macros. `.github/workflows/ci.yml` running an empty test binary that prints "no tests yet" and exits 0. `Makefile` at `platform/linux/Makefile` and a top-level `Makefile` that delegates to `test/`.

**Tests added:** One trivial unit test (`TEST_CASE(harness_works) { EXPECT_EQ(1, 1); }`) to prove the harness compiles and links.

**Heads-up.** If hand-rolled C99 test harnesses are unfamiliar territory, debug `harness.h` against this trivial assertion before opening Stage 1 — `EXPECT_EQ` macro behavior, ctest registration, and the build wire-up are easier to diagnose against a one-line test than against a failing real test. Better half a day here than half a day mid-Stage 5.

**Regression value:** Establishes the CI gate exists. Any future PR that breaks the build is blocked from merging.

**Exit gate:** `build-linux` and `unit` green on `main`.

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

**IIR formula (pinned before tests are written):**

```
filtered_next = filtered_prev + alpha_q16 * (sample - filtered_prev)
```

With this convention:
- `alpha_q16 = 0` holds the previous value (no update).
- `alpha_q16 = Q16_ONE` passes the new sample through unchanged.
- Intermediate values produce a first-order low-pass with time constant `T = (period_ms / alpha) - period_ms` (approximately, for small alpha).

This is the standard convention, pinned in PRD §4.5 + decision 32. Stage 3 just implements it; test expectations, goldens, and Python reference all derive from the PRD formula.

**Tests added:**
- **Unit:** step response, impulse response, `alpha = 0` (no update — output holds previous), `alpha = Q16_ONE` (passthrough — output equals input), intermediate alphas converging to the input asymptotically, Q16.16 saturation under extreme values, `valid=0` propagation (filter holds last good value or marks invalid per spec).
- **Module golden:** a noisy-sensor CSV (1000 samples with simulated jitter) → filtered CSV golden.
- **Reference cross-check:** a small integer-only Python reference (`test/reference/iir.py`) computes the same outputs on the same input using Q16.16 arithmetic; the C output must match exactly. Pure-integer reference avoids float rounding entirely; no scipy dependency needed for this stage.

**Regression value:** Filter math is small but easy to break with off-by-one shifts or accidental int promotion. Module golden catches that immediately; the integer reference catches drift the golden would happily re-baseline.

**Exit gate:** unit + replay green.

---

### Stage 4 — Zone aggregation + step-wise governor
**Deliverable:** `core/thermal_zone.c` (sensor aggregation: max, avg, weighted), `core/thermal_governor.c` (step-wise governor with hysteresis), trip-point evaluation. Active-trip-mask computation. PRD §4.7 partial-validity rules.

**Tests added:**
- **Unit:** aggregation modes including partial-invalid sensors, weighted with edge weights (one weight zero, one weight dominant), trip enter/exit with hysteresis, multiple-active-trip "highest state wins" logic.
- **Module golden:** a heat-soak ramp CSV (45°C → 90°C → 45°C over 600 ticks) through a multi-trip zone; zone-state output captured to golden.
- **Property (new layer):** generated configs with 1–8 sensors, randomized trip points, run through validator; must never crash or return undocumented status.

**CI rigor added:** `property` job — generated configs through `thermal_core_validate_config()`.

**Regression value:** The step-wise governor is the simplest reference behavior; once locked, it's the cross-check for PID and modifier work that follows.

**Exit gate:** unit + replay + property green.

---

### Stage 5 — PID governor (Q16.16)
**Deliverable:** `core/thermal_pid.c`. Anti-windup per PRD §4.8. dt clamping. Derivative on measurement, optional first-order filter. Saturation telemetry on overflow.

**Tests added:**
- **Unit:** step response, settling time, anti-windup under sustained saturation, `dt_min_ms`/`dt_max_ms` clamp on missing or extremely late tick, non-monotonic `now_ms` jump (per PRD v0.10 — should clamp via dt, not crash; diagnostic event emitted), gain-change resets integral+derivative (per `CMD_SET_PID` contract), PID-trip-floor interaction (critical trip floors output via `state_pwm[cooling_state]`).
- **Module golden:** step-load CSV (50°C → 85°C step), full PID-term telemetry captured. Becomes the golden for tuning regressions.
- **Reference cross-check:** a pure-integer Q16.16 Python reference (`test/reference/pid.py`) computes the same outputs; the C output must match the Python output exactly. No float, no scipy — same arithmetic in both. An *optional* scipy-based float-reference comparison runs in nightly only, with a documented tolerance, to confirm the Q16.16 design is close enough to the textbook PID it implements.

**Regression value:** PID is the most-tuned single module. Module golden + integer reference catches math regressions immediately; the optional nightly scipy check sanity-checks the Q16.16 design itself.

**Exit gate:** unit + replay + property green.

---

### Stage 6 — Fault detectors (all four)
**Deliverable:** `core/thermal_fault.c` — stall, stuck-sensor, runaway, stale-context detectors. State machines per PRD §4.7. `THERMAL_FAULT_ACTION_*` handlers. Spin-up grace window. Latching only; the typed `CMD_CLEAR_FAULT` API lands in Stage 8 (`thermal_core_apply_command`); its wire transport lands in Stage 10. Stage 6 implements the state machine and the "is clear allowed yet?" check; later stages plug in the command paths.

**Tests added:**
- **Unit per detector:** entry conditions, persist_ticks behavior, recovery_ticks behavior, LATCHED requires explicit clear, stuck-sensor advisory mode without correlated context.
- **Runaway formula (per PRD v0.10):** active when `zone_temp_mc[N] - zone_temp_mc[M] >= rise_mc_threshold` with `M = N - persist_ticks`, AND `min(commanded_pwm[M..N]) >= cooling_pwm_threshold` for the affected actuator set. Test cases: rising temp under high PWM (fires), rising temp under low PWM (doesn't fire), flat temp under high PWM (doesn't fire), oscillating PWM that dips below threshold during the window (doesn't fire because of `min`).
- **Module golden:** each fault scenario as a CSV input + golden output: stall raise + recover, stuck sensor with correlated load, runaway under high-PWM rising-temp window.
- **Regression hook:** the stall scenario includes the spin-up window — catches future regressions where someone "fixes" stall logic and breaks spin-up grace.

**CI rigor added:** ASan/UBSan join CI here. From this stage on, sanitizer-clean is required for merge.

**Exit gate:** unit + replay + property + asan green.

---

### Stage 7 — Acoustic modifier + arbitration + slew + end-to-end step
**Deliverable:** `core/thermal_modifier.c` (acoustic_mask, pre + post stages), `core/thermal_arbitrator.c` (max-wins), slew-rate limiter, `core/thermal_core.c` wiring per the control-loop diagram in PRD §4.6. `thermal_core_step()` is now a complete end-to-end function.

**Tests added:**
- **Unit:** modifier pre-stage trip offset, modifier post-stage pwm-cap, modifier active-flag semantics, modifier output-domain clamping (interpolated `pwm_cap` clipped into `0..255` after curve eval, per the v0.10 clamp rule), stale-context fail_safe behavior, arbitrator under three-zone-one-actuator load, slew limiter bypass for upward safety overrides.
- **Full-step golden (first one):** the full closed loop — a 60-second `thermal_input_snapshot_t` stream driving every module in `thermal_core_step()`. Output frame + telemetry captured. This is the canonical "did anything change anywhere in the loop" canary.

**CI rigor added:** clang-tidy on `core/` only.

**Exit gate:** unit + replay + property + asan + clang-tidy green.

---

### Stage 8 — Runtime command + state inspection
**Deliverable:** `thermal_core_apply_command()` for all five v1 commands (`SET_PID`, `SET_SETPOINT`, `SET_TRIP`, `SET_CURVE_POINT`, `CLEAR_FAULT`). `thermal_core_get_state()`. PID-integrator reset on gain change. Curve-edit monotonicity check per PRD §5.3.

**Scope boundary:** Stage 8 tests the *typed* `thermal_command_t` API only. The wire encoder/decoder, CRC handling, and frame-level fuzz live in Stage 10. This keeps the boundary clean — semantic command validation here, frame integrity there.

**Tests added:**
- **Unit:** each command, valid + each error path; bounds enforcement; monotonicity rejection on curve edit; LATCHED clear gating; `TEVENT_COMMAND_APPLIED` / `_REJECTED` emission with the right `now_ms`.
- **`INVALID_ARG` vs `BOUNDS` split (per PRD v0.10):** unknown `command_id` → `INVALID_ARG`; unknown target ID → `INVALID_ARG`; `trip_idx >= trip_count` → `BOUNDS`; `point_idx >= curve_count` → `BOUNDS`; `kp > kp_max` → `BOUNDS`; curve edit breaking monotonicity → `BOUNDS`. Every documented path covered.
- **Full-step golden:** a "step response with mid-experiment kp change" sequence. Captures both the gain-change ack event and the resulting PWM response.
- **Property:** randomly generated *typed* `thermal_command_t` values (any command_id, any payload value) through `apply_command()` — must always return a documented status, never crash, never invoke a callback with malformed state.

**CI rigor added:** cppcheck.

**Exit gate:** unit + replay + property + asan + clang-tidy + cppcheck green.

---

### Stage 9 — Linux daemon: `bsp_mock_tmpfs` + JSON loader + `json2static.py` + telemetry UDP + probe
**Deliverable:** `platform/linux/thermalcored.c`, `bsp_mock_tmpfs.c`, `bsp_telemetry_udp.c`, `config_jsmn.c`, **`tools/json2static.py`**. The daemon boots from a JSON file, polls a tmpfs hwmon mock, runs `thermal_core_step()` every 100 ms, emits telemetry UDP frames. `tools/thermalcore-probe` (Python) reads UDP and writes CSV. `json2static.py` lands here (not at ESP32 bring-up) so the loader-to-C-struct mapping is round-trip tested at the same stage it's written.

**Tests added:**
- **Smoke (new test type):** daemon starts with `configs/minimal-1zone-1fan.json`, runs for 2 seconds, exits cleanly on SIGTERM, produced at least one telemetry frame on UDP.
- **JSON loader unit tests:** valid configs accepted; each documented invalid case rejected with the right status code and a clear error message.
- **Platform-only vs core config separation:** the JSON loader splits fields into `thermal_config_t` (deterministic policy) and `thermalcored_runtime_cfg_t` (platform-only — `source`, `pwm_freq_hz`, `tach_pulses_per_rev`, `telemetry.transport`, `telemetry.signals`, `control.listen`). A unit test loads the reference config and asserts each field landed on the right side of that line.
- **`json2static.py` round-trip:** parse `configs/minimal-1zone-1fan.json` → emit static `const thermal_config_t` C file → compile it into a tiny test binary → call `thermal_core_validate_config()` on the static config → assert `THERMAL_OK` and that the canonical config hash matches the hash computed by the JSON loader. Proves the two config paths reach the same in-memory representation.
- **Fuzz (new test type):** libFuzzer over the JSON loader, seed corpus = `configs/*.json`. 60 s/PR.
- **Daemon-level full-step replay:** write a known sequence of temp values to the tmpfs mock, capture the telemetry UDP stream, diff against golden.
- **Probe parity:** the same UDP stream replayed through `thermalcore-probe --log` produces the same CSV every time.

**CI rigor added:** smoke-linux, coverage (visibility only, no gate), fuzz-json.

**Exit gate:** unit + replay + property + asan + clang-tidy + cppcheck + smoke + fuzz-json green.

---

### Stage 10 — Control plane: command listener + `thermalcore-tune` + wire codec
**Deliverable:** UDP command listener on the daemon (`127.0.0.1:9002`), the wire encode/decode helpers in `core/thermal_commands.c` (CRC-16/CCITT-FALSE, packed encoding per PRD §7.2), `tools/thermalcore-tune` CLI.

**Tests added:**
- **Unit:** wire encoder/decoder round-trip for every opcode + command_id; CRC validation; sequence-number tracking; payload-cap rejection.
- **Daemon-level:** `thermalcore-tune set-pid soc 5000 400 0` produces a `CMD_ACK` with the request seq, and the next telemetry frames carrying `TSIG_PID_*` reflect the new gains. (v1 has no `CMD_GET_STATE` wire command — verification flows through the ACK + the existing telemetry stream, not a synchronous state pull.)
- **Timestamp propagation:** `thermalcore-tune set-setpoint soc 75000` at host wall time `t` produces a `TEVENT_COMMAND_APPLIED` whose `ts_ms` equals the `now_ms` the daemon passed to `thermal_core_apply_command()`. Catches regressions where command-apply events accidentally carry a stale or zero timestamp.
- **Fuzz:** libFuzzer over the wire decoder, seed corpus = recorded valid frames.

**CI rigor added:** fuzz-wire.

**Exit gate:** all previous + fuzz-wire green.

---

### Stage 11 — SocketCAN + OBD-II + `car-can-emulator` integration
**Deliverable:** `bsp_socketcan.c`, OBD-II PID 0x0D decode. `car-can-emulator` integrated as a git submodule and built by CI on `vcan0`. The acoustic_mask modifier is now driven by real CAN traffic in the test rig.

**Setup details to pin in this stage:**
- `.gitmodules` pins `tools/car-can-emulator` to a specific commit SHA on the `v2-improvements` branch (per PRD decision 3). Submodule update is a deliberate PR, not a floating dependency.
- CI provisions `vcan0`: a setup step runs `sudo modprobe vcan` and `sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0` on the GitHub Actions Linux runner. The standard `ubuntu-latest` image grants this; if a future runner image lacks `CAP_NET_ADMIN` the integration job degrades to a skip with a clear log, while unit tests continue to gate.
- The emulator's TCP control port is used *only* by scenario and integration tooling (`tools/thermalcore-scenario`, this stage's integration test). The core and the daemon never open the TCP port — they consume CAN frames only, per PRD §6.4.

**Tests added:**
- **Unit:** OBD-II frame encode/decode, response timeout handling, fail_safe fallback after `timeout_ms`.
- **Integration:** CI starts `car-can-emulator` on `vcan0`, starts `thermalcored`, sets speed via the emulator's TCP control port at `t=0, 5, 10, 15 s`, asserts the daemon's telemetry stream shows the expected speed values within the configured filter time constant.
- **Module golden:** a CAN bus-loss scenario (emulator killed mid-run) producing `assume_stationary` fallback.

**Exit gate:** all previous + CAN integration test green.

---

### Stage 12 — Scenario runner + thermal-plant simulator + canonical scenarios
**Deliverable:** Two artifacts with a clean split:
- **Plant math (C)**: `tools/thermalcore-scenario/plant.c` + `plant.h`, pure Q16.16, no Python. Linked into the scenario runner CLI, `test/replay/`, and `test/unit/` (per §7.5). Single source of truth for the simulator.
- **Scenario runner (Python)**: `tools/thermalcore-scenario/run.py`, orchestrates `.scn` file parsing, drives the C plant through a small CFFI/ctypes binding, talks to `thermalcored` over UDP, evaluates assertions, writes telemetry CSVs.

There is **no duplicate Python plant model**. If the Python orchestrator ever needs a plant value, it calls into the C library. This guarantees that scenario runs and unit/replay tests share bit-for-bit-identical plant arithmetic.

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

1. **Deterministic replay parity (PR-gating).** Host build and ESP32-C6 standalone build are fed *identical synthetic input streams* (no real sensors, no real CAN — just `thermal_input_snapshot_t` arrays played from a fixture file). Both rigs are deterministic; the SHA-256 of the resulting telemetry CSV must be byte-equal. This is the "works on Linux, breaks on MCU" canary, achievable because both rigs share the same `core/` source and Q16.16 math. Runs on every PR via the `replay-parity` CI job. When QEMU isn't available, the ESP32 side runs on a self-hosted runner nightly and gates the merge queue rather than the per-PR CI.

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

**CI rigor added:** `replay-parity` (PR-gating); `hil-tolerance` (nightly or `[hil]`-labeled, not PR-gating).

**Exit gate:** all previous + `replay-parity` green. `hil-tolerance` is informational on PRs, blocking only on release tags.

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

## 6. Regression-finding workflow

The plan is built so that when something breaks, finding it is mechanical:

### 6.1 A unit test failed
The test names the module. The PR's diff names the change. Use `git bisect` only if a long-dormant test fails after many merges — usually a single PR is the culprit.

### 6.2 A golden replay diff is non-empty
Open `test/replay/golden/<name>.csv.diff` (CI uploads the diff as a job artifact). The diff is a precise behavioral delta. Two outcomes:
- **Unintended:** find the bug, fix it, golden stays unchanged.
- **Intended:** run `make regen-replay-goldens`, review the new goldens (they're committed in the same PR), reviewer explicitly approves the behavior change.

This is the principal regression-detection lever — the moment a math change has any unintended ripple, the diff makes it visible in the PR.

### 6.3 A scenario assertion failed
The assertion text says what was expected. The scenario's telemetry CSV is in CI artifacts; open it in `thermalcore-probe --plot` to see the trajectory. Most scenario failures are simulator-plant interaction bugs that unit tests can't catch.

### 6.4 A determinism test failed
The two telemetry CSVs have different SHA-256. Diff them with `csvdiff`. The first row where they diverge is the tick where nondeterminism crept in. Common causes: uninitialized memory (caught by ASan in a later CI job), accidental float math, hash-table iteration order.

### 6.5 Fuzz crashed
The fuzzer dumps a reproducer to `crash-<hex>.bin`. Reproduce locally with `./fuzz_json crash-<hex>.bin`. Add the reproducer to the seed corpus as a regression sentinel — even after the fix, future PRs run it.

### 6.6 Sanitizer caught something
Stack trace in the CI log. Fix and add a focused unit test that exercises the path the sanitizer flagged.

### 6.7 Cross-platform parity failed
The Linux telemetry SHA and the ESP32 telemetry SHA differ. Run both rigs side-by-side, diff the CSVs at the first divergence. This is the test that catches "works on Linux, breaks on MCU" subtly.

---

## 7. Conventions

### 7.1 PR hygiene
- One logical change per PR. A feature + its tests + its golden updates is one PR.
- Goldens change explicitly: a PR that updates `test/replay/golden/*.csv` without a stated reason in the PR description should be questioned in review.
- New error codes / new event codes / new signal IDs added to `core/thermal_*.h` are mentioned in the PR description so the wire-protocol surface is reviewable.

### 7.2 Commit hygiene
- Subject line: `<area>: <short summary>` (e.g., `pid: clamp integral on saturation`).
- Body explains the why; the diff explains the what.
- Tests live in the same commit as the code they exercise.

### 7.3 Branching
- `main` is always green. CI gates block direct pushes to `main`; merges happen via PR only.
- Feature branches off `main`. Rebase, don't merge, before opening a PR.

### 7.4 Naming
- C functions: `thermal_<module>_<verb>` — `thermal_pid_step`, `thermal_zone_aggregate`.
- Test names: `TEST_CASE(<module>_<scenario>)` — `TEST_CASE(pid_anti_windup_under_saturation)`.
- Goldens: `test/replay/golden/<module>_<scenario>.csv` — `pid_step_load.csv`.

### 7.5 Where the simulator lives
The deterministic thermal-plant simulator (PRD §9.3) is implemented once in `tools/thermalcore-scenario/plant.c` and `plant.h`, then linked into three places:
- `test/replay/` for replay tests (deterministic seed, scripted load/ambient).
- `test/unit/` for unit-level simulator tests.
- The scenario runner CLI.

This is the load-bearing simulator code; it gets the same review scrutiny as the core.

---

## 8. Stages summary

| Stage | Deliverable | New CI layer | Exit gate |
|---|---|---|---|
| 0 | Scaffolding | unit, build-linux | dummy test passes |
| 1 | Core types declared | size-budget test | headers compile |
| 2 | Curve interpolation | replay | curve module golden + integer reference green |
| 3 | IIR + sensor pipeline | — | filter module golden + integer reference green |
| 4 | Zone agg + step-wise governor | property | zone module golden green |
| 5 | PID (Q16.16) | — | PID module golden + integer reference green |
| 6 | Fault detectors | asan-ubsan | per-detector module golden green |
| 7 | Modifier + arbitration + slew + full loop | clang-tidy | full-step golden stable |
| 8 | apply_command + get_state (typed API) | cppcheck | command tests green |
| 9 | Linux daemon + JSON + json2static + telemetry UDP | smoke, coverage, fuzz-json | daemon smoke + json round-trip green |
| 10 | Control plane + tune CLI + wire codec | fuzz-wire | wire round-trip + ack + timestamp green |
| 11 | SocketCAN + OBD-II + emulator | — | vcan0 integration green |
| 12 | Scenario runner + C plant simulator + 10 scenarios | scenario, determinism | all canonical scenarios green |
| 13 | ESP32-C6 STANDALONE build | build-esp32 | C6 RISC-V build + size budgets green |
| 14 | ESP32 HIL_PERIPHERAL build | — | HIL build green; bench run on demand |
| 15 | Deterministic replay parity + benchmarks | replay-parity (PR), hil-tolerance (nightly) | host vs ESP32 standalone byte-equal replay green |
| 16 | White paper figures + benchmark manifest | (release workflow) | PDF builds + manifest consistent |

---

## 9. Open decisions for later

Items deliberately deferred from this plan; resolve when the relevant stage starts:

- **Coverage gate threshold.** Stage 9 introduces lcov as visibility only. A future decision: should coverage become a gate (e.g., new code must keep total coverage ≥ 80%)? Defer until coverage baseline is established.
- **ESP32 emulator in CI.** Stage 13 cross-compiles for ESP32 but doesn't run the binary. Once `qemu-xtensa` or the ESP-IDF QEMU image is stable enough, the cross-platform unit replay can move to a real instruction-accurate run in CI. Defer until needed.
- **Self-hosted runner for HW-in-CI.** v1 keeps bench scenarios manual. If bench failures start landing in `main` repeatedly, promote the bench Pi 4 to a self-hosted runner gated on a `[hil]` PR label.
- **Property test budget.** Stage 4 introduces property testing with a 100-case budget per PR. If shrinkage costs become noticeable, drop to 20 cases per PR + 1000 cases nightly.

---

*End of implementation plan v0.3*
