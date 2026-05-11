# thermal-core — Product Requirements Document

**Project/repo name:** `thermal-core`
**Linux daemon binary:** `thermalcored`
**Repository (planned):** `github.com/hackboxguy/thermal-core`
**Document status:** Draft v0.5
**Author:** Albert David
**License (code):** MIT  **License (paper/doc):** CC-BY-4.0

> **Naming decision.** `thermal-engine` was the original working name, but Qualcomm ships a daemon by the same name in many Android/AOSP automotive trees. To avoid Google-search collision and confusion in automotive contexts, this PRD uses `thermal-core` for the repo/library concept and `thermalcored` for the Linux daemon binary.

---

## 1. Overview

`thermal-core` is a portable, configurable thermal-control core with an automotive In-Vehicle Infotainment (IVI) reference application. The reference application implements closed-loop fan control across multiple temperature zones with the deliberate objective of trading off two competing goals: **sufficient cooling** to keep silicon within its thermal envelope, and **low acoustic emissions** in the cabin, especially when the vehicle is stationary and ambient noise is low.

The core innovation is not the control algorithm itself — PID and step-wise governors are well-understood — but the **architectural separation** that lets one body of policy code run unmodified across:

- Linux userspace (development host, simulation, hardware-in-the-loop bench)
- ESP-IDF / FreeRTOS on ESP32 (reference embedded implementation and bench rig peripheral concentrator)
- Renesas RH850-F1KM under FreeRTOS (future automotive target)

This is supported by a snapshot-driven core API, small platform callback surface, static memory allocation, fixed-point math, and numeric event logging — discipline imposed up front so the Linux experience does not produce code that fails to port.

The deliverable is **both** a working open-source codebase and a **LaTeX-typeset white paper** that documents the architecture, the acoustic-thermal tradeoff, and the bench validation. The white paper is grounded in working code: every figure regenerates from raw bench data, every benchmark cites a specific git SHA, and every claim is reproducible from the repository.

## 2. Motivation

Automotive IVI head units increasingly host high-TDP SoCs (Qualcomm SA8255P / SA8295P / SA8775P, Samsung Exynos Auto V920) alongside thermally significant peripherals: audio amplifiers, tuners, and the head unit's own board temperature. These systems must operate across automotive ambient ranges, often without natural airflow when the vehicle is stationary. Fan-based active cooling is increasingly common.

But fan noise inside a quiet cabin is one of the most consistent customer-perceived quality issues in modern IVI. A fan that is acoustically invisible at 80 km/h is loud and annoying at a red light. Reactive thermal control that ignores cabin context produces exactly this outcome.

Existing open-source thermal daemons (`thermald`, `fancontrol`, `fan2go`, `nbfc-linux`) target desktops and laptops. None integrates vehicle-context signals (speed, ignition state, drive mode). None is structured for an MCU port. The Linux kernel thermal framework provides good abstractions but a Linux-only consumer.

`thermal-core` is an attempt to fill that gap with a deliberately small, deliberately portable, deliberately reproducible reference implementation.

### 2.1 Product boundary

The intended product is the generic control core plus the reference platform integrations around it. The automotive IVI fan controller is the first proof point, not a hard-coded assumption inside the core.

The core owns:

- zone state, trip evaluation, governors, filters, curves, arbitration, fault detection, and numeric telemetry IDs
- deterministic stepping over typed input snapshots and actuator output frames
- fixed-size configuration structures that can be populated from JSON on Linux or generated C on MCU targets

The platform layer owns:

- Linux `hwmon`, SocketCAN, files, UDP, serial, systemd, and CLI handling
- ESP-IDF / FreeRTOS drivers such as LEDC, PCNT, TWAI, 1-Wire, I2C, USB-CDC, and NVS
- future RH850-F1KM CAN, PWM, ADC, timer, and RTOS binding

Domain-specific context, such as vehicle speed from OBD-II, enters the core only as a typed context signal in the input snapshot. The core should never parse CAN frames, know OBD-II request IDs, open sysfs files, allocate JSON objects, block on hardware, or depend on any vehicle-specific protocol.

## 3. Goals and Non-Goals

### 3.1 Goals (v1)

- One C99 core that runs unmodified on Linux, ESP32 (ESP-IDF / FreeRTOS), and (by structural readiness) RH850-F1KM under FreeRTOS.
- Configurable multi-zone thermal control: 2–4 temperature zones, 1–2 PWM-controlled fans, step-wise and PID governors selectable per zone.
- Vehicle-speed-aware acoustic policy: PWM cap and trip-point offset as functions of speed.
- A generic context-input mechanism so vehicle speed is one policy input, not a core dependency.
- A stable public C API: config validation, init, deterministic step, runtime command application, state inspection, and telemetry/log callbacks.
- Fault detection: fan stall, sensor stuck-at, thermal runaway.
- Runtime observability: telemetry stream of internal state for live plotting and offline analysis.
- Runtime tuneability: PID gains, trip points, curves modifiable without rebuild.
- Reproducible reference bench rig: pinned BOM, documented wiring, scripted scenarios.
- White paper documenting architecture, acoustic-thermal model, and bench results.

### 3.2 Non-goals (v1)

- ASIL-rated implementation; this is a concept, not a safety-certified component.
- AUTOSAR Classic or Adaptive integration.
- DLT (Diagnostic Log and Trace) logging integration (numeric event log only).
- PID autotune (Ziegler-Nichols, relay feedback, etc.).
- NVS-persisted learned PID gains.
- Full RH850-F1KM port (BSP scaffolding only; full port is v2).
- Multi-governor mixing within one zone.
- Runtime configuration file reload (signal-driven).
- OEM-specific CAN DBC integration beyond a single OBD-II PID.
- Direct coupling to the `car-can-emulator` TCP control interface from the core; emulator control belongs in scenario tooling.

## 4. Architecture

### 4.1 Three-layer split

```
+-----------------------------------------------------------+
| Platform shim (Linux daemon / ESP-IDF app / RH850 task)   |
|   - main(), task creation, signal handling, CLI args      |
|   - config loading (JSON on Linux, static struct on MCU)  |
|   - telemetry sink, control-plane endpoint                |
+-----------------------------------------------------------+
| BSP backends (sensor_backend, actuator_backend)           |
|   - hwmon_sysfs, mock_tmpfs, serial_esp32, esp_native,    |
|     rh850_native (future)                                 |
+-----------------------------------------------------------+
| Core (thermal_core.c, governors, faults, curves, PID)     |
|   - Pure C99, no platform dependencies                    |
|   - Static allocation, fixed-point, numeric logging       |
|   - Identical bytes across all targets                    |
+-----------------------------------------------------------+
```

The core depends only on a configuration struct (`thermal_config_t`), caller-provided input snapshots, caller-owned output frames, and an optional callback surface for numeric logs and telemetry. It does not call any hardware, OS, file, CAN, serial, or actuator function. It does not allocate after init. It does not use floating point except optionally in the PID block (controlled by a build flag; default is Q16.16 fixed-point).

### 4.2 Discipline constraints (apply to core code)

| Constraint | Rationale |
|---|---|
| No `malloc`/`free`/`calloc` after init | RH850-F1KM has no heap; static allocation avoids fragmentation and makes worst-case memory footprint exact. |
| No floating point in hot path (PID is opt-in float) | Avoid reliance on FPU; Q16.16 is exact across platforms. |
| No strings in hot path | Strings are interned to numeric IDs at config-load; logs carry numeric event codes + up to 4 uint32 args. |
| No syscalls or blocking I/O in core | The platform builds snapshots and applies output frames outside the core. |
| Single control thread | Same model on Linux (one process) and MCU (one FreeRTOS task). |
| Compile-time max-size limits | `THERMAL_MAX_ZONES`, `THERMAL_MAX_SENSORS`, etc. Configurable, but fixed at build. |
| `-Wall -Wextra -Werror -std=c99 -pedantic` clean | Portability hygiene. |

### 4.3 Public core API and snapshot model

The platform layer owns all blocking I/O. It polls sensors, tach inputs, CAN signals, files, serial streams, and test injectors, then passes a bounded snapshot into the core. The core consumes that snapshot synchronously and returns actuator requests plus optional telemetry/log callbacks. This keeps Linux, ESP32, and future RH850 timing behavior aligned.

```c
typedef enum {
    THERMAL_SAMPLE_TEMP_MC,
    THERMAL_SAMPLE_TACH_RPM,
    THERMAL_SAMPLE_CONTEXT_I32
} thermal_sample_kind_t;

typedef struct {
    uint16_t id;          /* sensor, actuator tach, or context signal ID */
    uint8_t  kind;        /* thermal_sample_kind_t */
    uint8_t  valid;       /* 0 = absent/stale/faulted */
    int32_t  value;       /* units determined by kind/config */
    uint32_t sample_ts_ms;
    uint16_t quality;     /* platform-defined, 0 = unknown/normal */
} thermal_sample_t;

typedef struct {
    uint32_t now_ms;
    const thermal_sample_t *samples;
    uint8_t sample_count;
} thermal_input_snapshot_t;

typedef struct {
    uint8_t actuator_id;
    uint8_t duty_0_255;
    uint16_t reason_code; /* governor, policy, fault, or manual command */
} thermal_actuator_cmd_t;

typedef struct {
    thermal_actuator_cmd_t actuator_cmds[THERMAL_MAX_ACTUATORS];
    uint8_t actuator_cmd_count;
} thermal_output_frame_t;
```

The v1 public API:

```c
thermal_status_t thermal_core_validate_config(const thermal_config_t *cfg);
thermal_status_t thermal_core_init(thermal_core_t *ctx,
                                   const thermal_config_t *cfg,
                                   const thermal_core_callbacks_t *cb);
thermal_status_t thermal_core_step(thermal_core_t *ctx,
                                   const thermal_input_snapshot_t *in,
                                   thermal_output_frame_t *out);
thermal_status_t thermal_core_apply_command(thermal_core_t *ctx,
                                            const thermal_command_t *cmd,
                                            thermal_command_result_t *result);
thermal_status_t thermal_core_get_state(const thermal_core_t *ctx,
                                        thermal_state_snapshot_t *state);
```

Allocation and lifetime contract:

- `thermal_core_t` is a caller-owned, fixed-size public struct declared in `core/thermal_types.h`. Callers may allocate it statically, globally, or on the stack; the core does not allocate it internally.
- `sizeof(thermal_core_t)` depends on compile-time maxima such as `THERMAL_MAX_ZONES` and `THERMAL_MAX_ACTUATORS`. v1 promises a stable source API, not a stable binary ABI across builds with different maxima.
- `thermal_core_init()` fully initializes caller-provided storage; callers do not need to pre-zero the context. The `thermal_config_t` passed to init must remain alive and immutable for the lifetime of the context. Runtime commands update the core's runtime shadow state, not the const config.
- The callback struct is copied by value during init. The function pointers it contains must remain callable while the context is active.
- `thermal_input_snapshot_t` and its `samples` array are borrowed only for the duration of `thermal_core_step()`. The core may copy sample values into internal state, but it must not retain pointers to caller-owned snapshot memory.
- On success, `thermal_core_step()` writes a full actuator output frame every tick: one command for each configured actuator, even when the duty cycle has not changed. Changed-only frames are not part of the v1 contract.
- A single `thermal_core_t` is not reentrant. Platforms use one control thread/task per context; independent contexts may run independently if the platform owns synchronization around shared I/O.

`thermal_core_step()` must not call sensor, CAN, file, serial, or actuator drivers. It performs bounded work over the provided snapshot. Missing or stale samples are represented explicitly with `valid = 0` and are handled by configured fail-safe behavior.

### 4.4 Platform callback surface

```c
typedef struct {
    /* Logging — numeric event codes, no strings */
    void     (*log_event)(uint16_t code, uint32_t a1, uint32_t a2,
                          uint32_t a3, uint32_t a4);

    /* Telemetry emit (optional, may be NULL) */
    void     (*telemetry_emit)(uint32_t ts_ms, uint16_t signal_id,
                               int32_t value);
} thermal_core_callbacks_t;
```

Persistence, sleeping, scheduling, NVS, JSON parsing, SocketCAN, sysfs, ESP-IDF drivers, and actuator writes remain platform responsibilities. If a platform wants persisted runtime-tuned values, it stores command deltas outside the core and reapplies them through `thermal_core_apply_command()` at startup.

### 4.5 Data model

Borrowed in spirit from the Linux kernel thermal framework:

- **Sensor**: a source of temperature samples in millidegrees C. Has an ID, a name (debug only), an IIR filter coefficient, and a stuck-sensor detector.
- **Zone**: a logical thermal region (e.g., "soc", "amp", "tuner"). Has 1+ sensors (aggregated max/avg/weighted), 1+ trip points, an active governor, and a current state.
- **Trip point**: a temperature threshold with a hysteresis band, a severity, and a target cooling state.
- **Actuator**: a cooling device. v1 = PWM fan with optional tach feedback. Has min/max PWM, slew-rate limit, stall detector.
- **Governor**: a control algorithm. v1 = `step_wise` (Linux-thermal style discrete states) and `pid` (continuous PID with anti-windup).
- **Context signal**: a typed non-temperature input, such as vehicle speed, ignition state, drive mode, HVAC state, or ambient-noise proxy. v1 uses vehicle speed only. Context IDs are configured; the core never knows where the value came from.
- **Policy modifier**: a configured rule that can transform either pre-governor effective targets/trips or post-governor actuator requests based on context. v1 = `acoustic_mask` with a pre-governor trip/setpoint offset and a post-governor PWM cap as functions of vehicle speed.
- **Arbitrator**: when multiple zones target the same actuator, the arbitrator decides the final command. v1 = max-wins (highest demanded PWM).
- **Fault detector**: stall, sensor-stuck, runaway, stale context. Per detector, configurable thresholds, severity, latching/recovery behavior, and action.

### 4.6 Control loop

```
tick (every CONTROL_PERIOD_MS, default 100ms):
    platform:
      1. poll or reuse cached sensor/tach/context samples
      2. build thermal_input_snapshot_t with timestamp and validity
    core:
      3. apply IIR filters → update zone temperatures
      4. update context filters and context fail-safe state
      5. apply pre-governor modifiers (effective trips/setpoints)
      6. for each zone: evaluate trips, run governor → desired PWM
      7. for each actuator: arbitrate across zones (max-wins)
      8. apply post-governor modifiers (PWM cap, policy clamp)
      9. check faults and apply fault actions / safety overrides
     10. apply slew-rate limit and populate thermal_output_frame_t
     11. emit telemetry for changed signals and log state transitions
    platform:
     12. write actuator commands and persist/forward telemetry as needed
```

Safety ordering: acoustic caps and comfort policies may reduce cooling only while the relevant zone is below critical severity and no safety override is active. Critical trips, runaway faults, and configured emergency actions override acoustic caps. Because post-governor policy caps run before fault actions, a fault action such as `force_pwm_max` is allowed to overwrite an already-capped PWM request.

Determinism: the loop is monotonic; `now_ms()` rollover (32-bit ms = ~49 days) is handled with `(uint32_t)(a - b)` subtraction throughout. All loop work is bounded by compile-time maxima.

### 4.7 Fault model

Faults are explicit state machines, not one-shot booleans. Each detector emits a numeric event when it enters or leaves a state.

| State | Meaning | Typical action |
|---|---|---|
| `NORMAL` | No active fault. | Run configured governors and policies. |
| `DEGRADED` | Fault detected, system can continue with reduced confidence. | Use fallback value, force minimum safe cooling, or mark telemetry. |
| `CRITICAL` | Thermal margin or actuator confidence is compromised. | Override acoustic caps and request maximum configured cooling. |
| `LATCHED` | Fault remains active until explicit reset or reboot. | Hold configured fault action and require operator/tool action. |
| `RECOVERING` | Signal has returned to normal but must remain stable for `recovery_ticks`. | Keep conservative output until recovery completes. |

Required v1 detector behavior:

- **Fan stall:** active when requested PWM is above `stall_pwm_threshold` and tach stays below `stall_rpm` for `persist_ticks`, excluding configured spin-up grace. Recovery requires tach above threshold for `recovery_ticks`.
- **Stuck sensor:** active when a valid sensor changes by less than `delta_mc` across `window_ticks` while at least one correlated signal or scenario injection indicates changing thermal load. If no correlated signal is configured, the detector is advisory only: it emits telemetry/events but does not move the detector out of `NORMAL`.
- **Thermal runaway:** active when temperature rises for `persist_ticks` while requested cooling is high or increasing. This fault is `CRITICAL` by default and overrides acoustic caps.
- **Stale context:** active when a context sample is invalid longer than its timeout. The configured context fail-safe value is applied and the policy emits telemetry so tests can prove the fallback occurred.

Fault actions must be idempotent and bounded. A fault action may force an actuator command higher than the governor requested, but v1 must not silently command a lower cooling level during `CRITICAL` or `LATCHED` thermal faults.

Trip severity and fault state are separate vocabularies. Trip severity describes thermal policy for a zone; fault state describes detector confidence and recovery. v1 maps trip severities as follows:

| Trip severity | Core behavior |
|---|---|
| `warn` | Emit telemetry/event and request the configured cooling state. Acoustic caps may still apply. |
| `critical` | Request the configured cooling state, bypass acoustic caps for affected actuators, and emit a safety-override event. |
| `shutdown` | Request maximum configured cooling for affected actuators, latch a shutdown-request condition, and emit `TEVENT_SHUTDOWN_REQUEST`. The core never halts the OS, powers off hardware, or enters an OEM limp-home mode; the platform decides what to do with the request. |

`CMD_CLEAR_FAULT` is the explicit reset path for `LATCHED` v1 faults. It succeeds only when the underlying detector condition is no longer present and the configured recovery criteria are satisfied; otherwise it returns `CMD_NACK` with `rejected_safety_transition`. Production builds may compile out remote `CMD_CLEAR_FAULT`; in that case the reset path is reboot or a platform-local maintenance action.

### 4.8 Governor semantics

The implementation must make governor math reproducible across targets:

- **Step-wise governor:** trip points are evaluated with hysteresis. Each active trip contributes a `cooling_state`; the zone request is the highest active state.
- **PID governor:** error is `zone_temp_mc - effective_setpoint_mc`; positive error increases cooling demand. Output units are PWM duty `0..255` before arbitration and policy modifiers.
- **PID timestep:** `dt` is derived from `thermal_input_snapshot_t.now_ms` and clamped to configured min/max bounds. A missing or extremely late tick emits telemetry and uses the clamped value.
- **Anti-windup:** v1 uses bounded integral state. The integral is not increased further when output is saturated and the error would drive it deeper into saturation.
- **Derivative:** derivative is computed on measured temperature by default, with optional first-order filtering to avoid tach/sensor noise coupling.
- **Fixed-point arithmetic:** Q16.16 is the default representation for gains and PID terms. All multiplies, adds, clamps, and conversions use explicit saturating helpers; overflow is a fault in tests and a logged event in release builds.
- **Step-state mapping:** v1 maps `cooling_state` to PWM through the per-actuator `state_pwm` table in actuator config. There is no zone-local cooling-state curve in v1.
- **Curve interpolation:** all policy and governor curves use deterministic integer linear interpolation between adjacent points, with endpoint clamping outside the configured x-axis range. The formula is `y = y0 + ((int64_t)(x - x0) * (y1 - y0)) / (x1 - x0)`, using C99 signed-division truncation toward zero. Duplicate or descending x-axis points are rejected at config validation. No floating point is used for curve evaluation.

## 5. Configuration

### 5.1 Linux: JSON

Linux daemon loads JSON at startup via `jsmn` (small, no-malloc parser, vendored into repo). Schema (illustrative; full schema in `docs/schema/thermal-core.schema.json`):

```json
{
  "config_version": 1,
  "control_period_ms": 100,
  "sensors": [
    {"id": 0, "name": "soc",     "source": "hwmon:chip=coretemp:input=temp1_input", "iir_alpha_q16": 16384},
    {"id": 1, "name": "amp",     "source": "ds18b20:28-0000abc"},
    {"id": 2, "name": "tuner",   "source": "i2c:1:0x18:mcp9808"},
    {"id": 3, "name": "board",   "source": "hwmon:chip=nct6775:input=temp2_input"},
    {"id": 4, "name": "ambient", "source": "ds18b20:28-0000xyz"}
  ],
  "context_signals": [
    {"id": 0, "name": "vehicle_speed", "unit": "kmh",
     "source": "canbus:obd2:pid_0x0D", "iir_alpha_q16": 2048,
     "timeout_ms": 3000, "fail_safe": "assume_stationary"}
  ],
  "actuators": [
    {"id": 0, "name": "main_fan", "pwm": "hwmon:chip=nct6775:input=pwm1", "tach": "hwmon:chip=nct6775:input=fan1_input",
     "pwm_min": 80, "pwm_max": 255, "slew_per_tick": 8, "stall_rpm": 200,
     "tach_pulses_per_rev": 2, "spinup_pwm": 180, "spinup_ms": 500,
     "state_pwm": [0, 100, 160, 220, 255]}
  ],
  "zones": [
    {"name": "soc", "sensors": ["soc"], "aggregation": "max",
     "governor": "pid", "pid": {"kp_q16": 4915, "ki_q16": 327, "kd_q16": 0, "setpoint_mc": 75000},
     "actuators": ["main_fan"],
     "trips": [
       {"temp_mc": 70000, "hyst_mc": 2000, "severity": "warn", "cooling_state": 1},
       {"temp_mc": 85000, "hyst_mc": 2000, "severity": "critical", "cooling_state": 3},
       {"temp_mc": 95000, "hyst_mc": 2000, "severity": "shutdown", "cooling_state": 4}
     ]},
    {"name": "amp",   "sensors": ["amp"],   "governor": "step_wise", "...": "..."},
    {"name": "tuner", "sensors": ["tuner"], "governor": "step_wise", "...": "..."}
  ],
  "policy_modifiers": [
    {"name": "acoustic_mask", "context": "vehicle_speed",
     "stages": ["pre_governor_trip_offset", "post_governor_pwm_cap"],
     "curve": [
       {"speed_kmh": 0,   "pwm_cap": 120, "trip_offset_mc": 0},
       {"speed_kmh": 30,  "pwm_cap": 180, "trip_offset_mc": 0},
       {"speed_kmh": 80,  "pwm_cap": 255, "trip_offset_mc": -5000},
       {"speed_kmh": 130, "pwm_cap": 255, "trip_offset_mc": -8000}
     ],
     "fail_safe": "assume_stationary"}
  ],
  "fault_detection": {
    "stall": {"persist_ticks": 30, "stall_pwm_threshold": 80, "severity": "degraded", "action": "force_pwm_max_until_recovered"},
    "stuck_sensor": {"window_ticks": 600, "delta_mc": 100, "severity": "degraded", "action": "use_zone_fallback"},
    "runaway": {"persist_ticks": 50, "severity": "critical", "action": "force_pwm_max_and_latch"}
  },
  "telemetry": {
    "enable": true,
    "transport": "udp:127.0.0.1:9001",
    "signals": ["zone_temp_*", "actuator_pwm_*", "actuator_rpm_*", "pid_terms_*", "speed_kmh"]
  },
  "control": {
    "enable": true,
    "listen": "udp:127.0.0.1:9002",
    "scope": "dev_bench_only"
  }
}
```

Linux `hwmon` sources must be resolved by stable chip name, label, and input name where possible. The platform must not require fixed `/sys/class/hwmon/hwmonN` numbering, because `hwmon0`, `hwmon1`, and friends are not stable across boots.

### 5.2 ESP32 and RH850: static `const` struct

Same `thermal_config_t` shape, populated as a static const in C (built into the firmware image). The JSON parser is never linked. Configuration is changed by rebuilding, **or** at runtime via the control-plane (see §7) for parameters within configured limits.

A small Python tool `tools/json2static.py` converts a JSON config into a `static const thermal_config_t` C file, for round-trip consistency between Linux experimentation and MCU deployment.

### 5.3 Configuration validation rules

`thermal_core_validate_config()` and the Linux JSON loader must reject invalid configuration before the control loop starts. Required v1 checks:

- `config_version` is present and supported.
- The Linux loader treats `jsmn` as a tokenizer only; validation is a hand-written, schema-aware walk that enumerates every allowed key for each object type.
- IDs and debug names are unique within their namespaces.
- All references resolve: zones to sensors, zones to actuators, policies to context signals, telemetry selectors to known signal IDs.
- All arrays fit compile-time maxima.
- Units are explicit in field names or schema metadata; core units are millidegrees C, 0-255 PWM duty, RPM, milliseconds, and Q16.16 coefficients.
- Curves are strictly increasing in their x-axis and have at least two points; interpolation uses the deterministic formula in §4.8.
- PWM bounds satisfy `0 <= pwm_min <= pwm_max <= 255`; `state_pwm` has an entry for every referenced `cooling_state`, and each entry is within bounds; spin-up PWM, slew limits, and stall thresholds are actuator-specific.
- Trip points are ordered by temperature and hysteresis does not overlap neighboring trips unless explicitly allowed.
- Runtime tuning bounds are configured for every tunable value; commands outside those bounds return an error and leave state unchanged.
- Unknown fields are rejected in strict mode. Development tools may offer a non-strict warning mode, but release configs use strict validation.
- Future `config_version` migrations must be explicit. v1 tools may reject newer versions with a clear error instead of attempting best-effort parsing.

## 6. Vehicle Speed Integration (OBD-II via CAN)

### 6.1 Source

Speed is queried via OBD-II Service 01, PID `0x0D` (vehicle speed, single byte, km/h, range 0–255). The generic context signal storage may support a wider configured range for non-OBD speed sources, but the OBD-II source itself is capped at 255 km/h. The reference test source is **`car-can-emulator`**, included as a git submodule at `tools/car-can-emulator/` and tracking the upstream `v2-improvements` branch.

The emulator behavior relevant to `thermal-core`:

- Runs on Linux with SocketCAN using a real CAN adapter (`can0`) or virtual CAN (`vcan0`).
- Responds to SAE J1979 Mode 01 requests on `0x7DF` (functional request) and `0x7E0` (ECU request).
- Emits OBD-II responses on `0x7E8`.
- Supports PID `0x0D` for vehicle speed and additional PIDs useful for future context experiments (`rpm`, coolant `temp`, engine `load`, fuel level, battery voltage).
- Provides a TCP control interface on port `8080`; `echo -n "speed 120" | nc 127.0.0.1 8080` sets the simulated speed to 120 km/h.
- Provides a drive-cycle simulation mode (`--simulate`) and packaging examples for systemd, OpenWrt, and Buildroot.

Repository decision: keep `car-can-emulator` as a separate upstream repo and add it under `tools/car-can-emulator/` as a submodule. `thermal-core` consumes only the CAN frames; scenario tooling may use the emulator's TCP port to set speed during tests.

### 6.2 Mechanism

- **ESP32**: uses TWAI (CAN) peripheral. Sends OBD-II request at 1 Hz; decodes `0x7E8` response; publishes filtered speed (km/h) as the configured `vehicle_speed` context sample in `thermal_input_snapshot_t`.
- **Linux**: uses SocketCAN on a configurable interface (`can0`, `vcan0`). Same 1 Hz polling, same decoding. Allows replaying recorded CAN logs via `canplayer` and local testing against `car-can-emulator`; publishes the same context sample shape as ESP32.
- **RH850** (future): native CAN-FD module; same protocol.

### 6.3 Filtering and fail-safe

- Raw speed is filtered by the platform or by the configured context filter with a slow IIR (time constant ~5 s by default). Thermal time constants are tens of seconds; aggressive filtering avoids fan jitter from brake taps.
- If no valid response is received for the context signal's configured `timeout_ms` (default 3000 ms), the `acoustic_mask` modifier applies the `fail_safe` mode. Default: `assume_stationary` (most acoustically conservative).
- `fail_safe` is explicit in config; it is **not** silently inferred.

### 6.4 Emulator integration contract

`car-can-emulator` is a test dependency, not a runtime dependency of `thermalcored`. The integration contract is:

1. `thermalcored` and ESP32 firmware request PID `0x0D` on CAN and decode the response.
2. Scenario scripts set simulated vehicle speed through the emulator's TCP command interface.
3. Unit tests validate OBD-II request/response encoding separately from the thermal-control core.
4. HIL runs may use either physical CAN (`can0` + CANable) or `vcan0` for host-only tests.

## 7. Observability and Runtime Tuning

This is a first-class v1 requirement: the bench rig is also a tuning lab. Without it, every gain change requires a rebuild and the closed-loop dynamics are invisible.

### 7.1 Telemetry bus

The core emits `(timestamp, signal_id, value)` tuples through the optional `telemetry_emit()` callback. Signal IDs are numeric, defined once in `core/thermal_signals.h`. Examples:

```
TSIG_ZONE_TEMP_SOC          0x0100
TSIG_ZONE_TEMP_AMP          0x0101
TSIG_ACTUATOR_PWM_MAIN_FAN  0x0200
TSIG_ACTUATOR_RPM_MAIN_FAN  0x0201
TSIG_PID_ERROR_SOC          0x0300
TSIG_PID_INTEGRAL_SOC       0x0301
TSIG_PID_DERIVATIVE_SOC     0x0302
TSIG_SPEED_KMH              0x0400
TSIG_MODIFIER_PWM_CAP       0x0401
TSIG_FAULT_STALL_COUNT      0x0500
```

Signals are opt-in via config — small MCUs can drop high-rate signals if needed.

### 7.2 Binary transport framing

All binary telemetry and command transports use the same little-endian frame header. CSV remains available for simple host logs, but all machine-to-machine transports use this framing. The wire format is packed by definition and is encoded/decoded field-by-field; implementations must not cast raw byte buffers to C structs because padding and alignment are compiler-dependent.

```
u8  magic[2]      /* "TC" */
u8  version       /* v1 = 1 */
u8  opcode
u16 seq
u16 payload_len
u32 ts_ms
u8  payload[payload_len]
u16 crc16         /* set to 0 when disabled; required on serial/CAN */
```

CRC contract:

- Variant: CRC-16/CCITT-FALSE.
- Polynomial: `0x1021`.
- Initial value: `0xFFFF`.
- Reflected input/output: false/false.
- Final XOR: `0x0000`.
- Coverage: all bytes from `magic[0]` through the final payload byte, excluding the trailing `crc16` field itself.
- A zero CRC field means "CRC disabled" only on transports that explicitly allow it. Serial and CAN require CRC; loopback UDP may disable it for bench convenience.

`payload_len` is a 16-bit field for protocol headroom, but v1 implementations must enforce a configured receive cap. The default cap is 1024 bytes on Linux and 256 bytes on MCU transports. Frames above the local cap are rejected before payload allocation or command dispatch.

Required opcodes:

| Opcode | Direction | Payload |
|---|---|---|
| `TELEM_SAMPLE` | device → host | `u16 signal_id, u16 flags, i32 value` |
| `TELEM_EVENT` | device → host | `u16 event_code, u32 a1, u32 a2, u32 a3, u32 a4` |
| `CMD_REQUEST` | host → device | command-specific payload |
| `CMD_ACK` | device → host | `u16 request_seq, u16 status, u32 reason_code` |
| `CMD_NACK` | device → host | `u16 request_seq, u16 status, u32 reason_code` |

Every command receives an ACK or NACK. Invalid opcode, invalid payload length, out-of-bounds value, unavailable target, and rejected safety transition are distinct status codes. `seq` is monotonically incremented by the sender and wraps modulo 65536. ACK matching is against the outstanding `(transport, seq)` pair; host tools keep a small outstanding window and expire old requests by timeout so wraparound is unambiguous in normal use.

### 7.3 Per-platform transport

- **Linux**: telemetry UDP packets to `127.0.0.1:9001` (configurable) using the `TC` binary frame. The command listener is separate and disabled unless `control.enable = true`; when enabled for v1 it binds only to loopback, default `127.0.0.1:9002`. CSV to stdout/file is available for simple logs.
- **ESP32**: USB-CDC binary frame stream by default; optionally UDP-over-WiFi for untethered tests.
- **RH850 (future)**: UART or CAN with same frame format.

### 7.4 Host-side tool: `thermalcore-probe`

A single Python script that:

- Reads telemetry from UDP, serial, or file (auto-detected by URI).
- Logs to CSV (`probe --log run.csv`).
- Plots live (`probe --live` — matplotlib, signal-selectable from CLI).
- Generates white-paper figures from logs (`probe --plot scenario_heatsoak.csv --out fig.pdf`).

Same tool, same plots, regardless of source. This is the figure-generation backbone for the white paper.

### 7.5 Control plane (runtime tuning)

Bidirectional: telemetry frames go host-ward, command frames go device-ward. Same framing, distinguished by the opcode field. On Linux v1, command ingress is loopback-only, unauthenticated, and intended for development/bench use. It must not bind to a non-loopback address unless a deliberate unsafe-development flag is set. Production packaging may compile out the command listener entirely.

Core commands:

```
CMD_SET_PID         (zone_id, kp_q16, ki_q16, kd_q16)
CMD_SET_SETPOINT    (zone_id, mc)
CMD_SET_TRIP        (zone_id, trip_idx, mc, hyst_mc)
CMD_SET_CURVE_POINT (modifier_id, point_idx, x, y)
CMD_CLEAR_FAULT     (fault_type, target_id)
```

A companion CLI tool `thermalcore-tune` issues commands. This is what makes the bench rig usable for actual loop tuning: change `kp`, see step response in `--live` plot, change again, in seconds. No rebuild, no reflash.

Commands respect compile-time bounds — `kp` cannot be set outside `[KP_MIN, KP_MAX]` defined in the config. This is intentional; runtime tuning is for the bench, not for the field, and bound-checking is cheap insurance. `CMD_CLEAR_FAULT` follows the latching contract in §4.7: it clears a latched fault only after the detector condition has ended and recovery criteria have passed.

Platform/test commands are separate from core commands:

```
TEST_INJECT_FAULT   (fault_type, target_id, on/off)
TEST_FREEZE_INPUT   (sample_id, kind, fixed_value)
TEST_RESUME_INPUT   (sample_id, kind)
TEST_SET_CONTEXT    (context_id, value)
```

The Linux daemon and ESP32 HIL firmware may implement these test commands. Production builds may compile them out.

### 7.6 Scenario scripting

A small text format (`scenarios/heatsoak.scn`) lists timed commands:

```
0      freeze_input soc 45000
0      freeze_input amp 40000
0      set_setpoint soc 75000
30000  freeze_input soc 90000     # heat ramp at 30s
60000  freeze_input soc 70000     # cool down at 60s
90000  inject_fault stall main_fan on
120000 inject_fault stall main_fan off
```

A scenario runner (`thermalcore-scenario run heatsoak.scn`) issues commands at the specified offsets while `thermalcore-probe --log` captures the response. For vehicle-speed scenarios, the runner can also drive `car-can-emulator` over TCP (`speed <kmh>`) while `thermal-core` observes only CAN. This is how white-paper benchmarks are generated reproducibly.

Scenarios include assertions so CI can return pass/fail without manual plot inspection:

```
assert within 3000 fault_active stall main_fan
assert max actuator_pwm main_fan <= 120 between 0 60000
assert eventually zone_temp soc < 80000 within 120000
assert no_faults except stall
```

The scenario runner stores the command log, telemetry CSV, assertion result, git SHA, config hash, platform target, and tool versions beside each benchmark artifact.

## 8. Reference Bench Rig (pinned)

The bench rig is part of the deliverable. Anyone with the BOM and the wiring diagram in `docs/bench-rig.md` should be able to reproduce the results in the white paper.

v1 does not require a physical heat-injection plant to be considered complete. The release-gating plant is the deterministic host simulator described in §9.3 plus real fan PWM/tach validation on the bench. The resistor + MOSFET heater is retained as an optional validation aid and a useful white-paper photograph/demo if time permits.

### 8.1 Bill of materials

| Item | Qty | Notes |
|---|---|---|
| ESP32-C6 DevKitC-1 | 1 | Primary MCU; supports TWAI (CAN) for OBD-II, plenty of LEDC/PCNT/GPIO |
| Noctua NF-A8 PWM (80 mm, 4-pin) | 1 | Reference low-noise fan; published PWM/RPM curve used for model validation |
| Arctic P8 PWM (80 mm, 4-pin) | 2 | Cheaper test/abuse fans for fault injection |
| 4-pin fan extension cables | 5 | Cut and use as wiring pigtails |
| DS18B20 waterproof 1-Wire probes | 5 | SoC, amp, tuner, board, ambient |
| 4.7 kΩ resistor | 1 | 1-Wire bus pull-up |
| 12V 2A barrel-jack PSU + screw-terminal breakout | 1 | Fan power; ground tied to ESP32 GND |
| 10 Ω 10 W wirewound resistor + N-channel MOSFET | 1 | Optional heater for physical thermal-plant testing |
| Raspberry Pi 4 (4 GB) | 1 | Already owned; OBD-II simulator host running `car-can-emulator` |
| CANable USB-CAN dongle | 1 | Already owned; CAN bus between Pi 4 and ESP32 |
| CAN transceiver breakout (e.g., SN65HVD230) | 1 | ESP32-side CAN PHY |
| Small protoboard / breadboard | 1 | Wiring substrate |

Estimated cost for parts not already owned: ~€65.

### 8.2 Wiring (summary)

Per fan: GND → PSU GND **and** ESP32 GND (common ground is mandatory); +12V → PSU +12V; TACH → ESP32 GPIO with internal pull-up; PWM → ESP32 LEDC output (3.3V logic, accepted by 4-wire fan spec). Per DS18B20: data wire to ESP32 GPIO, 4.7 kΩ pull-up to 3.3V, parasite power disabled. CAN: ESP32 TX/RX to SN65HVD230, CAN_H/CAN_L to bus shared with CANable. Full schematic and photograph in `docs/bench-rig.md` and `docs/img/bench-rig.jpg`.

### 8.3 Reference ESP32 firmware

Sits in `platform/esp32_idf/`. Built with `idf.py build flash monitor`. Minimal use of ESP-IDF idioms: the BSP files are thin wrappers around `ledc_*`, `gpio_*`, `pcnt_*`, `twai_*`, and 1-Wire bit-bang. Application entry is `app_main()` ≤ 100 lines: init platform, init core, create one FreeRTOS task that calls `thermal_core_step()` in a 100 ms loop.

The same firmware supports two modes selectable at build time (`-DTHERMALCORE_MODE=...`):

- **STANDALONE**: ESP32 runs the full thermal-core, including governor. White-paper experiments demonstrating MCU portability run in this mode.
- **HIL_PERIPHERAL**: ESP32 acts as a peripheral concentrator (PWM/tach/temp/CAN over USB-CDC); the Linux daemon runs the thermal-core. White-paper experiments demonstrating Linux-side development and long-running scenarios run in this mode.

## 9. Test Scenarios and Benchmarks

All scenarios are scripted (`scenarios/*.scn`), produce deterministic outputs, and are run on three rigs: pure unit-test sim, Linux-with-mock-tmpfs, and Linux-with-ESP32-HIL plus the standalone ESP32 build for cross-validation.

### 9.1 Canonical scenarios

| Scenario | Description |
|---|---|
| `idle_steady_state` | All zones at ambient; verify no fan activity, no false faults over 5 min. |
| `heat_soak_ramp` | SoC temperature ramps from 45°C to 90°C over 60 s; capture fan response, settling time, overshoot. |
| `step_load` | SoC temperature steps from 50°C to 85°C; capture step response, used for PID tuning. |
| `fan_stall_recovery` | Force fan tach to 0 while PWM > 0; verify stall fault raised within `stall_persist_ticks`, recover when tach restored. |
| `stuck_sensor` | Freeze SoC sensor at 50°C while injecting load; verify stuck-sensor fault, fallback behavior. |
| `acoustic_mask_low_speed` | Vehicle speed = 0; verify PWM cap applied; temperature trends with cap engaged. |
| `acoustic_mask_high_speed` | Vehicle speed sweep 0 → 130 km/h; verify cap releases, trip offset applied. |
| `multi_zone_coupling` | Amp and tuner zones heat simultaneously, shared fan; verify max-wins arbitration. |
| `runaway` | PWM forced to 0 (actuator failure), temperature rising; verify runaway fault. |
| `can_bus_loss` | Cut CAN; verify fail-safe to `assume_stationary` after the configured context `timeout_ms`. |

### 9.2 Benchmarks committed to the white paper

| Benchmark | Target |
|---|---|
| Step time per `thermal_core_step()` call | Linux: sub-100 µs; ESP32-C6: sub-1 ms; budget for RH850-F1KM: sub-500 µs |
| Memory footprint (`.text`/`.bss`/`.data`) | Linux: not bounded (informational); ESP32: ≤ 64 KB text, ≤ 16 KB bss; RH850 budget: ≤ 32 KB text, ≤ 8 KB bss |
| Settling time (`step_load`) | Reported per platform, per governor |
| Overshoot (`step_load`) | Reported per platform, per governor |
| Fan PWM-seconds integral (acoustic proxy) | Reported with vs without `acoustic_mask`; quantifies acoustic benefit |
| Fault detection latency | `fan_stall_recovery`: stall raised within ≤ 3 s |

All benchmark figures are regenerated from scenario CSV logs by `make figures`. Each figure caption in the white paper cites the scenario name and the git SHA used.

### 9.3 Deterministic thermal-plant simulator

The scenario runner includes a deterministic first-order thermal plant for repeatable controller tests. It is not intended to prove enclosure physics; it provides stable input/output dynamics for governor, policy, fault, and telemetry validation. The simulator lives with `tools/thermalcore-scenario` and is reusable by replay/unit tests so the same plant math drives CLI scenarios, CI assertions, and white-paper figure generation.

For each simulated zone:

```
temp_next = temp_now
          + heat_gain(load_w, coupling_w, ambient_mc) * dt
          - cooling_gain(pwm, fan_curve, temp_now, ambient_mc) * dt
```

Scenario files can configure:

- initial temperature and ambient temperature;
- load steps/ramps in abstract watts or normalized load units;
- fan effectiveness curve;
- optional zone-to-zone coupling coefficient;
- sensor noise, dropout, freeze, and stale-sample injection;
- actuator failure modes such as forced PWM, missing tach, and low tach.

The simulator must be bit-for-bit deterministic for a given config, scenario, and git SHA. All simulator state and coefficients use integer units or Q16.16 fixed-point math; host floating point, platform math-library behavior, and `-ffast-math` are not part of the v1 simulator contract. Any configured noise source uses a deterministic PRNG with an explicit scenario seed. Physical heater tests may be added later, but the first implementation should not depend on analog heat injection to validate basic control behavior.

## 10. Repository Layout

```
thermal-core/
├── README.md
├── LICENSE                              MIT
├── core/                                 Pure C99, no platform deps
│   ├── thermal_core.c
│   ├── thermal_core.h
│   ├── thermal_zone.c
│   ├── thermal_curve.c
│   ├── thermal_pid.c
│   ├── thermal_governor.c
│   ├── thermal_fault.c
│   ├── thermal_modifier.c               Acoustic-mask and future modifiers
│   ├── thermal_types.h
│   ├── thermal_platform.h               snapshot/callback interface
│   ├── thermal_signals.h                Telemetry signal IDs
│   └── thermal_config.h                 Compile-time limits
├── platform/
│   ├── linux/
│   │   ├── thermalcored.c               Main daemon
│   │   ├── bsp_hwmon.c
│   │   ├── bsp_mock_tmpfs.c
│   │   ├── bsp_serial_esp32.c           HIL transport
│   │   ├── bsp_socketcan.c              OBD-II speed source
│   │   ├── bsp_log_syslog.c
│   │   ├── bsp_telemetry_udp.c
│   │   ├── config_jsmn.c
│   │   └── Makefile
│   ├── esp32_idf/
│   │   ├── main/
│   │   │   ├── app_main.c
│   │   │   ├── bsp_pwm_ledc.c
│   │   │   ├── bsp_tach_pcnt.c
│   │   │   ├── bsp_temp_onewire.c
│   │   │   ├── bsp_temp_i2c.c
│   │   │   ├── bsp_can_twai.c           OBD-II PID 0x0D request/decode
│   │   │   ├── bsp_log_esplog.c
│   │   │   ├── bsp_telemetry_cdc.c
│   │   │   ├── bsp_nvs.c
│   │   │   ├── bsp_time.c
│   │   │   └── config_static.c
│   │   ├── CMakeLists.txt
│   │   └── sdkconfig.defaults
│   └── rh850_freertos/                  Scaffolding only in v1
│       └── README.md                    Notes on planned port
├── tools/
│   ├── thermalcore-probe                Telemetry reader + plotter
│   ├── thermalcore-tune                 Runtime tuning CLI
│   ├── thermalcore-scenario             Scenario runner + deterministic plant simulator
│   ├── json2static.py                   JSON → static const C
│   └── car-can-emulator/                Git submodule, branch v2-improvements
├── scenarios/
│   ├── idle_steady_state.scn
│   ├── heat_soak_ramp.scn
│   ├── step_load.scn
│   ├── fan_stall_recovery.scn
│   ├── stuck_sensor.scn
│   ├── acoustic_mask_low_speed.scn
│   ├── acoustic_mask_high_speed.scn
│   ├── multi_zone_coupling.scn
│   ├── runaway.scn
│   └── can_bus_loss.scn
├── test/
│   ├── unit/                            Core tests, no platform
│   ├── replay/                          CSV-driven replay tests
│   └── CMakeLists.txt
├── configs/
│   ├── reference-bench.json             Matches the bench rig BOM
│   ├── minimal-1zone-1fan.json
│   └── 4zone-2fan.json
├── docs/
│   ├── thermal-core-prd.md
│   ├── bench-rig.md                     BOM, wiring, photograph
│   ├── schema/
│   │   └── thermal-core.schema.json     JSON Schema for config
│   ├── paper/
│   │   ├── README.md                    Paper migration notes
│   │   ├── .gitignore                   Ignores LaTeX output artifacts
│   │   ├── Makefile                     Builds the white paper
│   │   ├── src/
│   │   │   ├── thermal-core-spec.tex    LaTeX root
│   │   │   ├── 01-introduction.tex
│   │   │   ├── 02-environments.tex
│   │   │   ├── ...
│   │   │   └── appendices.tex
│   │   ├── figures/
│   │   │   ├── tikz/                    TikZ source for block diagrams
│   │   │   ├── d2/                      D2 source for system diagrams
│   │   │   ├── plots/                   matplotlib scripts
│   │   │   └── img/                     Photographs (bench rig)
│   │   └── refs.bib
│   └── tmp-latex-artifacts-template/    Migration source; remove before v1 release
└── ci/
    ├── build-linux.sh
    ├── build-esp32.sh
    └── run-scenarios.sh
```

Temporary migration inputs such as `docs/tmp-latex-artifacts-template/` and the earlier `tmp-code/car-can-emulator/` checkout are not part of the public v1 layout. The LaTeX template has an active home in `docs/paper/`; the emulator has an active home as the `tools/car-can-emulator/` submodule.

## 11. Build and Toolchain

- **Linux**: `make -C platform/linux` (plain Makefile, `gcc -std=c99 -Wall -Wextra -Werror`). Optionally CMake for parity with embedded.
- **ESP32**: `idf.py -C platform/esp32_idf build`. ESP-IDF v5.x. CMake-based per IDF requirements.
- **OBD-II emulator**: `cmake -H tools/car-can-emulator -B build/car-can-emulator && cmake --build build/car-can-emulator` after submodules are initialized.
- **Unit tests**: `cmake -B build && cmake --build build && ctest --test-dir build` from `test/`. Uses host gcc + a deterministic platform stub.
- **CI**: GitHub Actions workflow runs unit tests, builds Linux daemon, builds ESP32 firmware. Bench-rig scenario runs are manual (require hardware).
- **Reproducibility**: each benchmark log filename embeds the git SHA; figures regenerated by `make figures`. The white paper build (`make -C docs/paper`) regenerates all plots from logs in `docs/paper/figures/plots/data/`.

## 12. White Paper Plan

### 12.1 Audience and tone

Primary: automotive system architects and embedded software engineers building IVI thermal management or similar concepts. Tone: experience-report, hybrid systems/embedded. Not academic. Math present where it earns its place (acoustic-thermal tradeoff, PID anti-windup) but not for its own sake.

### 12.2 Structure (15–25 pages)

1. **Abstract** (½ page)
2. **Introduction and motivation** (1–2 pp)
   - IVI thermal landscape, why acoustic-aware matters, gap in existing tooling
3. **Background and related work** (1–2 pp)
   - Linux thermal framework, `thermald`, `fancontrol`, `fan2go`, `nbfc-linux`; Qualcomm `thermal-engine`; printer firmware thermal control
4. **Architecture** (2–3 pp)
   - Three-layer split, snapshot API, callback surface, discipline constraints, data model
5. **Acoustic-aware policy** (2–3 pp)
   - The acoustic-thermal tradeoff, vehicle-context inputs, modifier design, math of the cap+offset curve
6. **Portability strategy** (1–2 pp)
   - Linux ↔ ESP-IDF ↔ RH850; what's shared vs platform-specific; memory and timing budgets
7. **Implementation notes** (1–2 pp)
   - Fixed-point PID, numeric logging, IIR filters, fault detectors
8. **Bench rig** (1–2 pp)
   - BOM, wiring, photograph, ESP32 firmware modes (standalone vs HIL peripheral)
9. **Observability and tuning workflow** (1 p)
   - Telemetry bus, runtime tuning, scenario scripting; loop-tuning anecdote
10. **Evaluation** (3–5 pp)
    - Scenario results: heat soak, step load, fan stall, stuck sensor, acoustic mask on/off
    - Tables for benchmarks (step time, memory, settling, overshoot, PWM-seconds, fault latency)
    - Per-platform comparison
11. **Honest limitations** (1 p) — see §15
12. **Future work** (1 p) — see §16
13. **Conclusions** (½ page)
14. **References** (½–1 p)

### 12.3 Figures

- **TikZ** (in-line LaTeX): control-loop block diagram, governor state machine, fault detector logic
- **D2** (`make` → SVG → PDF): three-layer architecture, BSP backend swap, repository layout
- **matplotlib** (from scenario CSV): all benchmark plots; one Python script per figure under `docs/paper/figures/plots/`
- **Photograph**: one well-lit photo of the bench rig

All figures regenerable from raw inputs. `make -C docs/paper clean && make -C docs/paper` rebuilds everything from sources without any manual editor step.

### 12.4 LaTeX template migration

The planned LaTeX structure has been migrated early into `docs/paper/` from `docs/tmp-latex-artifacts-template/`. It is an ALS-Dimmer-derived document structure that already has the right style for a technical control-system white paper: numbered section files, TikZ figures, `listings`, image assets, and a Makefile-driven PDF build.

Migration notes:

- `docs/paper/Makefile` sets `MAIN = thermal-core-spec`.
- `docs/paper/src/thermal-core-spec.tex` is the active root file.
- `docs/paper/README.md` records the migration status and build commands.
- The old ALS-Dimmer roots remain only as source references during the rewrite.
- Keep the existing section-file numbering because it gives a useful writing checklist.

Proposed section mapping:

| Template file | `thermal-core` content |
|---|---|
| `01-introduction.tex` | IVI thermal motivation, generic core boundary, project goals |
| `02-environments.tex` | thermal operating environments: ambient ranges, idle cabin, moving vehicle, heat soak |
| `03-human-vision.tex` | rename to acoustic perception / cabin noise masking |
| `04-zone-mapping.tex` | thermal zone modeling and sensor aggregation |
| `05-curves.tex` | fan curves, trip curves, vehicle-speed policy curves |
| `06-response-time.tex` | loop timing, slew limits, PID step response, stability considerations |
| `07-architecture.tex` | three-layer architecture, snapshot API, and callback surface |
| `08-components.tex` | sensors, actuators, BSP backends, fault detectors |
| `09-zone-controller.tex` | governors, arbitration, policy modifiers |
| `10-json-config.tex` | Linux JSON schema and generated static MCU config |
| `11-control-interface.tex` | telemetry, runtime tuning, scenario commands |
| `12-deployment.tex` | Linux daemon, ESP32 modes, `car-can-emulator`, systemd/OpenWrt notes |
| `13-summary.tex` | results summary, recommendations, limitations, future work |
| `appendices.tex` | full configs, OBD-II frame reference, BOM, build reference |

A `Makefile` in `docs/paper/` will codify the build:

```
make -C docs/paper           # full build
make -C docs/paper figures   # only regenerate figures from data
make -C docs/paper clean
make -C docs/paper watch     # latexmk -pvc for iterative editing
```

### 12.5 Writing process

A practical caution: writing always takes longer than coding. Suggested split:

- **Week 1**: core + Linux platform + mock backend + unit tests + initial scenarios
- **Week 2**: ESP32 firmware (both modes) + bench rig assembly + cross-platform scenario runs + telemetry tooling
- **Week 3**: white paper exclusively. Regenerate figures from fresh runs. Prose passes. Honest-limitations and future-work sections written *last*, after all benchmarks are in.

If code overruns into week 3, the paper slips. The right tradeoff is to ship a slightly slimmer v1 with a complete paper, not a feature-complete v1 with a stub paper.

## 13. Schedule (target: 2–3 weeks full-time)

| Day | Milestone |
|---|---|
| 1 | Repo scaffolding, headers (`thermal_platform.h`, `thermal_types.h`), CI green |
| 2 | Core compiles with stub platform; unit-test harness runs |
| 3 | `thermal_zone`, `thermal_curve`, `step_wise` governor; first unit tests pass |
| 4 | `thermal_pid` (Q16.16), step-response unit test |
| 5 | Fault detectors (stall, stuck, runaway); fault unit tests |
| 6 | Linux daemon (`thermalcored`) + `bsp_mock_tmpfs` + JSON config; end-to-end run |
| 7 | Telemetry UDP + `thermalcore-probe`; live plot working on Linux |
| 8 | `bsp_socketcan` + OBD-II PID 0x0D decode; acoustic modifier wired in |
| 9 | ESP32 firmware skeleton; `app_main`, LEDC PWM out, PCNT tach in |
| 10 | ESP32 1-Wire / I2C temp; standalone mode end-to-end on bench |
| 11 | ESP32 TWAI / OBD-II; HIL peripheral mode; serial bridge to Linux |
| 12 | Runtime tuning (`thermalcore-tune`); scenario runner; first canonical scenarios pass |
| 13 | All canonical scenarios pass on Linux and ESP32; benchmarks captured |
| 14 | Buffer / fixes / cross-validation |
| 15–21 | White paper: figures, prose, iteration |

A real schedule will slip; this is the optimistic version. v1.0 tag at day 21; minor `v1.0.x` releases for paper revisions.

## 14. Success Criteria

A v1 release succeeds when:

- `make -C platform/linux` and `idf.py build` both succeed cleanly on a fresh checkout
- All canonical scenarios pass on Linux (mock backend), Linux (ESP32-HIL), and ESP32 (standalone)
- All committed benchmarks have results in the white paper with regeneration scripts
- The reference bench rig is reproducible from `docs/bench-rig.md` alone
- `make -C docs/paper` produces the white paper PDF without manual intervention
- The white paper PDF is committed (or auto-built in CI release) and linked from `README.md`
- An external embedded engineer can clone the repo, build the daemon, and reproduce at least one scenario plot without contacting the author

## 15. Honest Limitations

The white paper contains a dedicated section on limitations, written explicitly and not hidden in footnotes. The current known limitations:

- **Not ASIL-rated.** The implementation does not follow ISO 26262 process. Static analysis, MISRA-C compliance, and formal coverage are out of scope. This is a concept, not a production-safety component.
- **No AUTOSAR integration.** No Classic CDD wrapping, no Adaptive service interface. Integration with a production AUTOSAR stack is a separate effort.
- **First-order thermal model in simulation.** The simulator models thermal mass and a single cooling coefficient; it does not capture multi-mass effects, radiative coupling, or sensor-placement artifacts in a real enclosure.
- **No RH850 hardware validation in v1.** The architecture is structured for RH850-F1KM port; only the Linux and ESP32 implementations are exercised. Memory and timing budgets for RH850 are projected, not measured.
- **Bench fans are not automotive-qualified.** Noctua and Arctic fans are PC-grade; real automotive fans differ in lifetime, vibration, EMC, temperature range. The acoustic and thermal *characteristics* on the bench are indicative, not directly transferable.
- **OBD-II as speed source is unrealistic for production IVI.** Real head units receive speed via OEM-specific CAN messages on a private bus, not OBD-II. OBD-II is used here because it is universally available for a reproducible reference simulator.
- **Single speed signal, no other vehicle context.** Ignition state, drive mode, ambient temperature, HVAC state would all matter in a production policy; v1 demonstrates the modifier mechanism with one input.
- **No closed-loop acoustic measurement.** PWM-seconds is used as an acoustic proxy. Real SPL measurement with a calibrated microphone is future work.
- **Fixed-point PID may not match a floating-point reference exactly.** Verified against scipy-based reference in unit tests, but production deployments should re-validate gains.
- **Single control thread.** Cooperative model. A long sensor read could disturb timing. Watchdog patterns are in scope, full preemptive multi-threading is not.

## 16. Future Work

- **PID autotune** (relay-feedback / Åström-Hägglund).
- **NVS-persisted learned gains**, with safe-rollback.
- **Full RH850-F1KM port** with hardware-validated benchmarks.
- **AUTOSAR Adaptive integration** as a separate package.
- **DLT logging adapter** for production observability.
- **Calibrated SPL measurement** to replace the PWM-seconds proxy.
- **Multi-input policy modifiers**: ignition, ambient, drive mode, HVAC.
- **CAN DBC integration** for OEM-private speed signals.
- **Multi-fan coordinated control** with acoustic cancellation considerations (out-of-phase PWM, RPM detuning).
- **Functional-safety scaffolding** (MISRA-C, static analysis CI) as a path toward ASIL-B/C readiness.
- **Web-based live dashboard** built on the telemetry UDP stream (separate tool, not in core repo).

## 17. Decision Log and Remaining Questions

### 17.1 Decisions

1. **Repo name:** `thermal-core` is the project/repo name; `thermalcored` is the Linux daemon binary.
2. **Core boundary:** `thermal-core` stays protocol-agnostic. CAN, OBD-II, SocketCAN, JSON, sysfs, and emulator TCP commands remain in platform or tooling layers.
3. **OBD-II emulator ownership:** `car-can-emulator` stays a separate upstream repo and is consumed as a git submodule at `tools/car-can-emulator/`, tracking branch `v2-improvements`.
4. **LaTeX migration:** the template is migrated early into `docs/paper/`; `docs/paper/src/thermal-core-spec.tex` is the active root.
5. **PID numeric default:** Q16.16 fixed-point remains the default; floating-point PID may be an optional build flag.
6. **Core API shape:** the core consumes input snapshots and returns output frames; platform code owns all blocking I/O.
7. **v1 plant model:** deterministic first-order simulation plus real fan PWM/tach validation is the v1 release gate. Physical heat injection is optional.
8. **Core storage contract:** `thermal_core_t` is caller-owned fixed-size storage; snapshots are borrowed only for the duration of `thermal_core_step()`, and output frames are full actuator snapshots every tick.
9. **Curve math:** v1 curves use integer linear interpolation with endpoint clamping and no floating point.
10. **Transport contract:** binary telemetry/control frames are little-endian, manually encoded, CRC-16/CCITT-FALSE when CRC is enabled, and receive-buffer capped per platform.
11. **Linux control ingress:** runtime tuning over UDP is loopback-only, config-gated, and bench/development scoped in v1.
12. **Latched fault reset:** `CMD_CLEAR_FAULT` is the explicit reset path for latched v1 faults when recovery criteria are already satisfied; production builds may compile out remote clearing.

### 17.2 Remaining Questions

1. **Telemetry compression at high signal counts?** With ~20 signals at 10 Hz, ESP32 USB-CDC handles it easily. At 100 Hz or 100+ signals, may need ring-buffered batched frames. Defer until measured.

---

## Appendix A: Coordinate-System Conventions

| Quantity | Type | Units | Range |
|---|---|---|---|
| Temperature | `int32_t` | millidegrees C | -40000 to +150000 |
| PWM duty | `uint8_t` | 0–255 | 0–255 |
| Fan RPM | `uint16_t` | RPM | 0–65535 |
| Vehicle speed context | `uint16_t` | km/h | 0–500 generic storage; OBD-II PID `0x0D` source is 0–255 |
| Time | `uint32_t` | milliseconds (monotonic) | rollover handled |
| PID gains | `int32_t` | Q16.16 fixed-point | implementation defined |

## Appendix B: Glossary

- **BSP**: Board Support Package — platform-specific I/O code
- **DLT**: Diagnostic Log and Trace — automotive logging protocol (AUTOSAR)
- **FALD**: Full-Array Local Dimming — display backlight technique (referenced for context)
- **FPDLink / GMSL**: Automotive serializer-deserializer link standards
- **HIL**: Hardware-in-the-Loop testing
- **hwmon**: Linux hardware-monitor sysfs interface
- **IIR**: Infinite Impulse Response (low-pass filter)
- **IVI**: In-Vehicle Infotainment
- **NVS**: Non-Volatile Storage
- **OBD-II PID**: On-Board Diagnostics II Parameter ID
- **PCNT**: ESP32 pulse-counter peripheral
- **PWM**: Pulse-Width Modulation
- **Q16.16**: Fixed-point format, 16 integer bits + 16 fractional bits, stored as `int32_t`
- **SoC**: System on Chip
- **SPL**: Sound Pressure Level
- **TWAI**: ESP32 CAN-bus peripheral name ("Two-Wire Automotive Interface")
- **callback surface**: Optional struct of function pointers used by the core only for numeric logs and telemetry emission

---

*End of PRD v0.5*
