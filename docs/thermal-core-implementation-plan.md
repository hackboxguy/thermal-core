# thermal-core — Implementation Plan

**Document status:** Draft v0.2
**Author:** Albert David
**Companion to:** [thermal-core-prd.md](thermal-core-prd.md) (v0.10)

This document describes how to build `thermal-core` incrementally, stage by stage, with the test automation that prevents regressions evolving alongside the code. Stages are ordered by dependency, not by calendar time — each stage closes with a green CI gate, and the next stage starts from that green main.

The guiding bet is simple: **every feature lands in the same PR as the automated tests that prove it works and the golden artifacts that will detect its future regression.** If a feature can't be exercised by an automated test, it's not done.

---

## 1. Guiding principles

1. **Stage-gated, not date-gated.** A stage ends when its CI gate is green on `main`. Time per stage varies; effort per stage is bounded by the size of the deliverable, not by the calendar.
2. **Tests and code ship together.** A PR that adds a feature must also add the tests that exercise it. CI rejects PRs whose new code is not covered by new tests of an appropriate type.
3. **Lean CI grows.** Stage 0 enables unit + build + replay only. Each subsequent milestone adds at most one new rigor layer (sanitizers, then static analysis, then coverage, then fuzz, then cross-build matrix) so noise stays manageable.
4. **Golden replays are the canary.** Once a feature has deterministic behavior, its output is captured to a checked-in golden file. Future changes that alter that output fail CI loudly until the golden is consciously re-recorded.
5. **Reproducibility is testable.** A determinism job re-runs the canonical scenarios and asserts the SHA-256 of the telemetry stream is unchanged. Catches accidental nondeterminism (uninitialized memory, time-dependent math, host floating-point drift) before it ships.
6. **Hand-rolled, no third-party test deps.** The harness lives in `test/unit/harness.h` as ~50 lines of C99 macros. Same harness compiles for ESP32 unit tests later. No Unity, no CMock, no Greatest.
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
A CSV of input snapshots is fed through `thermal_core_step()` in a tight loop; the output frame and all telemetry are captured to a CSV; the result is `diff`-compared against a checked-in golden CSV under `test/replay/golden/`. Byte-equal or test fails.

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

### 2.7 Property tests (Stage 12 onward, lean-and-grow)
Randomly generated configs (within compile-time maxima) run through `thermal_core_validate_config()`. The validator must either return `THERMAL_OK` or one of the documented error codes — never crash, never use uninitialized memory, never deadlock. Generated via simple QuickCheck-style helpers in `test/property/`.

**Catches:** validator crashes on configs the unit tests didn't think of.

### 2.8 Fuzz tests (Stage 9 onward for JSON, Stage 11 for wire decoder)
libFuzzer-driven fuzz of the JSON loader and the wire-frame decoder. Seed corpus = the example configs in `configs/` and a recorded set of valid frames. Run for 60 seconds per CI job; longer in nightly.

**Catches:** parser crashes on adversarial input, OOB reads on truncated frames, length-field handling bugs.

---

## 3. CI growth plan

The CI workflow is `.github/workflows/ci.yml`. Each row below is a `job:` added in the listed stage. Once added, a job runs on every PR.

| Stage added | Job | What it does |
|---|---|---|
| 0 | `build-linux` | gcc + clang, `-Werror`, builds core + harness |
| 0 | `unit` | runs all unit tests under `ctest` |
| 2 | `replay` | runs golden replay tests; diff goldens on failure |
| 6 | `asan-ubsan` | rebuilds with `-fsanitize=address,undefined` and re-runs unit + replay |
| 7 | `clang-tidy` | `clang-tidy` on `core/` and `platform/linux/`; new warnings fail |
| 8 | `cppcheck` | `cppcheck --error-exitcode=1 core/ platform/linux/` |
| 9 | `coverage` | lcov over unit + replay; uploads HTML to the PR as an artifact; **no gate** in v1, visibility only |
| 9 | `smoke-linux` | starts the daemon, asserts it boots and emits telemetry |
| 9 | `fuzz-json` | libFuzzer over the JSON loader, 60 s per PR, 30 min nightly |
| 11 | `fuzz-wire` | libFuzzer over the wire decoder, same shape |
| 12 | `scenario` | runs all canonical scenarios; assertions decide pass/fail |
| 12 | `determinism` | reruns scenarios twice, compares SHA-256 of telemetry |
| 13 | `build-esp32` | `idf.py build` on the ESP-IDF docker image; size budget assertions |
| 15 | `cross-platform-parity` | runs a fixed scenario on Linux mock + Linux+ESP32 HIL + ESP32 standalone; asserts identical telemetry SHA |

Three workflows in total:

- **`ci.yml`** — runs on every push and PR. Fast (target: under 10 minutes).
- **`nightly.yml`** — runs on cron. Longer fuzz, full scenario sweep, coverage trend, bench-rig scenarios (when self-hosted runner is available).
- **`release.yml`** — runs on `v*` tags. Builds white paper PDF, attaches release artifacts.

PR-gating CI is filtered to skip the full matrix when a change touches only documentation. The full `ci.yml` runs only when code paths change; docs-only PRs (PRD edits, paper text, this implementation plan) trigger a fast `docs-lint` job — markdown link check + table formatting — instead of the 10-minute matrix. Implemented via `paths-ignore: ['docs/**', '**.md']` on the heavy jobs and a complementary `paths: ['docs/**', '**.md']` on the docs job. Keeps text-only iteration cheap.

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

**Tests added:** A compile-time test that includes every public header and references each typedef once. Verifies size budgets: `sizeof(thermal_state_snapshot_t) < 1024`, `sizeof(thermal_config_t) < 16 KB`, etc.

**Regression value:** Pins the public ABI for tools and prevents accidental size blowups when fields are added later.

**Exit gate:** Everything compiles under `-Werror -pedantic`; the size-budget test passes.

---

### Stage 2 — Curve interpolation (first real testable function)
**Deliverable:** `core/thermal_curve.c` implementing the integer linear interpolation from PRD §4.8. Two functions: `thermal_curve_eval_y0(curve, x)` and `thermal_curve_eval_y1(curve, x)` (the acoustic_mask curve has two outputs).

**Tests added:**
- **Unit:** monotonicity, endpoint clamping, mid-point interpolation, two-point degenerate case, single-segment, eight-segment, exact-on-x-knot behavior. Twenty or so cases.
- **Golden replay (first one):** a "sweep speed from 0 to 200 km/h in 1 km/h steps" CSV; output CSV captured as `test/replay/golden/curve_sweep.csv`. Adds the `make regen-replay-goldens` helper and the `replay` CI job.
- **Reference cross-check:** a `numpy.interp`-based Python reference computes the expected outputs on the same x-axis; the C output must match within a Q16.16 rounding tolerance. Catches accidental drift from the documented PRD §4.8 formula even when goldens are silently re-baselined.

**Regression value:** Any future change to interpolation math, integer-division rounding, or endpoint clamping breaks the golden diff and the test must be re-baselined consciously. Locks down the formula from PRD §4.8.

**CI rigor added:** `replay` job.

**Exit gate:** unit + replay green.

---

### Stage 3 — IIR filter + sensor pipeline
**Deliverable:** `core/thermal_filter.c` (Q16.16 IIR), and the sensor-side of the input-snapshot processing in `core/thermal_core.c`. Builds the bridge between `thermal_input_snapshot_t.samples[]` and per-sensor filtered values.

**Tests added:**
- **Unit:** step response, impulse response, alpha=0 (passthrough), alpha=Q16_ONE (no change), Q16.16 saturation under extreme values, `valid=0` propagation (filter holds last good value or marks invalid per spec).
- **Replay:** a noisy-sensor CSV (1000 samples with simulated jitter) → filtered CSV golden.
- **Reference cross-check:** a `scipy.signal.lfilter` first-order IIR reference computes the same outputs on the same input; the C output must match within a Q16.16 rounding tolerance. Same pattern as the curve cross-check — defends against silent drift.

**Regression value:** Filter math is small but easy to break with off-by-one shifts or accidental int promotion. Golden replay catches that immediately; the scipy cross-check catches drift goldens would happily accept.

**Exit gate:** unit + replay green.

---

### Stage 4 — Zone aggregation + step-wise governor
**Deliverable:** `core/thermal_zone.c` (sensor aggregation: max, avg, weighted), `core/thermal_governor.c` (step-wise governor with hysteresis), trip-point evaluation. Active-trip-mask computation. PRD §4.7 partial-validity rules.

**Tests added:**
- **Unit:** aggregation modes including partial-invalid sensors, weighted with edge weights (one weight zero, one weight dominant), trip enter/exit with hysteresis, multiple-active-trip "highest state wins" logic.
- **Replay:** a heat-soak ramp CSV (45°C → 90°C → 45°C over 600 ticks) through a multi-trip zone; full output frame captured to golden.
- **Property (new layer):** generated configs with 1–8 sensors, randomized trip points, run through validator; must never crash or return undocumented status.

**Regression value:** The step-wise governor is the simplest reference behavior; once locked, it's the cross-check for PID and modifier work that follows.

**Exit gate:** unit + replay + property green.

---

### Stage 5 — PID governor (Q16.16)
**Deliverable:** `core/thermal_pid.c`. Anti-windup per PRD §4.8. dt clamping. Derivative on measurement, optional first-order filter. Saturation telemetry on overflow.

**Tests added:**
- **Unit:** step response, settling time, anti-windup under sustained saturation, dt clamp on missing tick, gain-change resets integral+derivative (per `CMD_SET_PID` contract), PID-trip-floor interaction (critical trip floors output via `state_pwm[cooling_state]`).
- **Replay:** step-load CSV (50°C → 85°C step), full PID-term telemetry captured. Becomes the golden for tuning regressions.
- **Cross-check unit test:** a scipy-based reference PID computes the same outputs in Python; the test compares within a small tolerance.

**Regression value:** PID is the most-tuned single module. Golden + scipy cross-check catches both math regressions and accidental numerical drift.

**Exit gate:** unit + replay + property + scipy cross-check green.

---

### Stage 6 — Fault detectors (all four)
**Deliverable:** `core/thermal_fault.c` — stall, stuck-sensor, runaway, stale-context detectors. State machines per PRD §4.7. `THERMAL_FAULT_ACTION_*` handlers. Spin-up grace window. Latching + `CMD_CLEAR_FAULT` (the command path lands in Stage 11; here only the state machine).

**Tests added:**
- **Unit per detector:** entry conditions, persist_ticks behavior, recovery_ticks behavior, LATCHED requires explicit clear, stuck-sensor advisory mode without correlated context.
- **Replay:** each fault scenario as a CSV input + golden output: stall raise + recover, stuck sensor with correlated load, runaway under high-PWM rising-temp window.
- **Regression hook:** the stall scenario includes the spin-up window — catches future regressions where someone "fixes" stall logic and breaks spin-up grace.

**CI rigor added:** ASan/UBSan join CI here. From this stage on, sanitizer-clean is required for merge.

**Exit gate:** unit + replay + property + asan green.

---

### Stage 7 — Acoustic modifier + arbitration + slew + end-to-end step
**Deliverable:** `core/thermal_modifier.c` (acoustic_mask, pre + post stages), `core/thermal_arbitrator.c` (max-wins), slew-rate limiter, `core/thermal_core.c` wiring per the control-loop diagram in PRD §4.6. `thermal_core_step()` is now a complete end-to-end function.

**Tests added:**
- **Unit:** modifier pre-stage trip offset, modifier post-stage pwm-cap, modifier active-flag semantics, stale-context fail_safe behavior, arbitrator under three-zone-one-actuator load, slew limiter bypass for upward safety overrides.
- **Replay:** the full closed loop on host stub — a 60-second snapshot stream driving all modules. Massive golden. This is the canonical "did anything change in the loop" canary.

**CI rigor added:** clang-tidy on `core/` only.

**Exit gate:** unit + replay + property + asan + clang-tidy green.

---

### Stage 8 — Runtime command + state inspection
**Deliverable:** `thermal_core_apply_command()` for all five v1 commands (`SET_PID`, `SET_SETPOINT`, `SET_TRIP`, `SET_CURVE_POINT`, `CLEAR_FAULT`). `thermal_core_get_state()`. PID-integrator reset on gain change. Curve-edit monotonicity check per PRD §5.3.

**Tests added:**
- **Unit:** each command, valid + each error path; bounds enforcement; monotonicity rejection on curve edit; LATCHED clear gating; `TEVENT_COMMAND_APPLIED` / `_REJECTED` emission with the right `now_ms`.
- **Replay:** a "step response with mid-experiment kp change" sequence. Captures both the gain-change ack event and the resulting PWM response.
- **Property:** randomly generated commands (any payload, any value) through `apply_command()` — must always return a documented status, never crash.

**CI rigor added:** cppcheck.

**Exit gate:** unit + replay + property + asan + clang-tidy + cppcheck green.

---

### Stage 9 — Linux daemon: `bsp_mock_tmpfs` + JSON loader + telemetry UDP + probe
**Deliverable:** `platform/linux/thermalcored.c`, `bsp_mock_tmpfs.c`, `bsp_telemetry_udp.c`, `config_jsmn.c`. The daemon now boots from a JSON file, polls a tmpfs hwmon mock, runs `thermal_core_step()` every 100 ms, and emits telemetry UDP frames. `tools/thermalcore-probe` (Python) reads UDP and writes CSV.

**Tests added:**
- **Smoke (new test type):** daemon starts with `configs/minimal-1zone-1fan.json`, runs for 2 seconds, exits cleanly on SIGTERM, produced at least one telemetry frame on UDP.
- **JSON loader unit tests:** valid configs accepted; each documented invalid case rejected with the right status code and a clear error message.
- **Fuzz (new test type):** libFuzzer over the JSON loader, seed corpus = `configs/*.json`. 60 s/PR.
- **Daemon-level replay:** write a known sequence of temp values to the tmpfs mock, capture the telemetry UDP stream, diff against golden.
- **Probe parity:** the same UDP stream replayed through `thermalcore-probe --log` produces the same CSV every time.

**CI rigor added:** smoke-linux, coverage (visibility only, no gate), fuzz-json.

**Exit gate:** unit + replay + property + asan + clang-tidy + cppcheck + smoke + fuzz-json green.

---

### Stage 10 — Control plane: command listener + `thermalcore-tune` + wire codec
**Deliverable:** UDP command listener on the daemon (`127.0.0.1:9002`), the wire encode/decode helpers in `core/thermal_commands.c` (CRC-16/CCITT-FALSE, packed encoding per PRD §7.2), `tools/thermalcore-tune` CLI.

**Tests added:**
- **Unit:** wire encoder/decoder round-trip for every opcode + command_id; CRC validation; sequence-number tracking; payload-cap rejection.
- **Daemon-level:** `thermalcore-tune set-pid soc 5000 400 0` followed by a state-inspection request produces a state snapshot with the new gains. ACK frame received with matching seq.
- **Fuzz:** libFuzzer over the wire decoder, seed corpus = recorded valid frames.

**CI rigor added:** fuzz-wire.

**Exit gate:** all previous + fuzz-wire green.

---

### Stage 11 — SocketCAN + OBD-II + `car-can-emulator` integration
**Deliverable:** `bsp_socketcan.c`, OBD-II PID 0x0D decode. `car-can-emulator` integrated as a git submodule and built by CI on `vcan0`. The acoustic_mask modifier is now driven by real CAN traffic in the test rig.

**Tests added:**
- **Unit:** OBD-II frame encode/decode, response timeout handling, fail_safe fallback after `timeout_ms`.
- **Integration:** CI starts `car-can-emulator` on `vcan0`, starts `thermalcored`, sets speed via the emulator's TCP control port at `t=0, 5, 10, 15 s`, asserts the daemon's telemetry stream shows the expected speed values within the configured filter time constant.
- **Replay:** a CAN bus-loss scenario (emulator killed mid-run) producing `assume_stationary` fallback.

**Exit gate:** all previous + CAN integration test green.

---

### Stage 12 — Scenario runner + thermal-plant simulator + canonical scenarios
**Deliverable:** `tools/thermalcore-scenario` (Python), the deterministic plant simulator in Q16.16 (shared library between the scenario runner and replay tests), the assertion grammar from PRD §7.6. All ten canonical scenarios from PRD §9.1 implemented.

**Tests added:**
- **Scenario (new test type):** every canonical `.scn` file runs in CI, assertions decide pass/fail.
- **Determinism (new test type):** every scenario runs twice; SHA-256 of the telemetry CSV must match. Then the gcc-built daemon and the clang-built daemon must produce identical SHA-256.
- **Simulator unit tests:** plant math (heat in, cooling gain, coupling), deterministic PRNG, zone-to-zone coupling.

**CI rigor added:** scenario, determinism.

**Exit gate:** all previous + scenario + determinism green. **This is the v1 Linux release-gate.**

---

### Stage 13 — ESP32 STANDALONE build
**Deliverable:** `platform/esp32_idf/main/*` with thin BSP wrappers around LEDC, PCNT, 1-Wire, I2C, TWAI. `app_main()` ≤ 100 lines. Static `thermal_config_t` generated by `tools/json2static.py` from the same JSON the Linux daemon uses. ESP32 standalone build runs the full thermal-core in a FreeRTOS task.

**Tests added:**
- **Build matrix:** `idf.py build` succeeds in CI under the ESP-IDF docker image. Size-budget assertions: `.text <= 64 KB`, `.bss <= 16 KB`.
- **Cross-platform unit replay:** the same `test/unit/` binary cross-compiled for `qemu-xtensa` (or ESP32-S3 simulator) — replay tests must produce identical CSVs to the host build. Catches any host-only assumption that snuck into `core/`.
- **Manual:** ESP32 flashed on the bench, runs `idle_steady_state` scenario, observed via probe over USB-CDC. Not in PR-gating CI.

**CI rigor added:** build-esp32.

**Exit gate:** all previous + build-esp32 + cross-platform unit replay green.

---

### Stage 14 — ESP32 HIL_PERIPHERAL build
**Deliverable:** Same firmware compiled in HIL mode (per PRD §8.3): ESP32 reports tach/temp/CAN to Linux over USB-CDC as `TELEM_SAMPLE` frames; Linux runs the thermal-core; Linux sends actuator commands back as `CMD_REQUEST` frames with platform-private command IDs.

**Tests added:**
- **Build:** HIL-mode firmware builds with size budgets met.
- **Integration (manual):** ESP32 + Linux on bench. Same scenarios run, same assertions. The HIL telemetry should match the standalone telemetry within the slew-rate envelope (latency-induced delta, not behavior delta).

**Exit gate:** HIL build green; manual bench run succeeds.

---

### Stage 15 — Cross-platform parity + benchmarks
**Deliverable:** Three-rig parity confirmed: pure host simulator, Linux + ESP32-HIL, ESP32 standalone. All ten canonical scenarios produce telemetry whose SHA-256 matches across rigs (within the documented tolerance for HIL transport latency). Bench-rig benchmark table from PRD §9.2 captured.

**Heads-up on effort budget.** Cross-platform parity is the single most valuable test in the plan and the hardest to set up correctly. HIL transport latency, tach-jitter normalization, and CAN-frame timing variance all need to be characterized before a tolerance band can be set; expect to spend more time tuning the parity check than building any single earlier stage. When it works, it eliminates 90% of "works on Linux, breaks on ESP32" risk forever. Worth the extra effort — but plan for it explicitly rather than assuming the test falls out for free.

**Tests added:**
- **Cross-platform parity (new gating test):** runs a deterministic scenario through all three rigs and asserts byte-identical telemetry where appropriate (host vs ESP32 standalone must match exactly; HIL has a known latency-induced tolerance).
- **Benchmark capture:** `make benchmarks` runs scenarios with timing instrumentation; emits a markdown table that gets included in the white paper.
- **Memory-footprint check:** size of the ESP32 binary is tracked over time as a CI artifact; trend visible in PR comments.

**CI rigor added:** cross-platform-parity.

**Exit gate:** all previous + cross-platform-parity green.

---

### Stage 16 — White paper integration
**Deliverable:** Figure regeneration pipeline. `make -C docs/paper figures` runs the canonical scenarios, regenerates every matplotlib plot from fresh telemetry CSVs, rebuilds the PDF. Bench-rig photo, BOM table, scenario plots, benchmark tables.

**Tests added:**
- **Paper build:** `make -C docs/paper` produces a PDF without manual intervention. Job in `release.yml`, not blocking PR merges.
- **Figure freshness:** every figure caption embeds the git SHA used to generate the underlying data. CI verifies these SHAs match the current commit (catches a stale figure being committed).

**Exit gate:** `release.yml` produces the PDF; SHAs in figure captions match.

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
| 2 | Curve interpolation | replay | curve goldens stable |
| 3 | IIR + sensor pipeline | — | filter goldens stable |
| 4 | Zone agg + step-wise governor | property | per-zone replay green |
| 5 | PID (Q16.16) | — | scipy cross-check green |
| 6 | Fault detectors | asan-ubsan | per-detector replay green |
| 7 | Modifier + arbitration + slew + full loop | clang-tidy | end-to-end replay golden |
| 8 | apply_command + get_state | cppcheck | command tests green |
| 9 | Linux daemon + JSON + telemetry UDP | smoke, coverage, fuzz-json | daemon smoke green |
| 10 | Control plane + tune CLI + wire codec | fuzz-wire | round-trip + ack green |
| 11 | SocketCAN + OBD-II + emulator | — | vcan0 integration green |
| 12 | Scenario runner + simulator + 10 scenarios | scenario, determinism | all canonical scenarios green |
| 13 | ESP32 STANDALONE | build-esp32 | cross-platform unit replay green |
| 14 | ESP32 HIL_PERIPHERAL | — | HIL build + manual bench run |
| 15 | Cross-platform parity + benchmarks | cross-platform-parity | three-rig parity green |
| 16 | White paper figures | (release workflow) | PDF builds + SHAs verified |

---

## 9. Open decisions for later

Items deliberately deferred from this plan; resolve when the relevant stage starts:

- **Coverage gate threshold.** Stage 9 introduces lcov as visibility only. A future decision: should coverage become a gate (e.g., new code must keep total coverage ≥ 80%)? Defer until coverage baseline is established.
- **ESP32 emulator in CI.** Stage 13 cross-compiles for ESP32 but doesn't run the binary. Once `qemu-xtensa` or the ESP-IDF QEMU image is stable enough, the cross-platform unit replay can move to a real instruction-accurate run in CI. Defer until needed.
- **Self-hosted runner for HW-in-CI.** v1 keeps bench scenarios manual. If bench failures start landing in `main` repeatedly, promote the bench Pi 4 to a self-hosted runner gated on a `[hil]` PR label.
- **Property test budget.** Stage 4 introduces property testing with a 100-case budget per PR. If shrinkage costs become noticeable, drop to 20 cases per PR + 1000 cases nightly.

---

*End of implementation plan v0.2*
