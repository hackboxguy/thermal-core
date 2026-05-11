# thermal-core — Product Requirements Document

**Project/repo name:** `thermal-core`
**Linux daemon binary:** `thermalcored`
**Repository (planned):** `github.com/hackboxguy/thermal-core`
**Document status:** Draft v0.3
**Author:** Albert Dav
**License (code):** MIT  **License (paper/doc):** CC-BY-4.0

> **Naming decision.** `thermal-engine` was the original working name, but Qualcomm ships a daemon by the same name in many Android/AOSP automotive trees. To avoid Google-search collision and confusion in automotive contexts, this PRD uses `thermal-core` for the repo/library concept and `thermalcored` for the Linux daemon binary.

---

## 1. Overview

`thermal-core` is a portable, configurable thermal-control core with an automotive In-Vehicle Infotainment (IVI) reference application. The reference application implements closed-loop fan control across multiple temperature zones with the deliberate objective of trading off two competing goals: **sufficient cooling** to keep silicon within its thermal envelope, and **low acoustic emissions** in the cabin, especially when the vehicle is stationary and ambient noise is low.

The core innovation is not the control algorithm itself — PID and step-wise governors are well-understood — but the **architectural separation** that lets one body of policy code run unmodified across:

- Linux userspace (development host, simulation, hardware-in-the-loop bench)
- ESP-IDF / FreeRTOS on ESP32 (reference embedded implementation and bench rig peripheral concentrator)
- Renesas RH850-F1KM under FreeRTOS (future automotive target)

This is supported by a small platform abstraction (vtable-based BSP), static memory allocation, fixed-point math, and numeric event logging — discipline imposed up front so the Linux experience does not produce code that fails to port.

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
- deterministic stepping over typed inputs and outputs
- fixed-size configuration structures that can be populated from JSON on Linux or generated C on MCU targets

The platform layer owns:

- Linux `hwmon`, SocketCAN, files, UDP, serial, systemd, and CLI handling
- ESP-IDF / FreeRTOS drivers such as LEDC, PCNT, TWAI, 1-Wire, I2C, USB-CDC, and NVS
- future RH850-F1KM CAN, PWM, ADC, timer, and RTOS binding

Domain-specific context, such as vehicle speed from OBD-II, enters the core only as a typed context signal. The core should never parse CAN frames, know OBD-II request IDs, open sysfs files, allocate JSON objects, or depend on any vehicle-specific protocol.

## 3. Goals and Non-Goals

### 3.1 Goals (v1)

- One C99 core that runs unmodified on Linux, ESP32 (ESP-IDF / FreeRTOS), and (by structural readiness) RH850-F1KM under FreeRTOS.
- Configurable multi-zone thermal control: 2–4 temperature zones, 1–2 PWM-controlled fans, step-wise and PID governors selectable per zone.
- Vehicle-speed-aware acoustic policy: PWM cap and trip-point offset as functions of speed.
- A generic context-input mechanism so vehicle speed is one policy input, not a core dependency.
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

The core depends only on a small vtable (`thermal_platform_t`) and a configuration struct (`thermal_config_t`). It does not call any function that is not in this vtable or in `<string.h>` (memcpy/memset only). It does not allocate after init. It does not use floating point except optionally in the PID block (controlled by a build flag; default is Q16.16 fixed-point).

### 4.2 Discipline constraints (apply to core code)

| Constraint | Rationale |
|---|---|
| No `malloc`/`free`/`calloc` after init | RH850-F1KM has no heap; static allocation avoids fragmentation and makes worst-case memory footprint exact. |
| No floating point in hot path (PID is opt-in float) | Avoid reliance on FPU; Q16.16 is exact across platforms. |
| No strings in hot path | Strings are interned to numeric IDs at config-load; logs carry numeric event codes + up to 4 uint32 args. |
| No syscalls in core | Everything goes through the platform vtable. |
| Single control thread | Same model on Linux (one process) and MCU (one FreeRTOS task). |
| Compile-time max-size limits | `THERMAL_MAX_ZONES`, `THERMAL_MAX_SENSORS`, etc. Configurable, but fixed at build. |
| `-Wall -Wextra -Werror -std=c99 -pedantic` clean | Portability hygiene. |

### 4.3 Platform vtable

```c
typedef struct {
    /* Time and scheduling */
    uint32_t (*now_ms)(void);
    void     (*sleep_ms)(uint32_t ms);

    /* Logging — numeric event codes, no strings */
    void     (*log_event)(uint16_t code, uint32_t a1, uint32_t a2,
                          uint32_t a3, uint32_t a4);

    /* I/O */
    int      (*sensor_read)(uint8_t id, int32_t *millideg_out);
    int      (*actuator_write_pwm)(uint8_t id, uint8_t duty_0_255);
    int      (*actuator_read_tach)(uint8_t id, uint16_t *rpm_out);
    int      (*speed_read)(uint16_t *kmh_out);  /* vehicle speed */

    /* Persistence (optional, may be NULL) */
    int      (*nvs_read)(uint16_t key, void *buf, size_t len);
    int      (*nvs_write)(uint16_t key, const void *buf, size_t len);

    /* Telemetry emit (optional, may be NULL) */
    void     (*telemetry_emit)(uint16_t signal_id, int32_t value);
} thermal_platform_t;
```

### 4.4 Data model

Borrowed in spirit from the Linux kernel thermal framework:

- **Sensor**: a source of temperature samples in millidegrees C. Has an ID, a name (debug only), an IIR filter coefficient, and a stuck-sensor detector.
- **Zone**: a logical thermal region (e.g., "soc", "amp", "tuner"). Has 1+ sensors (aggregated max/avg/weighted), 1+ trip points, an active governor, and a current state.
- **Trip point**: a temperature threshold with a hysteresis band and an action (target cooling state).
- **Actuator**: a cooling device. v1 = PWM fan with optional tach feedback. Has min/max PWM, slew-rate limit, stall detector.
- **Governor**: a control algorithm. v1 = `step_wise` (Linux-thermal style discrete states) and `pid` (continuous PID with anti-windup).
- **Policy modifier**: a post-processing stage that adjusts the actuator command from the governor based on context. v1 = `acoustic_mask` (PWM cap and trip offset as a function of vehicle speed).
- **Arbitrator**: when multiple zones target the same actuator, the arbitrator decides the final command. v1 = max-wins (highest demanded PWM).
- **Fault detector**: stall, sensor-stuck, runaway. Per detector, configurable thresholds, and a per-fault action.

### 4.5 Control loop

```
tick (every CONTROL_PERIOD_MS, default 100ms):
    1. read all sensors → apply IIR → update zone temperatures
    2. read vehicle speed → update speed-modifier inputs
    3. for each zone: evaluate trip points, run governor → desired PWM
    4. for each actuator: arbitrate across zones (max-wins)
    5. apply policy modifiers (acoustic mask: clamp, optionally shift trips)
    6. apply slew-rate limit, write PWM
    7. read tach → check stall fault
    8. check stuck-sensor and runaway faults
    9. emit telemetry for changed signals
   10. log any state transitions
```

Determinism: the loop is monotonic; `now_ms()` rollover (32-bit ms = ~49 days) is handled with `(uint32_t)(a - b)` subtraction throughout.

## 5. Configuration

### 5.1 Linux: JSON

Linux daemon loads JSON at startup via `jsmn` (small, no-malloc parser, vendored into repo). Schema (illustrative; full schema in `docs/schema/thermal-core.schema.json`):

```json
{
  "control_period_ms": 100,
  "sensors": [
    {"id": 0, "name": "soc",     "source": "hwmon:coretemp/temp1_input", "iir_alpha_q16": 16384},
    {"id": 1, "name": "amp",     "source": "ds18b20:28-0000abc"},
    {"id": 2, "name": "tuner",   "source": "i2c:1:0x18:mcp9808"},
    {"id": 3, "name": "board",   "source": "hwmon:nct6775/temp2_input"},
    {"id": 4, "name": "ambient", "source": "ds18b20:28-0000xyz"}
  ],
  "speed_source": "canbus:obd2:pid_0x0D",
  "actuators": [
    {"id": 0, "name": "main_fan", "pwm": "hwmon:nct6775/pwm1", "tach": "hwmon:nct6775/fan1_input",
     "pwm_min": 80, "pwm_max": 255, "slew_per_tick": 8, "stall_rpm": 200}
  ],
  "zones": [
    {"name": "soc", "sensors": ["soc"], "aggregation": "max",
     "governor": "pid", "pid": {"kp_q16": 4915, "ki_q16": 327, "kd_q16": 0, "setpoint_mc": 75000},
     "actuators": ["main_fan"],
     "trips": [
       {"temp_mc": 70000, "hyst_mc": 2000, "action": "warn"},
       {"temp_mc": 85000, "hyst_mc": 2000, "action": "critical"},
       {"temp_mc": 95000, "hyst_mc": 2000, "action": "shutdown"}
     ]},
    {"name": "amp",   "sensors": ["amp"],   "governor": "step_wise", "...": "..."},
    {"name": "tuner", "sensors": ["tuner"], "governor": "step_wise", "...": "..."}
  ],
  "policy_modifiers": [
    {"name": "acoustic_mask", "input": "vehicle_speed",
     "curve": [
       {"speed_kmh": 0,   "pwm_cap": 120, "trip_offset_mc": 0},
       {"speed_kmh": 30,  "pwm_cap": 180, "trip_offset_mc": 0},
       {"speed_kmh": 80,  "pwm_cap": 255, "trip_offset_mc": -5000},
       {"speed_kmh": 130, "pwm_cap": 255, "trip_offset_mc": -8000}
     ],
     "fail_safe": "assume_stationary"}
  ],
  "fault_detection": {
    "stall_persist_ticks": 30,
    "stuck_sensor_window_ticks": 600,
    "stuck_sensor_delta_mc": 100,
    "runaway_persist_ticks": 50
  },
  "telemetry": {
    "enable": true,
    "transport": "udp:127.0.0.1:9001",
    "signals": ["zone_temp_*", "actuator_pwm_*", "actuator_rpm_*", "pid_terms_*", "speed_kmh"]
  }
}
```

### 5.2 ESP32 and RH850: static `const` struct

Same `thermal_config_t` shape, populated as a static const in C (built into the firmware image). The JSON parser is never linked. Configuration is changed by rebuilding, **or** at runtime via the control-plane (see §7) for parameters within configured limits.

A small Python tool `tools/json2static.py` converts a JSON config into a `static const thermal_config_t` C file, for round-trip consistency between Linux experimentation and MCU deployment.

## 6. Vehicle Speed Integration (OBD-II via CAN)

### 6.1 Source

Speed is queried via OBD-II Service 01, PID `0x0D` (vehicle speed, single byte, km/h, range 0–255). The reference test source is **`car-can-emulator`**, included as a git submodule at `tools/car-can-emulator/` and tracking the upstream `v2-improvements` branch.

The emulator behavior relevant to `thermal-core`:

- Runs on Linux with SocketCAN using a real CAN adapter (`can0`) or virtual CAN (`vcan0`).
- Responds to SAE J1979 Mode 01 requests on `0x7DF` (functional request) and `0x7E0` (ECU request).
- Emits OBD-II responses on `0x7E8`.
- Supports PID `0x0D` for vehicle speed and additional PIDs useful for future context experiments (`rpm`, coolant `temp`, engine `load`, fuel level, battery voltage).
- Provides a TCP control interface on port `8080`; `echo -n "speed 120" | nc 127.0.0.1 8080` sets the simulated speed to 120 km/h.
- Provides a drive-cycle simulation mode (`--simulate`) and packaging examples for systemd, OpenWrt, and Buildroot.

Repository decision: keep `car-can-emulator` as a separate upstream repo and add it under `tools/car-can-emulator/` as a submodule. `thermal-core` consumes only the CAN frames; scenario tooling may use the emulator's TCP port to set speed during tests.

### 6.2 Mechanism

- **ESP32**: uses TWAI (CAN) peripheral. Sends OBD-II request at 1 Hz; decodes `0x7E8` response; exposes filtered speed (km/h) via `speed_read()` vtable.
- **Linux**: uses SocketCAN on a configurable interface (`can0`, `vcan0`). Same 1 Hz polling, same decoding. Allows replaying recorded CAN logs via `canplayer` and local testing against `car-can-emulator`.
- **RH850** (future): native CAN-FD module; same protocol.

### 6.3 Filtering and fail-safe

- Raw speed is filtered with a slow IIR (time constant ~5 s by default). Thermal time constants are tens of seconds; aggressive filtering avoids fan jitter from brake taps.
- If no valid response is received for `speed_timeout_ms` (default 3000 ms), the `acoustic_mask` modifier applies the `fail_safe` mode. Default: `assume_stationary` (most acoustically conservative).
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

The core emits `(timestamp, signal_id, value)` tuples through `telemetry_emit()` on the platform vtable. Signal IDs are numeric, defined once in `core/thermal_signals.h`. Examples:

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

### 7.2 Per-platform transport

- **Linux**: UDP packets to `127.0.0.1:9001` (configurable), 12-byte binary frame per sample (`u32 ts_ms, u16 sig_id, u16 reserved, i32 value`). Or CSV to stdout/file for simple cases.
- **ESP32**: USB-CDC binary frame stream by default; optionally UDP-over-WiFi for untethered tests.
- **RH850 (future)**: UART or CAN with same frame format.

### 7.3 Host-side tool: `thermalcore-probe`

A single Python script that:

- Reads telemetry from UDP, serial, or file (auto-detected by URI).
- Logs to CSV (`probe --log run.csv`).
- Plots live (`probe --live` — matplotlib, signal-selectable from CLI).
- Generates white-paper figures from logs (`probe --plot scenario_heatsoak.csv --out fig.pdf`).

Same tool, same plots, regardless of source. This is the figure-generation backbone for the white paper.

### 7.4 Control plane (runtime tuning)

Bidirectional: telemetry frames go host-ward, command frames go device-ward. Same transport, same framing, distinguished by an opcode field. Commands:

```
CMD_SET_PID         (zone_id, kp_q16, ki_q16, kd_q16)
CMD_SET_SETPOINT    (zone_id, mc)
CMD_SET_TRIP        (zone_id, trip_idx, mc, hyst_mc)
CMD_SET_CURVE_POINT (modifier_id, point_idx, x, y)
CMD_INJECT_FAULT    (fault_type, target_id, on/off)
CMD_FREEZE_INPUT    (sensor_id, fixed_value_mc)  /* for scenario testing */
CMD_RESUME_INPUT    (sensor_id)
```

A companion CLI tool `thermalcore-tune` issues commands. This is what makes the bench rig usable for actual loop tuning: change `kp`, see step response in `--live` plot, change again, in seconds. No rebuild, no reflash.

Commands respect compile-time bounds — `kp` cannot be set outside `[KP_MIN, KP_MAX]` defined in the config. This is intentional; runtime tuning is for the bench, not for the field, and bound-checking is cheap insurance.

### 7.5 Scenario scripting

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

## 8. Reference Bench Rig (pinned)

The bench rig is part of the deliverable. Anyone with the BOM and the wiring diagram in `docs/bench-rig.md` should be able to reproduce the results in the white paper.

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
| `can_bus_loss` | Cut CAN; verify fail-safe to `assume_stationary` after `speed_timeout_ms`. |

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
│   ├── thermal_platform.h               vtable interface
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
│   ├── thermalcore-scenario             Scenario runner
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
   - Three-layer split, platform vtable, discipline constraints, data model
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
| `07-architecture.tex` | three-layer architecture and platform vtable |
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

### 17.2 Remaining Questions

1. **What is the first physical thermal plant?** The PRD lists an optional resistor + MOSFET heater. Confirm whether v1 needs real heat injection or whether simulated/frozen sensor inputs plus fan tach are enough for the first white paper.
2. **Telemetry compression at high signal counts?** With ~20 signals at 10 Hz, ESP32 USB-CDC handles it easily (~2.4 KB/s). At 100 Hz or 100+ signals, may need ring-buffered batched frames. Defer until measured.

---

## Appendix A: Coordinate-System Conventions

| Quantity | Type | Units | Range |
|---|---|---|---|
| Temperature | `int32_t` | millidegrees C | -40000 to +150000 |
| PWM duty | `uint8_t` | 0–255 | 0–255 |
| Fan RPM | `uint16_t` | RPM | 0–65535 |
| Vehicle speed | `uint16_t` | km/h | 0–500 |
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
- **vtable**: Virtual function table; struct of function pointers used here for platform abstraction

---

*End of PRD v0.3*
