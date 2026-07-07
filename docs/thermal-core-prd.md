# thermal-core — Product Requirements Document

**Project/repo name:** `thermal-core`
**Linux daemon binary:** `thermalcored`
**Repository (planned):** `github.com/hackboxguy/thermal-core`
**Document status:** Draft v0.23
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

The deliverable is **both** a working open-source codebase and a **LaTeX-typeset white paper** that documents the architecture, the acoustic-thermal tradeoff, and the bench validation. The white paper is grounded in working code: every figure regenerates from raw bench data, every benchmark cites a manifest entry holding data SHA, config hash, source git SHA, and tool versions, and every claim is reproducible from the repository.

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

These flags bind the portable core. Platform and BSP layers may need target-specific compiler flags or header allowances for ESP-IDF, Linux, or future RH850 vendor SDKs, but those allowances must not leak into `core/`.

Reference v1 compile-time maxima in `core/thermal_config.h`:

| Macro | Default |
|---|---:|
| `THERMAL_MAX_ZONES` | 4 |
| `THERMAL_MAX_SENSORS` | 8 |
| `THERMAL_MAX_ACTUATORS` | 2 |
| `THERMAL_MAX_CONTEXT_SIGNALS` | 4 |
| `THERMAL_MAX_TRIPS_PER_ZONE` | 4 |
| `THERMAL_MAX_MODIFIERS` | 2 |
| `THERMAL_MAX_FAULTS` | 24 |
| `THERMAL_MAX_SAMPLES_PER_SNAPSHOT` | 16 |
| `THERMAL_MAX_SENSORS_PER_ZONE` | 4 |
| `THERMAL_MAX_ACTUATORS_PER_ZONE` | 2 |
| `THERMAL_MAX_COOLING_STATES` | 5 |
| `THERMAL_MAX_CURVE_POINTS` | 8 |
| `THERMAL_MAX_TELEMETRY_SIGNALS` | 128 |
| `THERMAL_NAME_MAX` | 24 |

### 4.3 Public core API and snapshot model

The platform layer owns all blocking I/O. It polls sensors, tach inputs, CAN signals, files, serial streams, and test injectors, then passes a bounded snapshot into the core. The core consumes that snapshot synchronously and returns actuator requests plus optional telemetry/log callbacks. This keeps Linux, ESP32, and future RH850 timing behavior aligned.

```c
typedef enum {
    THERMAL_OK                  = 0x0000,
    THERMAL_ERR_INVALID_ARG     = 0x0001,
    THERMAL_ERR_INVALID_CONFIG  = 0x0002,
    THERMAL_ERR_BOUNDS          = 0x0003,
    THERMAL_ERR_STATE           = 0x0004,
    THERMAL_ERR_UNAVAILABLE     = 0x0005,
    THERMAL_ERR_NO_SPACE        = 0x0006,
    THERMAL_ERR_REJECTED_SAFETY = 0x0007
} thermal_status_t;

typedef enum {
    THERMAL_SAMPLE_TEMP_MC,
    THERMAL_SAMPLE_TACH_RPM,
    THERMAL_SAMPLE_CONTEXT_I32
} thermal_sample_kind_t;

typedef enum {
    THERMAL_GOVERNOR_STEP_WISE = 0x01,
    THERMAL_GOVERNOR_PID       = 0x02
} thermal_governor_t;

typedef enum {
    THERMAL_AGG_MAX      = 0x01,
    THERMAL_AGG_AVG      = 0x02,
    THERMAL_AGG_WEIGHTED = 0x03
} thermal_aggregation_t;

typedef enum {
    THERMAL_TRIP_WARN     = 0x01,
    THERMAL_TRIP_CRITICAL = 0x02,
    THERMAL_TRIP_SHUTDOWN = 0x03
} thermal_trip_severity_t;

typedef enum {
    THERMAL_CONTEXT_UNIT_NONE       = 0x00,
    THERMAL_CONTEXT_UNIT_KMH        = 0x01,
    THERMAL_CONTEXT_UNIT_BOOL       = 0x02,
    THERMAL_CONTEXT_UNIT_RPM        = 0x03,
    THERMAL_CONTEXT_UNIT_CELSIUS_MC = 0x04
} thermal_context_unit_t;

typedef enum {
    THERMAL_FAILSAFE_ASSUME_STATIONARY = 0x01,
    THERMAL_FAILSAFE_HOLD_LAST         = 0x02,
    THERMAL_FAILSAFE_ASSUME_VALUE      = 0x03
} thermal_context_failsafe_t;

typedef enum {
    THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET = 0x01,
    THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP    = 0x02
} thermal_modifier_stage_t;

typedef enum {
    THERMAL_FAULT_SEVERITY_DEGRADED = 0x01,
    THERMAL_FAULT_SEVERITY_CRITICAL = 0x02
} thermal_fault_severity_t;

typedef enum {
    THERMAL_FAULT_ACTION_NONE                          = 0x00,
    THERMAL_FAULT_ACTION_MARK_DEGRADED                 = 0x01,
    THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK             = 0x02,
    THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED = 0x03,
    THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH       = 0x04,
    THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN              = 0x05
} thermal_fault_action_t;

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
    uint16_t actuator_id;
    uint8_t duty_0_255;
    uint16_t reason;      /* thermal_actuator_reason_t */
} thermal_actuator_cmd_t;

typedef struct {
    thermal_actuator_cmd_t actuator_cmds[THERMAL_MAX_ACTUATORS];
    uint8_t actuator_cmd_count;
} thermal_output_frame_t;

typedef enum {
    THERMAL_ACT_REASON_NONE                  = 0x0000,
    THERMAL_ACT_REASON_GOVERNOR_PID          = 0x0101,
    THERMAL_ACT_REASON_GOVERNOR_STEP         = 0x0102,
    THERMAL_ACT_REASON_MODIFIER_ACOUSTIC_CAP = 0x0201,
    THERMAL_ACT_REASON_FAULT_STALL           = 0x0301,
    THERMAL_ACT_REASON_FAULT_RUNAWAY         = 0x0302,
    THERMAL_ACT_REASON_FAULT_STUCK_SENSOR    = 0x0303,
    THERMAL_ACT_REASON_FAULT_STALE_CONTEXT   = 0x0304,
    THERMAL_ACT_REASON_SAFETY_SHUTDOWN       = 0x0401,
    THERMAL_ACT_REASON_MANUAL_CMD            = 0x0501,
    THERMAL_ACT_REASON_SPINUP                = 0x0601,
    THERMAL_ACT_REASON_DWELL                 = 0x0701
} thermal_actuator_reason_t;

typedef enum {
    THERMAL_CMD_SET_PID         = 0x0001,
    THERMAL_CMD_SET_SETPOINT    = 0x0002,
    THERMAL_CMD_SET_TRIP        = 0x0003,
    THERMAL_CMD_SET_CURVE_POINT = 0x0004,
    THERMAL_CMD_CLEAR_FAULT     = 0x0005
} thermal_command_id_t;

typedef struct {
    uint16_t command_id;        /* thermal_command_id_t */
    union {
        struct { uint16_t zone_id; int32_t kp_q16, ki_q16, kd_q16; } set_pid;
        struct { uint16_t zone_id; int32_t setpoint_mc; } set_setpoint;
        struct { uint16_t zone_id, trip_idx; int32_t temp_mc, hyst_mc; } set_trip;
        struct { uint16_t modifier_id, point_idx; int32_t x, value0, value1; } set_curve_point;
        struct { uint16_t fault_type, target_id; } clear_fault;
    } u;
} thermal_command_t;

typedef struct {
    thermal_status_t status;
    uint32_t detail_code;       /* command/result-specific detail namespace */
} thermal_command_result_t;

typedef enum {
    THERMAL_FAULT_NORMAL,
    THERMAL_FAULT_DEGRADED,
    THERMAL_FAULT_CRITICAL,
    THERMAL_FAULT_LATCHED,
    THERMAL_FAULT_RECOVERING
} thermal_fault_state_t;

typedef enum {
    THERMAL_FAULT_TYPE_STALL         = 0x01,
    THERMAL_FAULT_TYPE_STUCK_SENSOR  = 0x02,
    THERMAL_FAULT_TYPE_RUNAWAY       = 0x03,
    THERMAL_FAULT_TYPE_STALE_CONTEXT = 0x04
} thermal_fault_type_t;

typedef struct {
    int32_t temp_mc;
    uint32_t active_trip_mask;
    uint8_t cooling_state;       /* step-wise state, or PID safety-floor state */
    int32_t effective_setpoint_mc; /* 0 for step-wise zones in v1 */
} thermal_zone_state_t;

typedef struct {
    uint8_t requested_duty_0_255;
    uint8_t duty_0_255;
    uint16_t rpm;
    uint8_t tach_valid;
    uint8_t slew_limited;
    uint16_t reason;            /* thermal_actuator_reason_t */
} thermal_actuator_state_t;

typedef struct {
    uint8_t fault_type;
    uint16_t target_id;
    uint8_t state;              /* thermal_fault_state_t */
    uint32_t entered_ts_ms;      /* timestamp when current state was entered */
} thermal_fault_state_snapshot_t;

/* target_id namespace: STALL=actuator, STUCK_SENSOR=sensor,
   RUNAWAY=zone, STALE_CONTEXT=context signal */

typedef struct {
    int32_t filtered_value;
    uint8_t valid;
    uint32_t ms_since_last_valid;
} thermal_context_state_t;

typedef struct {
    uint8_t modifier_id;
    uint8_t active;              /* 1 when modifier affects output this tick */
    uint8_t pwm_cap_0_255;
    int32_t trip_offset_mc;
} thermal_modifier_state_t;

typedef enum {
    THERMAL_STATE_ANY_FAULT_ACTIVE     = 0x00000001u,
    THERMAL_STATE_SHUTDOWN_REQUESTED   = 0x00000002u,
    THERMAL_STATE_ANY_CONTEXT_STALE    = 0x00000004u,
    THERMAL_STATE_ANY_SAFETY_OVERRIDE  = 0x00000008u
} thermal_state_flags_t;

typedef struct {
    uint32_t now_ms;
    thermal_zone_state_t zones[THERMAL_MAX_ZONES];
    uint8_t zone_count;
    thermal_actuator_state_t actuators[THERMAL_MAX_ACTUATORS];
    uint8_t actuator_count;
    thermal_fault_state_snapshot_t faults[THERMAL_MAX_FAULTS];
    uint8_t fault_count;
    thermal_context_state_t contexts[THERMAL_MAX_CONTEXT_SIGNALS];
    uint8_t context_count;
    thermal_modifier_state_t modifiers[THERMAL_MAX_MODIFIERS];
    uint8_t modifier_count;
    uint32_t flags;
} thermal_state_snapshot_t;

typedef struct {
    int32_t x;
    int32_t value0;
    int32_t value1;
} thermal_curve_point_t;

typedef struct {
    uint16_t id;
    char name[THERMAL_NAME_MAX];
    int32_t iir_alpha_q16;
    uint32_t max_staleness_ms;
} thermal_sensor_cfg_t;

typedef struct {
    uint16_t id;
    char name[THERMAL_NAME_MAX];
    uint8_t unit;
    int32_t iir_alpha_q16;
    uint32_t timeout_ms;
    uint8_t fail_safe;
} thermal_context_cfg_t;

typedef struct {
    uint16_t id;
    char name[THERMAL_NAME_MAX];
    uint8_t pwm_min;
    uint8_t pwm_max;
    uint8_t slew_per_tick;
    uint8_t spinup_pwm;
    uint32_t spinup_ms;
    uint16_t min_on_ticks;
    uint16_t min_off_ticks;
    uint8_t state_pwm[THERMAL_MAX_COOLING_STATES];
#if THERMALCORE_ENABLE_PID
    thermal_curve_point_t duty_linearization[THERMAL_MAX_CURVE_POINTS];
    uint8_t duty_linearization_count;
#endif
} thermal_actuator_cfg_t;

typedef struct {
    int32_t kp_q16, ki_q16, kd_q16;
#if THERMALCORE_ENABLE_PID
    int32_t d_filter_alpha_q16;
#endif
    int32_t setpoint_mc;
    int32_t kp_min_q16, kp_max_q16;
    int32_t ki_min_q16, ki_max_q16;
    int32_t kd_min_q16, kd_max_q16;
    int32_t setpoint_min_mc, setpoint_max_mc;
    uint16_t dt_min_ms, dt_max_ms;
} thermal_pid_cfg_t;

typedef struct {
    int32_t temp_mc;
    int32_t hyst_mc;
    uint8_t severity;
    uint8_t cooling_state;
} thermal_trip_cfg_t;

typedef struct {
    char name[THERMAL_NAME_MAX];
    uint16_t sensor_ids[THERMAL_MAX_SENSORS_PER_ZONE];
    int32_t sensor_weights_q16[THERMAL_MAX_SENSORS_PER_ZONE];
    uint8_t sensor_count;
    uint8_t aggregation;
    int32_t fallback_temp_mc;
    uint8_t governor;
    thermal_pid_cfg_t pid;
    uint16_t actuator_ids[THERMAL_MAX_ACTUATORS_PER_ZONE];
    uint8_t actuator_count;
    thermal_trip_cfg_t trips[THERMAL_MAX_TRIPS_PER_ZONE];
    uint8_t trip_count;
} thermal_zone_cfg_t;

typedef struct {
    char name[THERMAL_NAME_MAX];
    uint16_t context_id;
    uint8_t stages;
    thermal_curve_point_t curve[THERMAL_MAX_CURVE_POINTS];
    uint8_t curve_count;
    uint8_t fail_safe;
} thermal_modifier_cfg_t;

typedef struct {
    uint8_t enabled;
    uint8_t severity;
    uint8_t action;
    uint16_t persist_ticks;
    uint16_t recovery_ticks;
    int32_t threshold0;
    int32_t threshold1;
    int32_t threshold2;
    uint16_t correlated_context_id;
} thermal_fault_detector_cfg_t;

typedef struct {
    thermal_fault_detector_cfg_t stall_defaults;
    thermal_fault_detector_cfg_t stuck_sensor_defaults;
    thermal_fault_detector_cfg_t runaway_defaults;
    thermal_fault_detector_cfg_t stale_context_defaults;
} thermal_fault_detection_cfg_t;

typedef struct {
    uint8_t enable;
    uint16_t period_ticks;
    uint16_t enabled_signal_ids[THERMAL_MAX_TELEMETRY_SIGNALS];
    uint8_t enabled_signal_count;
} thermal_telemetry_cfg_t;

typedef struct {
    uint16_t config_version;
    uint16_t control_period_ms;
    uint16_t period_relative_to_ms;
    thermal_sensor_cfg_t sensors[THERMAL_MAX_SENSORS];
    uint8_t sensor_count;
    thermal_context_cfg_t contexts[THERMAL_MAX_CONTEXT_SIGNALS];
    uint8_t context_count;
    thermal_actuator_cfg_t actuators[THERMAL_MAX_ACTUATORS];
    uint8_t actuator_count;
    thermal_zone_cfg_t zones[THERMAL_MAX_ZONES];
    uint8_t zone_count;
    thermal_modifier_cfg_t modifiers[THERMAL_MAX_MODIFIERS];
    uint8_t modifier_count;
    thermal_fault_detection_cfg_t faults;
    thermal_telemetry_cfg_t telemetry;
} thermal_config_t;
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
                                            uint32_t now_ms,
                                            const thermal_command_t *cmd,
                                            thermal_command_result_t *result);
thermal_status_t thermal_core_get_state(const thermal_core_t *ctx,
                                        thermal_state_snapshot_t *state);
```

Allocation and lifetime contract:

- `thermal_core_t` is a caller-owned, fixed-size public struct declared in `core/thermal_types.h`. Callers may allocate it statically, globally, or on the stack; the core does not allocate it internally.
- `sizeof(thermal_core_t)` depends on compile-time maxima such as `THERMAL_MAX_ZONES` and `THERMAL_MAX_ACTUATORS`. v1 promises a stable source API, not a stable binary ABI across builds with different maxima.
- `thermal_config_t` is the validated, platform-independent config shape consumed by the core. Platform-only source strings, PWM frequency, tach pulse conversion, and transport URIs are consumed by loaders/BSPs and do not enter the core config unless they affect deterministic policy behavior.
- `thermal_core_init()` fully initializes caller-provided storage; callers do not need to pre-zero the context. The `thermal_config_t` passed to init must remain alive and immutable for the lifetime of the context. Runtime commands update the core's runtime shadow state, not the const config.
- If `thermal_core_init()` returns non-OK, the context must be treated as uninitialized and no other API may be called on it until init succeeds. `thermal_core_validate_config()` should be called first so init failures are limited to invalid args or storage misuse. Re-initializing the same storage is allowed only after a previous successful context is no longer being stepped; no separate `deinit` is required in v1.
- The callback struct is copied by value during init. The function pointers it contains must remain callable while the context is active.
- `thermal_input_snapshot_t` and its `samples` array are borrowed only for the duration of `thermal_core_step()`. The core may copy sample values into internal state, but it must not retain pointers to caller-owned snapshot memory. `samples == NULL` with `sample_count == 0` is valid and means all configured inputs are absent for that tick.
- The platform supplies samples in any order, indexed by `(kind, id)`. It supplies at most one sample per `(kind, id)` per tick and `sample_count <= THERMAL_MAX_SAMPLES_PER_SNAPSHOT`. Unknown IDs, impossible kind/ID combinations, duplicates, or oversized snapshots are platform bugs; the core returns `THERMAL_ERR_INVALID_ARG` or `THERMAL_ERR_NO_SPACE` and does not update state for that step.
- On success, `thermal_core_step()` writes a full actuator output frame every tick: one command for each configured actuator, even when the duty cycle has not changed. `actuator_cmd_count` equals the configured actuator count in v1 and is retained as a consistency/forward-compat field.
- `thermal_output_frame_t` does not carry its own timestamp. The platform treats the frame as corresponding to the input snapshot's `now_ms` and may stamp hardware writes or logs with that value.
- A single `thermal_core_t` is not reentrant. In v1, all core API calls happen from the control thread/task. The Linux daemon drains and applies queued UDP commands between `thermal_core_step()` calls; platforms that choose a different threading model must serialize every core API call with a mutex.
- `thermal_core_apply_command()` uses its `now_ms` parameter for any command-applied/rejected events it emits. Transport frame `seq` is not part of the core API and remains owned by the UDP/serial/CAN framing layer.

`thermal_core_step()` must not call sensor, CAN, file, serial, or actuator drivers. It performs bounded work over the provided snapshot. Missing or stale samples are represented explicitly with `valid = 0` and are handled by configured fail-safe behavior.

`thermal_core_step()` return contract: `THERMAL_OK` means the snapshot was consumed, internal state updated, the output frame populated, and callbacks invoked best-effort. `THERMAL_ERR_INVALID_ARG` and `THERMAL_ERR_NO_SPACE` mean the snapshot was malformed or oversized; internal state and the output frame are left untouched. `THERMAL_ERR_STATE` means the context was not successfully initialized.

`THERMAL_ERR_NO_SPACE` is used when validated configuration or decoded command/state data exceeds compile-time maxima or caller-provided transport buffers. A successfully initialized core with a validated config must not return `THERMAL_ERR_NO_SPACE` merely because it has more actuators than fit in `thermal_output_frame_t`; that would be a validation bug.

Actuator output duty is clamped to configured limits after arbitration, policy modifiers, fault overrides, and slew handling. Duty `0` is a special off command. Any non-zero final request below `pwm_min` is raised to `pwm_min`; requests above `pwm_max` are lowered to `pwm_max`. A policy `pwm_cap` equal to `pwm_max` is equivalent to no cap.

`thermal_actuator_state_t.slew_limited` is a boolean: `1` when the current commanded duty differs from the immediate raw request because the slew limiter constrained the transition, otherwise `0`.

`thermal_actuator_state_t.tach_valid` is a boolean: `0` when no current valid tach reading exists, either because no tach source is configured or because the latest tach sample is stale/invalid; otherwise `1`.

`thermal_actuator_state_t.requested_duty_0_255` captures the raw post-governor, post-arbitration request before final clamp, fault override, and slew effects. `duty_0_255` is the final command sent to the platform.

State snapshots are intended for status views, assertions, and replay diffs, not mandatory per-tick polling. At v1 defaults the structure is small enough for on-demand CLI/status use, but telemetry remains the normal high-rate observation path.

Before the first successful step, state inspection reports actuator `reason = THERMAL_ACT_REASON_NONE`, fault `state = THERMAL_FAULT_NORMAL`, and `flags = 0`. Reserved `thermal_state_flags_t` bits must be masked off by v1 implementations and ignored by readers.

`thermal_modifier_state_t.active` is `1` when the modifier changed an effective setpoint/trip or actuator request during the current step. A stale context with an applied fail-safe value still counts as active if the resulting modifier output affects control.

### 4.4 Platform callback surface

```c
typedef struct {
    /* Logging — numeric event codes, no strings */
    void     (*log_event)(uint32_t ts_ms, uint16_t code, uint32_t a1,
                          uint32_t a2, uint32_t a3, uint32_t a4);

    /* Telemetry emit (optional, may be NULL) */
    void     (*telemetry_emit)(uint32_t ts_ms, uint16_t signal_id,
                               int32_t value);
} thermal_core_callbacks_t;
```

`telemetry_emit()` is for continuous numeric signals such as zone temperature, PWM, RPM, PID terms, context values, and policy outputs. `log_event()` is for discrete state transitions such as fault enter/leave, mode changes, accepted/rejected commands, and shutdown requests. Telemetry signal IDs live in `core/thermal_signals.h`; event codes live in `core/thermal_events.h` and use a separate namespace. During `thermal_core_step()`, all callback timestamps use the input snapshot's `now_ms`, so every callback emitted from one control step shares the same deterministic timestamp.

Callbacks are executed inside `thermal_core_step()` and must return promptly. Platform callbacks must buffer, queue, coalesce, or drop telemetry/log records rather than blocking on UDP, serial, syslog, NVS, disk, locks held by other threads, or slow formatting. Numeric-code to text rendering belongs in platform/tooling layers such as `bsp_log_syslog.c`, `bsp_log_esplog.c`, and `thermalcore-probe`; the core emits numeric IDs only.

Persistence, sleeping, scheduling, NVS, JSON parsing, SocketCAN, sysfs, ESP-IDF drivers, and actuator writes remain platform responsibilities. If a platform wants persisted runtime-tuned values, it stores command deltas outside the core and reapplies them through `thermal_core_apply_command()` at startup.

### 4.5 Data model

Borrowed in spirit from the Linux kernel thermal framework:

- **Sensor**: a source of temperature samples in millidegrees C. Has an ID, a name (debug only), an IIR filter coefficient, and a stuck-sensor detector.
- **Zone**: a logical thermal region (e.g., "soc", "amp", "tuner"). Has 1+ sensors (aggregated max/avg/weighted), 1+ trip points, an active governor, and a current state. Weighted aggregation requires a `weights` array with one Q16.16 weight per sensor.
- **Trip point**: a temperature threshold with a hysteresis band, a severity, and a target cooling state.
- **Actuator / thermal device**: the concept-level output target of a governor. The generic shape is a staged or continuous thermal-device request; the v1 backend is deliberately narrower: a single-quadrant PWM fan with optional tach feedback, min/max PWM, slew-rate limit, spin-up, dwell, and stall detection. v1 does not model throttling devices, heaters, pumps, inverted polarity, bidirectional Peltier devices, or per-actuator type dispatch. Those require an explicit future actuator-kind field and backend contract.
- **Governor**: a control algorithm. v1 = `step_wise` (Linux-thermal style discrete states) and `pid` (continuous PID with anti-windup).
- **Context signal**: a typed non-temperature input, such as vehicle speed, ignition state, drive mode, HVAC state, ambient-noise proxy, or workload/power estimate. v1 uses vehicle speed only. Context IDs are configured; the core never knows where the value came from. A future load/power context is the intended path for feed-forward control and a stronger stuck-sensor correlate than vehicle speed.
- **Policy modifier**: a configured rule that can transform either pre-governor effective targets/trips or post-governor actuator requests based on context. v1 allows exactly one modifier, `acoustic_mask`, with a pre-governor trip/setpoint offset and a post-governor PWM cap as functions of vehicle speed. The config field is an array only for forward compatibility. A future multi-modifier contract must evaluate modifiers in configured order, combine PWM caps by minimum, combine offsets by bounded saturating sum, re-apply the same trip/setpoint validation to the combined offset, and emit per-modifier telemetry so the final policy remains explainable.
- **Arbitrator**: when multiple zones target the same actuator, the arbitrator decides the final command. v1 = max-wins (highest demanded PWM), with no strategy hook. If one zone lists multiple actuators, the zone produces one demand that is broadcast unscaled to each listed actuator before per-actuator arbitration, clamps, and slew limits are applied. v1 does not split PID demand, balance load, or weight cooling across multiple actuators; those policies require an explicit future arbitration strategy.
- **Fault detector**: stall, sensor-stuck, runaway, stale context. Per detector, configurable thresholds, severity, latching/recovery behavior, and action.

**IIR filter math (used by sensor `iir_alpha_q16` and context-signal `iir_alpha_q16`):** a single-pole low-pass with Q16.16 coefficient `alpha`:

```
filtered_next = filtered_prev + alpha_q16 * (sample - filtered_prev)
```

Convention:
- `alpha_q16 = 0` holds the previous value (no update on new sample).
- `alpha_q16 = Q16_ONE` (0x00010000) passes the new sample through unchanged.
- Intermediate values produce a first-order low-pass; smaller `alpha_q16` is heavier filtering.

The multiplication is computed in 64-bit intermediate (`int64_t`) and shifted back to Q16.16. Overflow is saturated and emits an overflow event in release builds, fails the test in debug builds, matching the saturating-helper rule for governor math in §4.8. The same formula is used for context signals, so context-filter step responses are identical to sensor-filter step responses for the same `alpha_q16`.

**Filter validity lifecycle:**

- Each filter has its own `valid` flag tracked separately from the numeric `filtered_value`.
- Before the first valid sample, `valid = 0`; the numeric value is undefined and must not be read.
- On the first valid sample, the filter initializes `filtered_value` directly to the sample (no IIR step from zero) and sets `valid = 1`.
- On subsequent valid samples, the IIR equation above advances `filtered_value`.
- On an invalid sample (`sample.valid = 0` or staleness exceeded), the filter does not update arithmetic. `filtered_value` holds its last numeric value; `valid` is set to `0`.
- Aggregation skips sensors whose filter `valid = 0`. The held numeric value is not policy-active.
- For context signals, the `fail_safe = hold_last` mode keeps the held filtered value policy-active even with `valid = 0`; `assume_stationary` substitutes `0`. `assume_value` is a reserved enum value for a future config shape with an explicit fallback value and is rejected by v1 validation. The held filter state and the policy-active flag are distinct.

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
     10. apply spin-up actuation, slew-rate limit, and final clamp; populate thermal_output_frame_t
     11. emit selected telemetry on configured cadence and log state transitions
    platform:
     12. write actuator commands and persist/forward telemetry as needed
```

Safety ordering: acoustic caps and comfort policies may reduce cooling only while the relevant zone is below critical severity and no safety override is active. Critical trips, runaway faults, and configured emergency actions override acoustic caps. Because post-governor policy caps run before fault actions, a fault action such as `force_pwm_max` is allowed to overwrite an already-capped PWM request. Upward safety overrides from `CRITICAL`, `LATCHED`, `shutdown`, and runaway paths bypass the normal slew-rate limiter; recovery and downward transitions still obey slew limits. Anti-short-cycle dwell is an actuator-level comfort/lifetime policy: `min_on_ticks` can hold a non-zero command before allowing off, and `min_off_ticks` can hold zero before allowing restart; safety overrides bypass both. Fan spin-up is an actuator-level boost: when the previously applied duty was `0` and the post-safety request is non-zero after dwell, the core commands at least `spinup_pwm` for `spinup_ms` before returning to the requested duty, emitting `THERMAL_ACT_REASON_SPINUP` while the boost is active.

Determinism: the loop is monotonic; `now_ms()` rollover (32-bit ms = ~49 days) is handled with `(uint32_t)(a - b)` subtraction throughout. All loop work is bounded by compile-time maxima.

### 4.7 Fault model

Faults are explicit state machines, not one-shot booleans. Each detector emits a numeric event when it enters or leaves a state.

Detector configuration is specified as global defaults per detector type, while detector state is instantiated per target: stall per actuator, stuck-sensor per sensor, runaway per zone, and stale-context per context signal. `THERMAL_MAX_FAULTS` therefore counts active detector instances, not detector types; the v1 default of 24 covers the default maxima with headroom. Per-target tuning overrides are deferred unless bench data proves the global defaults are too coarse.

`thermal_fault_detector_cfg_t` uses generic threshold fields so all detector defaults fit one C shape. JSON uses descriptive names; the loader maps them as follows:

| Detector | `threshold0` | `threshold1` | `threshold2` | Notes |
|---|---|---|---|---|
| `stall` | `stall_rpm` | `stall_pwm_threshold` | unused | Detector defaults are the single source for both thresholds in v1. |
| `stuck_sensor` | `delta_mc` | `window_ticks` | `correlated_delta_threshold` | `window_ticks` is stored as `int32_t` for the generic field and validated non-negative. |
| `runaway` | `rise_mc_threshold` | `cooling_pwm_threshold` | unused | Defaults chosen so runaway means rising temperature while cooling is high or increasing. |
| `stale_context` | unused | unused | unused | Timeout comes from the context signal config. |

`enabled = 0` on a detector-type default means no instances of that detector type are created. `correlated_context_id` is used only by stuck-sensor detection. For stall, runaway, and stale-context defaults it is ignored and set to `0xFFFF` by convention. Tach-less actuators do not instantiate an active stall detector unless explicitly forced by a test scenario; otherwise missing tach would create false stall events.

`thermal_fault_detection_cfg_t` names the four v1 detector types directly. Adding new detector types in v2 may require extending this struct; that is accepted as a v1 source-compatibility tradeoff.

| State | Meaning | Typical action |
|---|---|---|
| `NORMAL` | No active fault. | Run configured governors and policies. |
| `DEGRADED` | Fault detected, system can continue with reduced confidence. | Use fallback value, force minimum safe cooling, or mark telemetry. |
| `CRITICAL` | Thermal margin or actuator confidence is compromised. | Override acoustic caps and request maximum configured cooling. |
| `LATCHED` | Fault remains active until explicit reset or reboot. | Hold configured fault action and require operator/tool action. |
| `RECOVERING` | Signal has returned to normal but must remain stable for `recovery_ticks`. | Keep conservative output until recovery completes. |

Required v1 detector behavior:

- **Fan stall:** active when requested PWM is above `stall_pwm_threshold` and tach stays below `stall_rpm` for `persist_ticks`, excluding configured spin-up grace. Recovery requires tach above threshold for `recovery_ticks`.
- **Stuck sensor:** active when a valid sensor changes by less than `delta_mc` across `window_ticks` while the configured `correlated_context` changes by at least `correlated_delta_threshold` during that same window. If no correlated context is configured (`0xFFFF` / JSON `null`), v1 runs in flatness-only mode and the same long flat sensor window is actionable.
- **Thermal runaway:** active when temperature rises for `persist_ticks` while requested cooling is high or increasing. This fault is `CRITICAL` by default and overrides acoustic caps. A `force_pwm_max_and_latch` runaway escalates to a shutdown request if the runaway condition remains active for `recovery_ticks` after the latch. `recovery_ticks = 0` disables that escalation path.
- **Stale context:** active when a context sample is invalid longer than its timeout. The configured context fail-safe value is applied and the policy emits telemetry so tests can prove the fallback occurred.

Fault actions must be idempotent and bounded. A fault action may force an actuator command higher than the governor requested, but v1 must not silently command a lower cooling level during `CRITICAL` or `LATCHED` thermal faults.

Required v1 fault actions:

| Action | Core behavior |
|---|---|
| `mark_degraded` | Emit event/telemetry and keep normal governor output. |
| `use_zone_fallback` | Replace the affected zone's computed temperature with the max of remaining valid zone sensors; if none exist, use the zone's configured `fallback_temp_mc`. Invalid config if neither is possible. Enter/exit edges emit `TEVENT_ZONE_FALLBACK_ENTER` / `TEVENT_ZONE_FALLBACK_EXIT`, and `aggregation_valid` is exposed as zone telemetry. |
| `force_pwm_max_until_recovered` | Request maximum configured PWM for affected actuators until the detector reaches `NORMAL`. Upward command is slew-exempt. |
| `force_pwm_max_and_latch` | Request maximum configured PWM, enter `LATCHED` after detection, and require `CMD_CLEAR_FAULT`, reboot, or platform-local maintenance reset after recovery. Upward command is slew-exempt. For runaway, continued active runaway for non-zero `recovery_ticks` while latched also emits one `TEVENT_SHUTDOWN_REQUEST` and latches shutdown-requested state. |
| `request_shutdown` | Latch a shutdown-request condition, emit `TEVENT_SHUTDOWN_REQUEST`, and request maximum configured cooling. The platform decides whether that event maps to process exit, system power action, or no action. |
| `none` | Emit detector telemetry only. Useful for advisory detectors and experiments. |

Trip severity and fault state are separate vocabularies. Trip severity describes thermal policy for a zone; fault state describes detector confidence and recovery. v1 maps trip severities as follows:

| Trip severity | Core behavior |
|---|---|
| `warn` | Emit telemetry/event. Step-wise zones request the configured cooling state; PID zones keep PID as the normal output source. Acoustic caps may still apply. |
| `critical` | Step-wise zones request the configured cooling state. PID zones floor the PID output to the affected actuator's `state_pwm[cooling_state]`. Affected actuators bypass acoustic caps and emit a safety-override event. |
| `shutdown` | Request maximum configured cooling for affected actuators, latch a shutdown-request condition, and emit `TEVENT_SHUTDOWN_REQUEST`. The core never halts the OS, powers off hardware, or enters an OEM limp-home mode; the platform decides what to do with the request. |

`CMD_CLEAR_FAULT` is the explicit reset path for `LATCHED` v1 faults. It succeeds only when the underlying detector condition is no longer present and the configured recovery criteria are satisfied; otherwise it returns `CMD_NACK` with `THERMAL_ERR_REJECTED_SAFETY`. Production builds may compile out remote `CMD_CLEAR_FAULT`; in that case the reset path is reboot or a platform-local maintenance action.

`TEVENT_COMMAND_APPLIED` and `TEVENT_COMMAND_REJECTED` are emitted by `thermal_core_apply_command()` itself, not by individual platform daemons, so command-event behavior is identical across Linux, ESP32, and replay tests.

Runaway v1 math: for each zone, let `N` be the current tick and `M = N - persist_ticks`. The detector enters active state when `zone_temp_mc[N] - zone_temp_mc[M] >= rise_mc_threshold` and the minimum commanded PWM for the affected actuator set across ticks `M..N` is at least `cooling_pwm_threshold`. This makes runaway mean "temperature is still rising materially even while cooling has been high for the whole persistence window."

### 4.8 Governor semantics

The implementation must make governor math reproducible across targets:

- **Step-wise governor:** trip points are evaluated with hysteresis. Each active trip contributes a `cooling_state`; the zone request is the highest active state.
- **PID governor:** error is `zone_temp_mc - effective_setpoint_mc`; positive error increases cooling demand. Output units are PWM duty `0..255` before arbitration and policy modifiers.
- **Governor dispatch:** the v1 governor set remains closed and enum-based. Implementations may centralize per-governor behavior in a static ops table or equivalent compile-time dispatch, but no runtime governor registration API is part of v1. A build profile may compile out an optional governor such as PID; the public enum value remains reserved, and any config selecting a governor unavailable in that build is invalid.
- **Trips with PID:** PID remains the normal continuous controller. Trips on PID zones provide telemetry and safety floors: `warn` is informational, `critical` floors output via `state_pwm[cooling_state]`, and `shutdown` requests maximum configured cooling plus shutdown event.
- **PID state snapshot:** `thermal_zone_state_t.cooling_state` is `0` for PID zones while no safety trip floor is active; it becomes the highest active safety trip's `cooling_state` while a `critical` or `shutdown` floor is affecting output.
- **PID timestep:** `dt` is derived from `thermal_input_snapshot_t.now_ms` and clamped to configured min/max bounds. A platform that passes measured wall time lets the core observe missing or extremely late ticks through that clamped `dt`; a platform that passes scheduled time must emit a platform diagnostic when the observed wall-clock wakeup overruns the scheduled deadline.
- **Timestamp behavior:** `now_ms` must be monotonic non-decreasing within a wrap window. Non-monotonic jumps are platform bugs; PID `dt` clamping limits damage but the platform should emit a diagnostic. The Linux daemon deliberately keeps scheduled-time `now_ms` in wall-clock mode for deterministic control math and emits `TEVENT_PLATFORM_TICK_OVERRUN` when `CLOCK_MONOTONIC` shows it woke late by more than its overrun threshold.
- **Anti-windup:** v1 uses bounded integral state. The integral is not increased further when output is saturated and the error would drive it deeper into saturation.
- **Derivative:** derivative is computed on measured temperature, not error. `d_filter_alpha_q16` is an optional first-order IIR over the D term; `0` disables it and preserves the unfiltered PID math exactly, while non-zero values are Q16.16 coefficients in `[0, Q16_ONE]`. The first real derivative sample initializes the filtered value directly, matching the sensor-filter lifecycle and avoiding a synthetic startup ramp.
- **PID actuator linearization:** an actuator may carry an optional `duty_linearization` table, compiled only when PID support is enabled. PID zones map their continuous `0..255` demand through this per-actuator monotonic curve before comparing it with any active safety floor (`max(linearized_pid, state_pwm[cooling_state])`). Step-wise zones never use this table; their `state_pwm[]` staircases remain literal.
- **Feed-forward:** v1 PID is feedback-only: `u = P + I + D`, with no load/power term. A future feed-forward extension should enter as a typed load or power context signal with an explicit gain/curve and should share validation and telemetry conventions with policy modifiers rather than placing platform-specific load logic inside the governor.
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
  "period_relative_to_ms": 100,
  "sensors": [
    {"id": 0, "name": "soc",     "source": "hwmon:chip=coretemp:input=temp1_input", "iir_alpha_q16": 16384, "max_staleness_ms": 500},
    {"id": 1, "name": "amp",     "source": "ds18b20:28-0000abc", "max_staleness_ms": 2000},
    {"id": 2, "name": "tuner",   "source": "i2c:1:0x18:mcp9808", "max_staleness_ms": 500},
    {"id": 3, "name": "board",   "source": "hwmon:chip=nct6775:input=temp2_input", "max_staleness_ms": 500},
    {"id": 4, "name": "ambient", "source": "ds18b20:28-0000xyz", "max_staleness_ms": 2000}
  ],
  "context_signals": [
    {"id": 0, "name": "vehicle_speed", "unit": "kmh",
     "source": "canbus:obd2:pid_0x0D", "iir_alpha_q16": 2048,
     "timeout_ms": 3000, "fail_safe": "assume_stationary"}
  ],
  "actuators": [
    {"id": 0, "name": "main_fan", "pwm": "hwmon:chip=nct6775:input=pwm1", "tach": "hwmon:chip=nct6775:input=fan1_input",
     "pwm_min": 80, "pwm_max": 255, "slew_per_tick": 8,
     "pwm_freq_hz": 25000, "tach_pulses_per_rev": 2, "spinup_pwm": 180, "spinup_ms": 500,
     "min_on_ticks": 0, "min_off_ticks": 0,
     "state_pwm": [0, 100, 160, 220, 255],
     "duty_linearization": [[0, 0], [100, 150], [255, 255]]}
  ],
  "zones": [
    {"name": "soc", "sensors": ["soc"], "aggregation": "max",
     "fallback_temp_mc": 85000,
     "governor": "pid",
     "pid": {"kp_q16": 4915, "ki_q16": 327, "kd_q16": 0,
             "d_filter_alpha_q16": 0, "setpoint_mc": 75000,
             "kp_min_q16": 0, "kp_max_q16": 327680,
             "ki_min_q16": 0, "ki_max_q16": 65536,
             "kd_min_q16": 0, "kd_max_q16": 65536,
             "setpoint_min_mc": 50000, "setpoint_max_mc": 95000,
             "dt_min_ms": 50, "dt_max_ms": 500},
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
    "stall": {"persist_ticks": 30, "recovery_ticks": 10, "stall_rpm": 200, "stall_pwm_threshold": 80, "severity": "degraded", "action": "force_pwm_max_until_recovered"},
    "stuck_sensor": {"window_ticks": 600, "delta_mc": 100, "correlated_context": null, "severity": "degraded", "action": "use_zone_fallback"},
    "runaway": {"persist_ticks": 50, "recovery_ticks": 20, "rise_mc_threshold": 500, "cooling_pwm_threshold": 200,
                "severity": "critical", "action": "force_pwm_max_and_latch"}
  },
  "telemetry": {
    "enable": true,
    "transport": "udp:127.0.0.1:9001",
    "period_ticks": 1,
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

In the example `soc` zone, the PID setpoint drives normal cooling output. The trip points still matter: `critical` acts as a safety floor through `state_pwm[cooling_state]`, and `shutdown` requests maximum cooling plus a shutdown-request event.

PID gain and setpoint bounds are per-zone config fields. Runtime tuning commands may change the live gains/setpoint only within these bounds; safety-related trip `severity` and `cooling_state` remain config-time only in v1.

Aggregation uses valid samples only. For `max`, `avg`, and `weighted`, invalid samples are skipped; if all zone sensors are invalid, the zone uses the configured fallback path or enters the relevant degraded/fault behavior. `sensor_weights_q16` is consulted only when `aggregation == THERMAL_AGG_WEIGHTED`; for `max` and `avg`, the field is ignored.

Sensor `max_staleness_ms` is enforced by the platform when building `thermal_input_snapshot_t`: if `(uint32_t)(now_ms - sample_ts_ms)` exceeds the configured age, the platform reports the sample with `valid = 0`. This lets slow sources such as DS18B20 probes reuse cached samples between conversions without confusing the core about freshness.

Fields such as `source`, `pwm_freq_hz`, and `tach_pulses_per_rev` are platform-loader fields. The core receives temperature in millidegrees C, tach as RPM, and actuator limits/state tables after platform conversion. For the reference 4-wire PC fan bench, PWM frequency is 25 kHz. Sensor/context IIR coefficients are applied once per control tick, and detector windows, telemetry cadence, dwell knobs, and fan-health stability gates are tick-count based. `period_relative_to_ms` is optional metadata for that authoring assumption; when present, it must match `control_period_ms` so a retimed deployment fails validation until those period-relative knobs are reviewed. `min_on_ticks`/`min_off_ticks` are period-relative anti-short-cycle dwell knobs; zero disables the corresponding dwell. `spinup_pwm`/`spinup_ms` apply whenever an actuator transitions from duty `0` to non-zero after dwell; the spin-up boost bypasses normal upward slew for its configured window, and stall detection ignores tach failures during the same grace window.

Platform-only fields live in platform-specific configuration structures, such as `thermalcored_runtime_cfg_t` under `platform/linux/`. Examples include source URIs, `pwm_freq_hz`, `tach_pulses_per_rev`, `telemetry.transport`, `telemetry.signals`, and `control.listen`. Loaders convert these into `thermal_config_t` plus platform runtime config.

The `fault_detection` object provides detector-type defaults. During validation/init, the core expands those defaults into bounded per-target detector instances for each configured actuator, sensor, zone, and context signal.

The Linux bench daemon may initially run as root because many `hwmon` PWM files require elevated access. The v1 docs should also include a udev/group-access example for running `thermalcored` without full root privileges.

### 5.2 ESP32 and RH850: static `const` struct

Same `thermal_config_t` shape, populated as a static const in C (built into the firmware image). The JSON parser is never linked. Configuration is changed by rebuilding, **or** at runtime via the control-plane (see §7) for parameters within configured limits.

A small Python tool `tools/json2static.py` converts a JSON config into a `static const thermal_config_t` C file, for round-trip consistency between Linux experimentation and MCU deployment.

MCU-target configs additionally carry an optional `mcu_pinmap` section — a target-agnostic JSON key that names the per-slot GPIO assignments and PWM frequency for each sensor and actuator. When present, `json2static.py` emits a second platform-scoped const struct (e.g. `const esp32_pinmap_t G_ESP32_PINMAP` for the ESP32 target) alongside `G_THERMAL_CFG`, so the firmware's BSP layer reads its pin map from the same JSON the core config came from. The `mcu_pinmap` key is generic; the emitted struct is platform-specific (each MCU port owns its own pinmap header).

### 5.3 Configuration validation rules

`thermal_core_validate_config()` and the Linux JSON loader must reject invalid configuration before the control loop starts. Required v1 checks:

- `config_version` is present and supported.
- `control_period_ms` is non-zero. Optional `period_relative_to_ms`, when present, equals `control_period_ms`; it documents that IIR coefficients and tick-count windows were authored for that control period.
- The Linux loader treats `jsmn` as a tokenizer only; validation is a hand-written, schema-aware walk that enumerates every allowed key for each object type.
- IDs and debug names are unique within their namespaces.
- All references resolve: zones to sensors, zones to actuators, policies to context signals, telemetry selectors to known signal IDs.
- All arrays fit compile-time maxima.
- Zone `sensor_count >= 1` and `actuator_count >= 1`.
- Sensor `max_staleness_ms` is non-zero and at least as large as the expected conversion/poll interval for that source.
- Units are explicit in field names or schema metadata; core units are millidegrees C, 0-255 PWM duty, RPM, milliseconds, and Q16.16 coefficients.
- Curves are strictly increasing in their x-axis and have at least two points; interpolation uses the deterministic formula in §4.8.
- Runtime curve edits must preserve the same strictly increasing x-axis invariant. `CMD_SET_CURVE_POINT` rejects edits that would make `x[point_idx - 1] < x[point_idx] < x[point_idx + 1]` false, returning `THERMAL_ERR_BOUNDS`.
- PWM bounds satisfy `0 <= pwm_min <= pwm_max <= 255`; `state_pwm` has an entry for every referenced `cooling_state`, is non-decreasing through the highest referenced cooling state for each affected actuator, and each referenced non-zero entry is within bounds; conventionally `state_pwm[0] = 0`, but configs may choose a non-zero idle state. Shorter JSON `state_pwm` arrays are valid when no trip references the zero-padded tail. If present, `duty_linearization` has 2..`THERMAL_MAX_CURVE_POINTS` `[input_pwm, output_pwm]` pairs, starts at `[0,0]`, ends at `[255,255]`, and is monotonic in both axes. If `spinup_ms > 0`, `spinup_pwm` is non-zero and satisfies `pwm_min <= spinup_pwm <= pwm_max`. `min_on_ticks` and `min_off_ticks` are unsigned 16-bit tick counts; zero disables each dwell.
- A zone's `fallback_temp_mc` is at least the highest configured `CRITICAL` trip temperature; if the zone has no `CRITICAL` trip, fallback reaches the highest configured trip so loss of all valid sensors fails toward cooling rather than away from it.
- Modifier `pwm_cap` values are in `0..255` and either `0` or at least each affected actuator's `pwm_min`. Modifier trip/setpoint offsets are validated at every curve point so adjusted trips remain valid and adjusted PID setpoints remain within their configured bounds; runtime curve/trip/setpoint commands are checked against the same invariants.
- PID runtime bounds are present for every PID zone and satisfy `min <= current <= max` for gains and setpoints; `d_filter_alpha_q16` is in `[0, Q16_ONE]`.
- PID `dt_min_ms` and `dt_max_ms` are present for every PID zone and satisfy `0 < dt_min_ms <= control_period_ms <= dt_max_ms`.
- PID zones must have at least one `critical` or `shutdown` trip as a safety floor.
- v1 has exactly one policy modifier and it must be `acoustic_mask`; plural `policy_modifiers` exists for schema forward compatibility.
- Weighted sensor aggregation requires a `weights` array matching the sensor list length; weights use Q16.16 and must have a positive sum.
- Fault actions are one of the v1 actions in §4.7; action-specific required fields such as `fallback_temp_mc` are present.
- Expanded per-target fault detector instances fit within `THERMAL_MAX_FAULTS`.
- Optional `correlated_context` values for stuck-sensor detection resolve to a configured context signal. If a real context is configured, `correlated_delta_threshold` must be a positive integer. If `correlated_context` is `null` or absent, the detector runs in flatness-only mode and the correlated threshold is ignored.
- Trip points are ordered by temperature and hysteresis does not overlap neighboring trips.
- Trip `cooling_state < THERMAL_MAX_COOLING_STATES` and is valid for every affected actuator's `state_pwm` table.
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

Repository decision: keep `car-can-emulator` as a separate upstream repo and add it under `tools/car-can-emulator/` as a submodule. The submodule is pinned by commit SHA for reproducible white-paper builds; the upstream `v2-improvements` branch is the update source, not a floating dependency. `thermal-core` consumes only the CAN frames; scenario tooling may use the emulator's TCP port to set speed during tests.

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
TSIG_ZONE_AGG_VALID_SOC     0x0140
TSIG_ACTUATOR_PWM_MAIN_FAN  0x0200
TSIG_ACTUATOR_RPM_MAIN_FAN  0x0201
TSIG_PID_ERROR_SOC          0x0300
TSIG_PID_INTEGRAL_SOC       0x0301
TSIG_PID_DERIVATIVE_SOC     0x0302
TSIG_SPEED_KMH              0x0400
TSIG_MODIFIER_PWM_CAP       0x0401
TSIG_FAULT_STALL_COUNT      0x0600
```

Signals are opt-in via config — small MCUs can drop high-rate signals if needed.

`THERMAL_MAX_TELEMETRY_SIGNALS = 128` is sized so wildcard expansion can enable every v1 default signal range at once with headroom. If a future config exceeds it, validation fails clearly and the user narrows the selector set or raises the compile-time maximum.

Signal IDs are fixed by type range plus configured slot, not by debug-name hash. This lets `thermalcore-probe` decode a raw stream with only the config's slot order:

| Range | Meaning |
|---|---|
| `0x0100..0x01FF` | Zone temperatures, current cooling state, aggregation validity, and zone-level outputs, indexed by zone slot |
| `0x0200..0x02FF` | Actuator PWM/RPM/state, indexed by actuator slot |
| `0x0300..0x03FF` | PID terms, indexed as `0x0300 + zone * 4 + term`, with `term = 0 error, 1 integral, 2 derivative, 3 output` |
| `0x0400..0x04FF` | Context values, indexed by context slot |
| `0x0500..0x05FF` | Modifier outputs, indexed by modifier slot and output offset |
| `0x0600..0x06FF` | Fault detector counters/states, indexed by detector slot |
| `0x0700..0x07FF` | HIL-injected sensor temperature / tach RPM samples, indexed by slot (`TSIG_HIL_BASE`) |
| `0x0800..0x08FF` | Fan-health detector (post-v1, PRD Appendix C) — delta / severity / baseline-source / confidence, indexed by actuator slot (`TSIG_FAN_HEALTH_BASE`) |
| `0x0900..0x09FF` | Platform diagnostics emitted directly by platform telemetry sinks, not selected through core telemetry config (`TSIG_PLATFORM_BASE`) |

Telemetry selectors such as `zone_temp_*`, `zone_aggregation_valid_*`, and `actuator_pwm_*` are expanded once by the loader after zones, actuators, context signals, and PID terms are registered. The resulting signal IDs are stored in `thermal_telemetry_cfg_t.enabled_signal_ids`; the core checks that list before calling `telemetry_emit()`. Unknown exact names are invalid config; wildcard selectors that match nothing are warnings in development mode and errors in release configs. Selected signals are emitted at most once per control step. `telemetry.period_ticks` sets the global cadence; default `1` means every control tick, while higher values decimate low-priority streams. Per-signal telemetry dividers are future work if measured bandwidth requires them.

`TSIG_PLATFORM_CH32_TELEMETRY_DROPS` (`0x0900`) is a cumulative counter emitted by the CH32 UART telemetry tap when its nonblocking callback ring overflows. Captures with a non-zero value are lossy and should not be used as byte-complete evidence.

Discrete event codes are separate from telemetry signals and are defined in `core/thermal_events.h`. Required v1 event examples:

```
TEVENT_FAULT_ENTER          0x1000
TEVENT_FAULT_RECOVERING     0x1001
TEVENT_FAULT_CLEAR          0x1002
TEVENT_SAFETY_OVERRIDE      0x1100
TEVENT_SHUTDOWN_REQUEST     0x1101
TEVENT_COMMAND_APPLIED      0x1200
TEVENT_COMMAND_REJECTED     0x1201
TEVENT_ZONE_FALLBACK_ENTER  0x1300
TEVENT_ZONE_FALLBACK_EXIT   0x1301
TEVENT_PLATFORM_TICK_OVERRUN  0x1400
```

`TEVENT_PLATFORM_TICK_OVERRUN` is emitted by the Linux wall-clock daemon, not by `thermal_core_step()`. Its args are `(late_ms, behind_ticks, period_ms, threshold_ms)` and its timestamp is the scheduled tick time.

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

| Value | Opcode | Direction | Payload |
|---|---|---|---|
| `0x01` | `TELEM_SAMPLE` | device → host | `u16 signal_id, u16 flags, i32 value` |
| `0x02` | `TELEM_EVENT` | device → host | `u16 event_code, u32 a1, u32 a2, u32 a3, u32 a4` |
| `0x10` | `CMD_REQUEST` | host → device | `u16 command_id, u8 command_payload[]` |
| `0x11` | `CMD_ACK` | device → host | `u16 request_seq, u16 status, u32 detail_code` |
| `0x12` | `CMD_NACK` | device → host | `u16 request_seq, u16 status, u32 detail_code` |

Opcode values `0x00`, `0x03..0x0F`, and `0x13..0x7F` are reserved for future core transports; `0x80..0xFF` are reserved for platform/private experiments and must not appear in portable tests.

Every command receives an ACK or NACK. Invalid opcode, invalid payload length, out-of-bounds value, unavailable target, and rejected safety transition are distinct status codes. The ACK/NACK `status` field uses the pinned numeric values of `thermal_status_t` for core statuses, with transport-specific statuses allocated above `0x8000`. `detail_code` is a 32-bit command/result detail namespace defined in `core/thermal_commands.h` (typed semantics, no wire concerns) and is separate from the 16-bit actuator `reason` namespace used in `thermal_output_frame_t`. `seq` is monotonically incremented by the sender and wraps modulo 65536. ACK matching is against the outstanding `(transport, seq)` pair; host tools keep a small outstanding window and expire old requests by timeout so wraparound is unambiguous in normal use. Wire `seq` is transport state and is not part of `thermal_command_t`.

**Wire codec lives outside `core/`.** Portable binary frame encode/decode (TC magic, version, opcode, seq, length, payload, CRC) lives in `protocol/thermal_wire.h` and `protocol/thermal_wire.c`. The `protocol/` module is portable C99 with no heap and no platform deps — but it is *not* part of the thermal policy core. UDP daemons, USB-CDC firmware, replay tests, and tools all link `core/` + `protocol/` together. `core/` has no dependency on `protocol/` in either direction; a hypothetical embedder that uses the thermal policy library via direct C calls (e.g., a ctypes binding) does not need to link `protocol/` at all. Transport-level failures (bad CRC, invalid frame length, unknown frame opcode) are rejected inside `protocol/` before the typed `thermal_command_t` reaches `thermal_core_apply_command()`. Semantic failures (unknown `command_id`, malformed command payload for a known command, out-of-bounds values, unknown target IDs) are reported through `thermal_core_apply_command()` and encoded by the caller as `CMD_NACK` via the `protocol/` helpers.

**Single source of truth for command IDs.** `thermal_command_id_t` in `core/thermal_commands.h` is the authoritative enum for command-ID numeric values. `protocol/thermal_wire_opcodes.h` defines only frame opcodes, transport status codes (above 0x8000), and per-platform receive caps — it must include `core/thermal_commands.h` and may add `_Static_assert` checks that wire encoders use the same numeric values, but it does not redeclare those constants.

### 7.3 Per-platform transport

- **Linux**: telemetry UDP packets to `127.0.0.1:9001` (configurable) using the `TC` binary frame. The command listener is separate and disabled unless `control.enable = true`; when enabled for v1 it binds only to loopback, default `127.0.0.1:9002`. CSV to stdout/file is available for simple logs.
- **ESP32**: USB-CDC binary frame stream by default; optionally UDP-over-WiFi for untethered tests.
- **RH850 (future)**: UART or CAN with same frame format.

### 7.4 Host-side tool: `thermalcore-probe`

A Python tool that reads the daemon's UDP telemetry stream and logs it to the canonical CSV (`thermalcore-probe --listen udp:HOST:PORT --log run.csv`). The scenario runner imports its `ProbeRecorder` as a library so host scenarios record telemetry without a second process.

**v1 scope:** the UDP source plus `--log` only. Serial and file sources, live plotting (`--live`), and figure generation (`--plot`) are deferred. The white-paper figures are regenerated by the Stage 16 figure pipeline — `make -C docs/paper figures` runs one matplotlib plot script per figure under `docs/paper/figures/plots/` — not by the probe.

### 7.5 Control plane (runtime tuning)

Bidirectional: telemetry frames go host-ward, command frames go device-ward. Same framing, distinguished by the opcode field. On Linux v1, command ingress is loopback-only, unauthenticated, and intended for development/bench use. It must not bind to a non-loopback address unless a deliberate unsafe-development flag is set. Production packaging may compile out the command listener entirely.

Portable command IDs carried inside `CMD_REQUEST`:

| Value | Command | Payload |
|---|---|---|
| `0x0001` | `CMD_SET_PID` | `u16 zone_id, i32 kp_q16, i32 ki_q16, i32 kd_q16` |
| `0x0002` | `CMD_SET_SETPOINT` | `u16 zone_id, i32 setpoint_mc` |
| `0x0003` | `CMD_SET_TRIP` | `u16 zone_id, u16 trip_idx, i32 temp_mc, i32 hyst_mc` |
| `0x0004` | `CMD_SET_CURVE_POINT` | `u16 modifier_id, u16 point_idx, i32 x, i32 value0, i32 value1` |
| `0x0005` | `CMD_CLEAR_FAULT` | `u16 fault_type, u16 target_id` |

Wire command IDs are the numeric values of `thermal_command_id_t`.

A companion CLI tool `thermalcore-tune` issues commands. This is what makes the bench rig usable for actual loop tuning: change `kp`, observe the step response in the logged telemetry CSV, change again, in seconds. No rebuild, no reflash. (A live plot was envisaged via `thermalcore-probe --live`; that is deferred — see §7.4.)

Commands respect compile-time bounds — `kp` cannot be set outside the per-zone bounds defined in the config. This is intentional; runtime tuning is for the bench, not for the field, and bound-checking is cheap insurance. `CMD_SET_TRIP` changes only `temp_mc` and `hyst_mc`; trip `severity` and `cooling_state` are config-time safety semantics in v1. `CMD_SET_CURVE_POINT` uses `value0`/`value1` so the acoustic-mask curve can update both PWM cap and trip offset atomically; single-output curves set `value1 = 0`. v1 has one curve per modifier; future multi-curve modifiers require a new command or payload version. `CMD_SET_PID` applies all three gains atomically and resets the affected zone's integral accumulator and derivative history to avoid a long unwind after gain changes. PID gain updates are accepted while a fault override is active, but the active fault still controls actuator output until it recovers. `CMD_CLEAR_FAULT` follows the latching contract in §4.7: it clears a latched fault only after the detector condition has ended and per-detector recovery criteria such as `recovery_ticks` have passed.

Unknown `command_id` values return `THERMAL_ERR_INVALID_ARG`. `CMD_CLEAR_FAULT` for a `(fault_type, target_id)` pair that does not correspond to a configured detector instance also returns `THERMAL_ERR_INVALID_ARG`; if the detector exists but is not clearable yet, it returns `THERMAL_ERR_REJECTED_SAFETY`.

Status-code split for commands: `THERMAL_ERR_INVALID_ARG` means malformed command, unknown command ID, unknown target ID, or missing required field. `THERMAL_ERR_BOUNDS` means the command and target are valid but the requested value is outside configured bounds, including gain/setpoint limits, invalid `trip_idx`/`point_idx`, or a curve edit that would break monotonicity.

Platform/test commands are separate from core commands:

| Value | Command | Payload |
|---|---|---|
| `0x8001` | `TEST_INJECT_FAULT` | `u16 fault_type, u16 target_id, u8 on` |
| `0x8002` | `TEST_FREEZE_INPUT` | `u16 sample_id, u8 kind, i32 fixed_value` |
| `0x8003` | `TEST_RESUME_INPUT` | `u16 sample_id, u8 kind` |
| `0x8004` | `TEST_SET_CONTEXT` | `u16 context_id, i32 value` |

The Linux daemon and ESP32 HIL firmware may implement these test commands. Production builds may compile them out. Command IDs `0x8000..0xFFFF` are platform/test command IDs inside the `CMD_REQUEST` payload and do not conflict with frame opcode values `0x80..0xFF`. `TEST_FREEZE_INPUT` delivers the frozen sample to the core with `valid = 1` until `TEST_RESUME_INPUT`; stale/invalid sample injection is a separate scenario action.

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

The scenario runner drives each scenario, captures its telemetry to a canonical CSV, and evaluates the assertions to a pass/fail result. Scenario reproducibility is enforced by the determinism gate (`make determinism`): the captured CSV must be byte-identical run-to-run and across the GCC and Clang toolchains, compared by SHA-256. A per-scenario provenance sidecar (command log, git SHA, tool versions) is not produced in v1.

A canonical config-hash encoding — all `thermal_config_t` fields packed in declaration order, strings null-terminated, unused tail bytes zeroed before hashing — is implemented in `support/thermal_config_hash.c` and verified by the `json2static` round-trip test (the JSON-loader path and the static-config path must hash equal). The digest is scoped to the compiled feature profile: feature-gated fields such as PID-only actuator/PID extensions or fan-health baselines are encoded only when that feature is compiled in, so cross-profile comparisons must include the feature-profile identity. It is not, in v1, stored beside scenario artifacts; the white-paper figure manifest records a config-file SHA-256 over the JSON bytes instead (§9.2).

## 8. Reference Bench Rig (pinned)

The bench rig is part of the deliverable. Anyone with the BOM and the wiring diagram in `docs/bench-rig.md` should be able to reproduce the results in the white paper.

v1 does not require a physical heat-injection plant to be considered complete. The release-gating plant is the deterministic host simulator described in §9.3 plus real fan PWM/tach validation on the bench. The resistor + MOSFET heater is retained as an optional validation aid and a useful white-paper photograph/demo if time permits.

### 8.1 Bill of materials

| Item | Qty | Notes |
|---|---|---|
| ESP32-C3 DevKitC-1 | 1 | Primary MCU; supports TWAI (CAN) for OBD-II, plenty of LEDC/GPIO |
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

`HIL_PERIPHERAL` uses the same `TC` binary framing over USB-CDC. ESP32-to-Linux sensor, tach, and decoded CAN samples use `TELEM_SAMPLE` frames with HIL-specific signal IDs; Linux-to-ESP32 PWM writes use `CMD_REQUEST` with platform/private command IDs in the `0x8000..0xFFFF` range. No second ad-hoc serial protocol is introduced for HIL.

## 9. Test Scenarios and Benchmarks

All scenarios are scripted (`scenarios/*.scn`) and run on three rigs: pure unit-test sim, Linux-with-mock-tmpfs, and Linux-with-ESP32-HIL plus the standalone ESP32 build for cross-validation. The simulator and mock-tmpfs rigs produce deterministic, bit-for-bit reproducible outputs; physical rigs (Linux+ESP32-HIL, ESP32 standalone on real hardware) run the same scripts but are evaluated against behavioral tolerance bands rather than byte-equal SHA, because real tach jitter, USB-CDC latency, and CAN frame timing vary tick-to-tick.

### 9.1 Canonical scenarios

| Scenario | Description |
|---|---|
| `idle_steady_state` | All zones at ambient; verify no fan activity, no false faults over 5 min. |
| `heat_soak_ramp` | SoC temperature ramps from 45°C to 90°C over 60 s; capture fan response, settling time, overshoot. |
| `step_load` | SoC temperature steps from 50°C to 85°C; capture step response, used for PID tuning. |
| `fan_stall_recovery` | Force fan tach to 0 while PWM > 0; verify stall fault raised within configured `persist_ticks`, recover when tach restored. |
| `stuck_sensor` | Freeze SoC sensor at 50°C while injecting load; verify stuck-sensor fault, fallback behavior. |
| `stuck_sensor_correlated` | Freeze SoC sensor while stepping a configured context by more than `correlated_delta_threshold`; verifies the correlated stuck-sensor gate. |
| `acoustic_mask_low_speed` | Vehicle speed = 0; verify PWM cap applied; temperature trends with cap engaged. |
| `acoustic_mask_high_speed` | Vehicle speed sweep 0 → 130 km/h; verify cap releases, trip offset applied. |
| `multi_zone_coupling` | Amp and tuner zones heat simultaneously, shared fan; verify v1 max-wins arbitration. This does not exercise multi-actuator load sharing, which is outside v1 scope. |
| `runaway` | PWM forced to 0 (actuator failure), temperature rising; verify runaway fault. |
| `can_bus_loss` | Cut CAN; verify fail-safe to `assume_stationary` after the configured context `timeout_ms`. |

### 9.2 Benchmarks committed to the white paper

| Benchmark | Target |
|---|---|
| Step time per `thermal_core_step()` call | Linux: sub-100 µs; ESP32-C3: sub-1 ms; budget for RH850-F1KM: sub-500 µs |
| Memory footprint (`.text`/`.bss`/`.data`) | Linux: not bounded (informational); ESP32: ≤ 64 KB text, ≤ 16 KB bss; RH850 budget: ≤ 32 KB text, ≤ 8 KB bss |
| Settling time (`step_load`) | Reported per platform, per governor |
| Overshoot (`step_load`) | Reported per platform, per governor |
| Fan PWM-seconds integral (acoustic proxy) | Reported with vs without `acoustic_mask`; quantifies acoustic benefit |
| Fault detection latency | `fan_stall_recovery`: stall raised within ≤ 3 s |

All benchmark figures are regenerated from scenario CSV logs by `make -C docs/paper figures`. Each figure caption cites the scenario name and the data SHA recorded in `docs/paper/figures/manifest.yaml`. The manifest entry holds the telemetry-CSV SHA-256, the config-file SHA-256, the source git SHA used to generate the data, and tool versions. (The config hash is a SHA-256 over the JSON config file bytes; a canonical post-validation hash was not built for the figure pipeline since the telemetry-CSV SHA is the load-bearing freshness signal.) Captions point at the manifest data SHA rather than the current commit SHA, so text-only paper edits do not mark every figure stale.

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
- fan effectiveness curve using the same strictly increasing point-list syntax as policy curves (`x`, `value0`, optional `value1`);
- optional zone-to-zone coupling coefficient;
- sensor noise, dropout, freeze, and stale-sample injection;
- actuator failure modes such as forced PWM, missing tach, and low tach.

The simulator must be bit-for-bit deterministic for a given config, scenario, and git SHA. All simulator state and coefficients use integer units or Q16.16 fixed-point math; host floating point, platform math-library behavior, and `-ffast-math` are not part of the v1 simulator contract. Any configured noise source uses a deterministic PRNG with an explicit scenario seed. Physical heater tests may be added later, but the first implementation should not depend on analog heat injection to validate basic control behavior.

## 10. Repository Layout

```
thermal-core/
├── README.md
├── LICENSE                              MIT
├── core/                                 Pure C99, no platform deps, no protocol deps
│   ├── thermal_core.c
│   ├── thermal_core.h
│   ├── thermal_zone.c
│   ├── thermal_curve.c
│   ├── thermal_pid.c
│   ├── thermal_governor.c
│   ├── thermal_fault.c
│   ├── thermal_modifier.c               Acoustic-mask and future modifiers
│   ├── thermal_commands.c               Typed command application, detail-code enum
│   ├── thermal_commands.h
│   ├── thermal_types.h
│   ├── thermal_platform.h               snapshot/callback interface
│   ├── thermal_signals.h                Telemetry signal IDs
│   ├── thermal_events.h                 Event codes
│   └── thermal_config.h                 Compile-time limits
├── protocol/                             Portable C99 binary wire codec; depends on core types only
│   ├── thermal_wire.c                   TC frame encode/decode, CRC-16/CCITT-FALSE
│   ├── thermal_wire.h
│   ├── thermal_wire_opcodes.h           Frame opcode values, transport status codes, receive caps (NO command IDs — those live in core/thermal_commands.h)
│   └── thermal_wire_crc.c               CRC implementation
├── support/                              Portable C99 reproducibility helpers; depend on core types only, not linked into the minimal control library
│   ├── thermal_config_hash.c            Field-by-field canonical encoder + SHA-256 for thermal_config_t
│   └── thermal_config_hash.h
├── platform/
│   ├── linux/
│   │   ├── thermalcored.c               Main daemon
│   │   ├── bsp_hwmon.c
│   │   ├── bsp_mock_tmpfs.c             hwmon-like tmpfs backend for rootless tests
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
│   ├── stuck_sensor_correlated.scn
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
- **Reproducibility**: benchmark logs and figures are tracked in `docs/paper/figures/manifest.yaml` by data SHA-256; benchmark log filenames may embed the source git SHA as a convenience, but freshness is verified against the manifest entry. Figures regenerated by `make -C docs/paper figures`. The white paper build (`make -C docs/paper`) regenerates all plots from logs in `docs/paper/figures/plots/data/`.

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

Current source mapping:

| Source file | `thermal-core` content |
|---|---|
| `01-introduction.tex` | IVI thermal motivation, generic core boundary, project goals |
| `01b-background.tex` | Related work and scope boundaries |
| `02-environments.tex` | thermal operating environments: ambient ranges, idle cabin, moving vehicle, heat soak |
| `03-human-vision.tex` | acoustic perception / cabin noise masking |
| `03b-control-model.tex` | closed-loop model, PID law, safety floors, fault interactions |
| `04-zone-mapping.tex` | thermal zone modeling and sensor aggregation |
| `05-curves.tex` | fan curves, trip curves, vehicle-speed policy curves |
| `06-response-time.tex` | loop timing, slew limits, PID step response, stability considerations |
| `07-architecture.tex` | three-layer architecture, snapshot API, and callback surface |
| `08-components.tex` | sensors, actuators, BSP backends, fault detectors |
| `09-zone-controller.tex` | governors, arbitration, policy modifiers |
| `10-json-config.tex` | Linux JSON schema and generated static MCU config |
| `11-control-interface.tex` | telemetry, runtime tuning, scenario commands |
| `12-deployment.tex` | Linux daemon, ESP32 modes, `car-can-emulator`, systemd/OpenWrt notes |
| `12b-evaluation.tex` | scenario plots, CI/coverage, CH32 bring-up and fan-health measurements |
| `13-summary.tex` | results summary, recommendations, limitations, future work |
| `appendices.tex` | full configs, OBD-II frame reference, BOM, build reference |

The `Makefile` in `docs/paper/` codifies the build:

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
- **PWM-fan actuator backend only.** The control model borrows the generic cooling-device vocabulary, but v1's implemented actuator is a PWM fan. Heat-source throttling, heating, pumps, reversed polarity, and bidirectional devices are future actuator-kind work, not hidden v1 behavior.
- **Single speed signal, no other vehicle context.** Ignition state, drive mode, ambient temperature, HVAC state, and workload/power would all matter in a production policy; v1 demonstrates the modifier mechanism with one input. PID is feedback-only in v1; no load/power feed-forward term is implemented.
- **Only one policy modifier in v1.** The schema uses an array for forward compatibility, but validation rejects `modifier_count > 1`. The future composition rule is pinned above; it is not implemented by v1 code.
- **No closed-loop acoustic measurement.** PWM-seconds is used as an acoustic proxy. Real SPL measurement with a calibrated microphone is future work.
- **Fixed-point PID may not match a floating-point reference exactly.** Verified against a pure-integer Q16.16 reference in unit tests; an optional scipy/float comparison runs nightly or as a design sanity check. Production deployments should re-validate gains.
- **Single control thread.** Cooperative model. A long sensor read could disturb timing. Watchdog patterns are in scope, full preemptive multi-threading is not.

## 16. Future Work

- **PID autotune** (relay-feedback / Åström-Hägglund).
- **NVS-persisted learned gains**, with safe-rollback.
- **Full RH850-F1KM port** with hardware-validated benchmarks.
- **AUTOSAR Adaptive integration** as a separate package.
- **DLT logging adapter** for production observability.
- **Calibrated SPL measurement** to replace the PWM-seconds proxy.
- **General thermal-device actuator kinds**: throttling/capping devices, heaters, pumps, inverted polarity, and bidirectional devices behind an explicit actuator-kind contract.
- **Multi-input policy modifiers**: ignition, ambient, drive mode, HVAC, with deterministic composition (`min` caps, bounded summed offsets, configured order, per-modifier telemetry).
- **Load/power context and feed-forward control**: optional PID feed-forward and stuck-sensor correlation from a workload or power estimate.
- **CAN DBC integration** for OEM-private speed signals.
- **Multi-fan coordinated control** with acoustic cancellation considerations (out-of-phase PWM, RPM detuning).
- **Functional-safety scaffolding** (MISRA-C, static analysis CI) as a path toward ASIL-B/C readiness.
- **Web-based live dashboard** built on the telemetry UDP stream (separate tool, not in core repo).
- **Fan-health / degradation detection** — graded PWM-to-RPM drift versus a calibrated baseline for predictive maintenance; see Appendix C for the post-v1 minimal-slice sketch.
- **CH32V003 STANDALONE port** — the portable core running self-contained on a ~10-cent 16 KB / 2 KB RV32EC microcontroller; see Appendix D for the post-v1 sketch.

## 17. Decision Log and Remaining Questions

### 17.1 Decisions

1. **Repo name:** `thermal-core` is the project/repo name; `thermalcored` is the Linux daemon binary.
2. **Core boundary:** `thermal-core` stays protocol-agnostic. CAN, OBD-II, SocketCAN, JSON, sysfs, and emulator TCP commands remain in platform or tooling layers.
3. **OBD-II emulator ownership:** `car-can-emulator` stays a separate upstream repo and is consumed as a git submodule at `tools/car-can-emulator/`, pinned by commit SHA and updated from branch `v2-improvements`.
4. **LaTeX migration:** the template is migrated early into `docs/paper/`; `docs/paper/src/thermal-core-spec.tex` is the active root.
5. **PID numeric default:** Q16.16 fixed-point remains the default; floating-point PID may be an optional build flag.
6. **Core API shape:** the core consumes input snapshots and returns output frames; platform code owns all blocking I/O.
7. **v1 plant model:** deterministic first-order simulation plus real fan PWM/tach validation is the v1 release gate. Physical heat injection is optional.
8. **Core storage contract:** `thermal_core_t` is caller-owned fixed-size storage; snapshots are borrowed only for the duration of `thermal_core_step()`, and output frames are full actuator snapshots every tick.
9. **Curve math:** v1 curves use integer linear interpolation with endpoint clamping and no floating point.
10. **Transport contract:** binary telemetry/control frames are little-endian, manually encoded, CRC-16/CCITT-FALSE when CRC is enabled, and receive-buffer capped per platform.
11. **Linux control ingress:** runtime tuning over UDP is loopback-only, config-gated, and bench/development scoped in v1.
12. **Latched fault reset:** `CMD_CLEAR_FAULT` is the explicit reset path for latched v1 faults when recovery criteria are already satisfied; production builds may compile out remote clearing.
13. **Wire IDs:** v1 assigns fixed opcode and command-ID values before implementation so Linux tools, ESP32 firmware, and future ports interoperate.
14. **Safety slew:** upward safety overrides bypass normal slew limits; normal cooling changes, recovery, and downward transitions still obey slew.
15. **Callback namespaces:** continuous telemetry signals and discrete event codes are separate numeric namespaces.
16. **HIL framing:** ESP32 HIL peripheral mode uses the same `TC` binary frame over USB-CDC; no separate ad-hoc serial protocol.
17. **Runtime command API:** public commands use a typed `thermal_command_t` tagged union; results use `thermal_command_result_t` with `thermal_status_t` plus a 32-bit detail code.
18. **State inspection API:** `thermal_core_get_state()` returns bounded arrays of zone, actuator, fault, context, and modifier state for tools and replay tests.
19. **PID trip semantics:** PID is the normal controller for PID zones; trips provide telemetry plus safety floors/maximum cooling for critical and shutdown states.
20. **Signal ID allocation:** telemetry IDs are fixed by type range and configured slot, not by debug-name hash.
21. **Threading model:** v1 core API calls are serialized through the control thread/task; Linux drains UDP commands between control ticks.
22. **Config API shape:** `thermal_config_t` and sub-structs are bounded C arrays generated from JSON on Linux or authored as static const data on MCU targets.
23. **Fault instances:** v1 fault configuration supplies global defaults per detector type, expanded into per-target detector instances at init.
24. **PWM clamp semantics:** non-zero actuator requests clamp into `[pwm_min, pwm_max]`; `0` remains the explicit off command.
25. **Runtime trip tuning:** v1 runtime trip commands may change threshold and hysteresis only; severity and cooling-state remain config-time safety semantics.
26. **Enum namespaces:** all loader-facing `uint8_t` config fields have pinned enum values for JSON parsing and static config generation.
27. **Command timestamps:** `thermal_core_apply_command()` takes `now_ms` explicitly so command events have deterministic timestamps without relying on the previous control tick.
28. **Command decoding boundary:** portable binary frame encode/decode lives in `protocol/thermal_wire.{c,h}`, outside `core/`. Transports use the `protocol/` helpers; semantic command validation stays in `thermal_core_apply_command()`. `core/` has no protocol dependency.
29. **Runaway math:** v1 runaway detection compares temperature rise across the persistence window while commanded PWM remains above the configured cooling threshold.
30. **Aggregation degradation:** zone aggregation uses valid samples only; all-invalid zones fall back or enter degraded behavior.
31. **Curve edit safety:** runtime curve edits are rejected if they break strict x-axis monotonicity.
32. **IIR filter convention:** single-pole Q16.16 low-pass with `filtered_next = filtered_prev + alpha_q16 * (sample - filtered_prev)`; `alpha_q16 = 0` holds the previous value, `alpha_q16 = Q16_ONE` passes the sample through. Applies identically to sensor and context-signal filters.
33. **Filter validity lifecycle:** first valid sample initializes `filtered_value` directly to the sample; invalid samples never advance filter arithmetic and clear the filter's `valid` flag while preserving the last numeric value; aggregation skips invalid sensors; context fail-safes decide whether held values stay policy-active.
34. **Wire codec location:** binary frame codec (TC framing, CRC, opcode dispatch) lives in `protocol/`, not `core/`. `core/` is protocol-agnostic by construction; transports link `core/` plus `protocol/`. Reverses an earlier draft assumption that put the codec in core helper code.

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

## Appendix C: Fan-Health Detector (Post-v1 Sketch)

This appendix sketches a **post-v1** feature: a fan-health (fan-aging / contamination) detector. It is recorded here so the design is settled before implementation begins; it is **not** part of the v1 scope and affects no v1 gate. The full predictive-maintenance feature is larger than what is sketched here — this appendix deliberately scopes only the **minimal core-worthy slice**, the part that belongs in `core/`. Baseline capture, persistence, and the host CLI are platform and tooling work, scoped separately.

### C.1 Concept

A 4-wire fan's RPM at a given PWM duty drifts predictably as the fan ages. Bearing wear, lubricant breakdown, and motor degradation add mechanical or electrical load and so **lower RPM** at a fixed PWM; blade and heatsink dust that loads the fan aerodynamically does the same. Capturing a known-good **PWM-to-RPM baseline** once — at the factory, at first boot, or at a service event — and comparing in-field operation against it detects this **negative RPM drift** *weeks or months before stall*, while service is convenient rather than urgent.

The slice detects negative drift only. Restricted or blocked airflow is deliberately **not** in scope: it does not reliably lower RPM — depending on where the restriction sits, it can *unload* the fan aerodynamically and *raise* RPM at the same PWM — so blocked-intake diagnosis is a separate positive-delta-plus-temperature feature, deferred (§C.6). A positive delta — RPM *above* baseline — is therefore never read as "health improvement": it is clamped to zero in the aggregate (§C.3), so it can neither raise the health score nor cancel a negative-drift reading at another operating point.

This is a **predictive-maintenance** feature, not a safety feature. It is **advisory-only**: it emits health telemetry and never commands an actuator. Thermal safety remains entirely with the v1 stall and runaway detectors.

### C.2 Relationship to the v1 stall detector

Fan-health and the stall detector sit at opposite ends of one spectrum: the stall detector raises a binary fault at the *terminal* failure (RPM near zero at high PWM); fan-health grades the *approach* to that failure. Fan-health is the early-warning companion to stall detection, reading tach data the core already has in hand — but **not the identical input**. The stall detector evaluates the *requested/arbitrated* PWM, before clamp, slew, and fault override, because it answers "did we ask hard enough that low tach is unsafe?". Fan-health answers a different question — "is the measured RPM low for the PWM the fan was *actually* driven with?" — so it must compare the tach sample against the **final applied `duty_0_255`** that produced it, not the pre-slew/pre-fault request `thermal_fault_stall_step()` uses. And because tach is sampled at the start of a tick, before that tick's new command is applied, the reading reflects the *previous* tick's applied duty; fan-health aligns to that one-period lag. Comparing current tach against the current requested duty would manufacture false degradation through every ramp, slew-limited transition, manual command, and fault override — which is why the steady-window gate (§C.3) skips exactly those situations.

It is **not** a fifth `thermal_fault` detector. The v1 fault detectors share `thermal_fault_detector_cfg_t` (two generic thresholds, an `action`, and the `NORMAL → … → LATCHED` FSM). Fan-health has no `action` (it is advisory-only), needs an N-point baseline table rather than two scalar thresholds, and produces a graded result rather than a binary state. It is therefore its own module, and the frozen v1 `thermal_fault_detection_cfg_t` is left untouched.

### C.3 The minimal core-worthy slice

A new compile-optional core module, `core/thermal_fan_health.{c,h}`, sibling to `core/thermal_fault.c`. It obeys every core constraint — no heap, no syscalls, deterministic fixed-point math, fixed-size state, snapshot-driven — so it passes the same `no-heap-no-syscall` portability gate and is replay-testable like every other core module.

**Baseline as configuration.** The per-actuator PWM-to-RPM baseline is supplied to the core as `const` configuration data, exactly as `thermal_config_t` is. The core never loads, captures, or persists the baseline — that is the platform's job. For the reference bench the baseline is hand-authored in the JSON config and `json2static.py` emits it as a `const` struct, identical to how `G_THERMAL_CFG` is produced. This single decision is what keeps the slice inside the core boundary: no file I/O, no NVS, no storage format in `core/`.

**Baseline sourcing and provenance.** A baseline is only as good as the fan it was measured on. The platform selects one by priority: a **field- or factory-calibrated** sweep of *this exact unit* (highest priority — it measures true aging from this fan's own known-good state); failing that, a **model-generic** baseline — a sweep of a golden sample of the same fan model, or, as a thin last resort, the manufacturer datasheet curve; failing both, no baseline is supplied and the detector is disabled for that actuator. A golden-sample sweep is strongly preferred over datasheet transcription, since fan datasheets rarely publish a clean PWM-to-RPM table. The chosen baseline carries a `source` tag — `field`, `factory`, or `model` — into the `const` config; selecting which file to load is platform I/O and stays outside `core/`. Sweeps captured in raw tach ticks are normalized to RPM at config-generation time, matching the core's `tach_rpm` convention (Appendix A).

A baseline is also only comparable to runtime under matching electrical and mechanical conditions: RPM at a fixed PWM shifts with PWM carrier frequency, supply-voltage droop, tach pulses-per-revolution, fan-model substitution, and enclosure back-pressure — none of which is aging. The `const` config the core consumes carries only the normalized RPM points and the `source` enum. The compatibility metadata — a `fan_model` identity string plus the capture conditions — lives in the JSON config and is the platform tooling's responsibility to record and check at config-generation time; `core/` neither sees nor interprets it. This keeps the core input compact while preventing a baseline captured under one condition from being treated as authoritative under another.

**Per-tick behavior (opportunistic monitoring).** No active fan cycling, no governor override:

1. **Gate the tick.** Skip this actuator's update entirely when the sample is not representative of free-running steady operation: `tach_valid` is 0, no tach source is configured, the applied PWM is below the baseline's lowest point, a spin-up grace window is active, the slew limiter was active over the sample window, or a fault force-max/shutdown override was active. A skipped tick updates no detector state — it is simply not evidence about aging. The previously published `health_delta_pct` / `severity` / `confidence` stand until the next steady sample refreshes them: telemetry, like the v1 fault-state signals, has no per-tick suppression, so a consumer reads `confidence` to judge how much evidence the current result rests on.
2. **Require a steady window.** Track whether the applied PWM has held within `stable_pwm_tolerance` for `stable_pwm_ticks`, and the tach RPM within `stable_rpm_tolerance_pct` for the (shorter) `stable_rpm_ticks`. Only a tick that clears both windows yields a delta; the applied PWM used is the one that produced the current tach sample (the previous tick's final `duty_0_255` — see §C.2).
3. **Compute the per-point delta by interpolation.** Linearly interpolate the *expected* RPM for the current applied PWM between the two adjacent baseline points — the same integer-interpolation discipline as `thermal_curve.c`, with endpoint clamping — so an arbitrary steady governor duty still yields a delta rather than only the exact baseline PWMs. Then `delta_pct = (measured − expected) × 100 / expected`: a **signed whole percent**, negative meaning under baseline, rounded half-away-from-zero, saturated to a documented range. This delta updates the nearest baseline point's accumulator.
4. **Fold into per-point confidence.** Each baseline point carries a fixed-size EMA accumulator and an observed-sample count. A new delta updates the accumulator for its nearest point; while the fan keeps being sampled, stale per-point observations decay naturally through the EMA, so no explicit per-observation TTL is needed. The aggregate `health_delta_pct` is the confidence-weighted mean of the per-point accumulators **clamped to their non-positive part** — a point reading *above* baseline contributes zero, never a positive offset, so positive drift can neither raise the score nor cancel negative drift at another point (§C.1). `health_delta_pct` is therefore a negative-drift score, ≤ 0. Mid-range PWM points are weighted most heavily — that is where signal-to-noise is best (near stall the fan is noisy; at 100% it is voltage-limited).
5. **Classify severity.** Map `health_delta_pct` to a `severity` — HEALTHY / AGING / DEGRADED / FAILING — through configurable signed thresholds, but report nothing stronger than HEALTHY until at least `min_points_observed` baseline points have each accumulated enough samples: an opportunistic detector must not escalate on one lucky reading. Resolution is further gated by baseline provenance (below).
6. **Emit.** `health_delta_pct`, `severity`, `baseline_source`, and a `confidence` figure per fan through the existing telemetry callback. No actuator command is ever produced.

State is one fixed-size struct per actuator, sized at `THERMAL_MAX_ACTUATORS`, holding the per-baseline-point EMA accumulators and observed-sample counts. The per-point arrays are bounded by a new compile-time constant `THERMAL_MAX_FAN_HEALTH_POINTS` (default 8, matching `THERMAL_MAX_CURVE_POINTS`); config validation (§C.4) rejects any baseline larger than that.

**Severity resolution by baseline provenance.** A unit-specific baseline (`field` or `factory`) measures drift of this fan from its *own* new condition, so the full `HEALTHY / AGING / DEGRADED / FAILING` ladder is meaningful. A model-generic baseline (`model`) cannot — it conflates aging with unit-to-unit manufacturing variance, itself a few percent, so a brand-new fan could legitimately read several percent off the golden sample. With a `model` baseline the detector therefore asserts only the coarse `DEGRADED` / `FAILING` end — gross degradation that exceeds any plausible manufacturing spread — and suppresses `AGING`. The emitted `severity` is always paired with the `baseline_source` signal (§C.5) so a consumer never mistakes model-deviation for unit-aging.

### C.4 Configuration surface

A per-actuator `fan_health` block, optional (absent → the detector is disabled for that actuator):

```json
"fan_health": {
  "enable": true,
  "fan_model": "noctua-nf-a8-pwm",
  "baseline_source": "field",
  "baseline": [[64,900],[96,1400],[128,1850],[160,2200],[192,2500],[255,2900]],
  "stable_pwm_ticks": 300,
  "stable_pwm_tolerance": 2,
  "stable_rpm_ticks": 50,
  "stable_rpm_tolerance_pct": 5,
  "min_points_observed": 3,
  "severity_pct": { "aging": -5, "degraded": -15, "failing": -30 }
}
```

`baseline` is the PWM-to-RPM table; `baseline_source` is `field`, `factory`, or `model` and gates severity resolution (§C.3); `fan_model` is the compatibility identity string (recorded and checked by tooling, not the core — §C.3). `stable_pwm_tolerance` (0–255 PWM counts) and `stable_rpm_tolerance_pct` (whole percent) define the steady-window gate; `min_points_observed` is the confidence floor. On MCU targets the same fields are populated through the generated static `const` config.

**Config validation.** Because the detector is baseline-driven, a malformed baseline reads as real degradation — so `tools/json2static.py` and the Linux JSON loader reject an ambiguous or physically implausible block at config-generation / load time rather than at runtime. The rules:

- An absent `fan_health` block, or `enable: false`, disables the detector for that actuator and requires no further fields.
- `enable: true` requires a non-empty `baseline` and a valid `baseline_source` (`field` / `factory` / `model`).
- The baseline point count is between 2 and `THERMAL_MAX_FAN_HEALTH_POINTS`.
- PWM values are sorted strictly ascending, unique, and within `0..255`; RPM values are positive and within `uint16_t`.
- The baseline is monotonic non-decreasing in RPM (RPM rises with PWM).
- The baseline's highest PWM point reaches at least the actuator's `pwm_max`, so high-duty operation is never scored against a clamped (below-range) expected RPM.
- `severity_pct` thresholds are strictly monotonic and within the signed-percent range: `0 > aging > degraded > failing`.
- Stability tick counts are non-zero; tolerance fields are present (no silent defaulting of a value that changes detector behavior) and in range — `stable_rpm_tolerance_pct` is `0..100`, `stable_pwm_tolerance` is `0..255`.
- A `fan_health` block on a tachless actuator is rejected — the detector has no RPM to compare.

### C.5 Telemetry signals

Per-fan signals in a dedicated namespace, **`TSIG_FAN_HEALTH_BASE = 0x0800`** — `0x0700` is no longer free, `TSIG_HIL_BASE` already owns it, so the range is reserved at `0x0800` to avoid collision with HIL or future platform signals. Four per-actuator signals, allocated per the signal-ID convention (§17.1 decision 20) and added to `core/thermal_signals.h` as part of Stage 17:

- `TSIG_FAN_HEALTH_DELTA(slot)` — the signed `health_delta_pct`.
- `TSIG_FAN_HEALTH_SEVERITY(slot)` — HEALTHY / AGING / DEGRADED / FAILING.
- `TSIG_FAN_HEALTH_BASELINE_SOURCE(slot)` — `field` / `factory` / `model` for an enabled detector, or `none` when fan-health is disabled for that actuator (so a consumer never mistakes a zeroed slot for a real `field` baseline). It tells the consumer whether it is reading unit-aging or model-deviation.
- `TSIG_FAN_HEALTH_CONFIDENCE(slot)` — the observed-coverage figure, which makes the opportunistic, slowly-accumulating nature of the detector legible to host tools (a low-confidence HEALTHY is "not enough evidence yet", not "verified healthy"). It counts *how many* baseline points have accumulated samples — it is **coverage, not recency**: because a skipped tick leaves the published result standing (§C.3), a confident `severity` can persist unchanged through an arbitrarily long off / tach-invalid period, and a consumer must not read `confidence` as a freshness or staleness indicator.

### C.6 Scope boundary — what is NOT in the slice

Deliberately deferred to a separate, larger post-v1 effort:

- **Baseline capture** — the active low-to-high PWM sweep with the governor disabled, plus the scenario-runner directives to drive it.
- **Baseline persistence** — NVS (ESP32) / JSON-file (Linux) load and save, and the cross-platform blob format and CRC.
- **The `thermalcore-fanhealth` host CLI** — capture, inspect, diff, live view.
- **Scheduled-probe mode** — a periodic active mini-sweep.
- **BLOCKED detection** — a *positive* delta (RPM above baseline) cross-correlated with rising zone temperatures to flag a blocked intake. That needs temperature-trend state; the minimal slice handles only the negative-delta ageing ladder.
- **2D temperature-compensated baseline** — baselines captured at several ambient bands.
- Bench dust-loading experiments and white-paper figures.

### C.7 Budget

The detector is compile-optional (`THERMALCORE_ENABLE_FAN_HEALTH`, default off on MCU). Compiled out, it contributes zero bytes. Compiled in, the slice is small — an estimated 1–2 KB of `.text`, comfortably inside the §9.2 ESP32 budget of ≤ 64 KB `.text`.

Implementation is tracked as Stage 17 of the implementation plan, the first post-v1 stage.

## Appendix D: CH32V003 STANDALONE Port (Post-v1 Sketch)

This appendix sketches a **post-v1** target: a fully self-contained thermal regulator on the WCH CH32V003 — a ~10-cent RV32EC microcontroller with 16 KB flash and 2 KB SRAM. It is recorded here so the design is settled before implementation begins; it is **not** part of v1 scope and affects no v1 gate.

### D.1 Goal

Run the unmodified `core/` — the same C99 source that runs in the Linux daemon and the ESP32-C3 firmware — **STANDALONE** on the CH32V003: a real DS18B20 probe feeds `thermal_core_step()`, which drives a real 4-wire fan over PWM with tach readback, on the chip, with **no host**. This demonstrates the portability thesis at the extreme low end: if the core fits a 10-cent part, the protocol-agnostic, static-allocation, fixed-point discipline has earned its keep.

### D.2 Fit (measured)

The CH32V003's 2 KB SRAM cannot hold the default `thermal_core_t` (a 4096-byte reservation), so the **tiny profile** (§D.3) is mandatory. With it, the budget — measured on a `riscv64` GCC 13.2.0 cross-build, RV32EC, `-Os -flto` with `--gc-sections` — is:

| Item | Size | Source |
|---|---|---|
| tiny-profile `core/` + libgcc soft-arithmetic | ~10.7 KB | measured |
| `const G_THERMAL_CFG` (tiny profile) | 0.54 KB | measured |
| 1-Wire + TIM2 PWM + EXTI tach BSP | ~0.8 KB | bench firmware |
| ch32fun runtime + vector table | ~1.0 KB | bench firmware |
| UART status print (optional) | ~1.2 KB | bench firmware |
| app glue (tick loop, snapshot build) | ~0.5 KB | new |
| **STANDALONE total** | **~14.5–15.5 KB of 16 KB** | fits |

RAM: the tiny-profile core state is 792 B (core internal struct + input snapshot + output frame); with BSP globals and stack the regulator runs in ~1.3 KB of the 2 KB SRAM. The RV32EC core has no hardware multiply or divide, so the Q16.16 math pulls in libgcc soft-arithmetic (`__muldi3`, `__divdi3`, …); that cost is included in the ~10.7 KB above.

**Display tradeoff.** An on-device SSD1306 OLED costs ~2.7 KB (driver + font table). A STANDALONE core and an OLED do not co-fit 16 KB — it is one or the other. This sketch assumes **no on-device display**: the regulator is headless, with an optional UART status line as the only readout.

### D.3 The tiny profile

A compile-time profile for severely flash- and RAM-constrained MCUs:

- The `THERMAL_MAX_*` constants in `core/thermal_config.h` are currently plain `#define`s. They become `#ifndef`-guarded so a profile header, selected by a build flag, can override them.
- The MCU profile sets 1 zone / 1 sensor / 1 actuator / 4 fault detectors / 2 samples-per-snapshot, and cuts `THERMAL_CORE_T_RESERVED_BYTES` from 4096 to ~1024.
- This shrinks `thermal_core_internal_t` from 2712 B to 760 B (measured) — the single change that takes the core from "2× over the 2 KB SRAM" to comfortably under.

The tiny profile is target-agnostic; any future constrained-MCU port reuses it.

### D.4 Platform integration

`platform/ch32v003/` mirrors `platform/esp32_idf/`:

- `bsp_ch32_pwm.c` / `bsp_ch32_tach.c` / `bsp_ch32_sensor.c` — slot-indexed BSP wrappers, adapted from the existing bench firmware (TIM2 PWM at 25 kHz, EXTI falling-edge tach, DS18B20 over bit-banged 1-Wire).
- The `mcu_pinmap` JSON section drives the GPIO assignments — already target-agnostic (that is why the key was renamed from `esp32_pinmap`); `json2static.py` emits the pin map exactly as it does for the ESP32.
- **`ch32fun` is vendored as a pinned git submodule** at `platform/ch32v003/ch32fun/` — the `tools/car-can-emulator/` model (§17.1 decision 3): pinned by commit SHA, tracked branch documented. ch32fun supplies the CH32 MCU headers, startup, and linker scripts; it is small enough to vendor, and pinning it keeps the build reproducible. ESP-IDF, by contrast, is too large to vendor and stays a system install. The `riscv64-unknown-elf-gcc` cross-compiler is likewise a system tool, pinned in `ci/tool-versions.md`.
- A plain Makefile (ch32fun's model — no CMake or IDF) and a `make build-ch32` target mirroring `make build-esp32`, including a `.text`/`.bss` size-budget assertion.

### D.5 Scope

STANDALONE only. No on-device display. No CAN — a CH32V003 node is a single zone with no vehicle-context input. The `HIL_PERIPHERAL` and `REPLAY_STANDALONE` modes also fit the part comfortably (the core dead-strips in HIL; REPLAY drops the BSP) and become available for free, but they are not the target of this sketch.

Implementation is tracked as Stage 18 of the implementation plan.

---

*End of PRD v0.23*
