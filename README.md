# thermal-core

A portable, reproducible thermal regulator written in C99.  Same
control law (step_wise governor + IIR filter + slew limiting +
fault state-machine) runs unchanged on a Linux server and an
ESP32-C3, and is testable end-to-end against a deterministic
fixture plant on the host before any hardware is involved.

This is a concept piece with an accompanying LaTeX white paper,
not a production deliverable.  It is **not** ASIL-rated and
**not** AUTOSAR-compatible (see PRD §15).

## What's in here

| Layer            | Path                                | Purpose                                                     |
|------------------|-------------------------------------|-------------------------------------------------------------|
| Core             | [`core/`](core/)                    | Portable C99 thermal regulator -- no heap, no syscalls      |
| Wire protocol    | [`protocol/`](protocol/)            | "TC" binary frames + OBD-II Service 01 codec                |
| Linux daemon     | [`platform/linux/`](platform/linux) | `thermalcored` + BSPs (tmpfs / SocketCAN / serial-HIL)      |
| ESP32-C3 firmware| [`platform/esp32_idf/`](platform/esp32_idf) | ESP-IDF v5.5.2 component built in three modes              |
| Scenario harness | [`tools/thermalcore-scenario/`](tools/thermalcore-scenario) | Plant simulator + Python runner + determinism gate          |
| Probe            | [`tools/thermalcore_probe.py`](tools/thermalcore_probe.py) | UDP -> CSV telemetry recorder for plots and SHA-256 gates  |
| Reference docs   | [`docs/`](docs)                     | PRD (`thermal-core-prd.md`) + implementation plan + paper   |
| Getting started  | [`docs/getting-started/`](docs/getting-started) | Full deployment writeups for the three demo topologies      |

## The three topologies

```
                ╭───────────────────────────────────────╮
                │   Portable C99 core/                  │
                │   (governor, filter, slew, fault SM)  │
                ╰───────────────────────────────────────╯
                            │ links into
        ┌───────────────────┼──────────────────────────────┐
        │                   │                              │
   ┌────▼──────┐     ┌──────▼────────┐         ┌───────────▼────────────┐
   │ ESP32     │     │  Linux daemon │         │  Linux daemon          │
   │ STANDALONE│     │  (tmpfs/sysfs)│   ── ── │  HIL_PERIPHERAL        │
   │           │     │   on Pi4/x86  │   over  │   (core on Linux,      │
   │ Full core │     │   Full core   │   USB-  │    BSPs on ESP32-C3)   │
   │ on MCU    │     │   on host     │   CDC   │                        │
   └───────────┘     └───────────────┘         └────────────────────────┘
        │                   │                              │
        ▼                   ▼                              ▼
   1× DS18B20         1× DS18B20                1× DS18B20 (on ESP32)
   1× PWM fan         1× PWM fan                1× PWM fan (on ESP32)
   (bench rig)        (Pi4 + pwm-fan            ESP32 streams samples
                       DT overlay)              over /dev/ttyACM0
```

All three run the **identical** `core/` C source -- only the BSP layer differs.

## Quick start: ESP32-C3 STANDALONE

The ESP32-C3 runs the full thermal-core in a FreeRTOS task.
Hardware: ESP32-C3 SuperMini + DS18B20 (4.7 kΩ pull-up) +
Noctua NF-A8 PWM fan with tach.

```bash
. ~/esp/esp-idf/export.sh                       # ESP-IDF v5.5.2
cd platform/esp32_idf
idf.py set-target esp32c3
idf.py build flash monitor
```

Expected pin map (see [`configs/esp32-c3-standalone.json`](platform/esp32_idf/configs/esp32-c3-standalone.json)):

| Signal       | GPIO | Notes                                  |
|--------------|------|----------------------------------------|
| Fan PWM out  | 4    | 25 kHz, 3.3 V                          |
| Fan tach in  | 5    | 10 kΩ pull-up to 3.3 V                 |
| DS18B20 data | 6    | 4.7 kΩ pull-up to 3.3 V (1-Wire)       |

You should see a status line every second:

```
T= 31.81 C  duty= 100/255 ( 39 %)  tach=  28 ticks/s (~ 840 RPM)
```

Trips fire at 30 / 45 / 60 °C (state_pwm 100 / 160 / 220).  Heat
the sensor with a finger or hairdryer; the fan steps up.  Full
walkthrough: [`docs/getting-started/esp32-standalone.md`](docs/getting-started/esp32-standalone.md).

## Quick start: Linux daemon on a Raspberry Pi 4

The same `core/` C source compiled as a Linux daemon
(`thermalcored`).  The Pi4 owns a DS18B20 (1-Wire) and a Noctua
fan via the kernel's `pwm-fan` driver, exposed through
`/sys/class/hwmon/`.

```bash
# On the Pi4 (Raspberry Pi OS, kernel >= 5.10)
sudo apt-get install build-essential

git clone <repo-url> thermal-core
cd thermal-core
make build                                       # builds thermalcored

# Configure hardware once (see linux-pi4.md for the DT overlay):
#   - Enable 1-Wire in /boot/config.txt: dtoverlay=w1-gpio,gpiopin=4
#   - Enable hardware PWM (gpio 12): dtoverlay=pwm,pin=12,func=4
#   - Add pwm-fan DT fragment (binds pwm + tach into hwmon).
# After reboot, identify the hwmon node:
ls /sys/bus/w1/devices/                          # find 28-XXXXXXXXXXXX
ls /sys/class/hwmon/                             # find the hwmonN for pwm-fan

# Run the daemon with a config pointing at those paths.
sudo build/platform-linux/thermalcored --config=docs/getting-started/pi4-config.json
```

Monitor over UDP (port 9030):

```bash
python3 tools/thermalcore_probe.py \
    --listen udp:127.0.0.1:9030 \
    --log /tmp/pi4-telemetry.csv \
    --duration 60
```

Heat the sensor; `/tmp/pi4-telemetry.csv` records the fan ramp.
Full walkthrough including the DT overlay snippet:
[`docs/getting-started/linux-pi4.md`](docs/getting-started/linux-pi4.md).

## Quick start: HIL_PERIPHERAL

Stage 14 closed.  HIL is now PR-gated in CI: the firmware build
runs in the `build-esp32` matrix with the size budget, and the
daemon's `bsp_hil_serial` is exercised by a PTY-driven
integration test on every push (`test/integration/test_hil_serial.py`).
Bench integration (real ESP32 + DS18B20 + fan on USB-CDC)
remains a manual / nightly procedure per impl-plan §5.

ESP32-C3 owns the hardware (DS18B20 + fan); the Linux daemon
runs the core over USB-CDC.  Two builds + one cable.

```bash
# 1. Flash the HIL firmware on the ESP32:
. ~/esp/esp-idf/export.sh
cd platform/esp32_idf
idf.py fullclean
idf.py -DTHERMALCORE_HIL_PERIPHERAL=ON flash

# 2. On the host (Linux, any flavour with the daemon built):
cd thermal-core
make build
sudo build/platform-linux/thermalcored --config=test/integration/hil-config.json
```

The ESP32 publishes sensor + tach as `TELEM_SAMPLE` TC frames on
`/dev/ttyACM0`; the daemon ingests them, runs
`thermal_core_step()`, and writes the duty back as
`CMD_REQUEST(HIL_CMD_SET_PWM_DUTY = 0x8001)` on the same device.
Status appears in the daemon's UDP telemetry (probe on port
9030) and as `bsp_hil_serial: NACK ...` lines on the daemon's
stderr.  Full walkthrough including frame anatomy:
[`docs/getting-started/hil-peripheral.md`](docs/getting-started/hil-peripheral.md).

## Build + test (host-side)

The Linux daemon, replay tests, scenario harness, and
determinism gate all build with `make`.  No system dependencies
beyond a C99 compiler (`gcc` or `clang`), `make`, and Python 3
for the scenario / probe tooling.

```bash
make build              # build daemon
make test               # 200-ish unit tests
make replay             # byte-equal replay against committed goldens
make scenario           # 10 canonical PRD §9.1 scenarios on a sim plant
make determinism        # same scenarios x2 + gcc-vs-clang SHA-256 parity
make integration-can    # SocketCAN integration via car-can-emulator
                        # (auto-SKIPs if vcan0 isn't available)
make build-esp32        # ESP32-C3 STANDALONE + REPLAY firmware builds
                        # (auto-SKIPs if ESP-IDF isn't installed)
```

PR CI runs all 15 of these gates on every push; see
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Repository status

The codebase tracks the implementation plan in
[`docs/thermal-core-implementation-plan.md`](docs/thermal-core-implementation-plan.md).
Recent stages:

- **Stages 0-12** — host-side daemon, scenario harness, CAN
  integration, deterministic plant + SHA-256 cross-compiler
  parity gate.  All PR-gated.
- **Stage 13** — ESP32-C3 STANDALONE + REPLAY_STANDALONE
  builds.  `build-esp32` CI gate active; size budget
  (.text ≤ 64 KB / .bss ≤ 16 KB for core/+protocol/) enforced.
- **Stage 14** — HIL_PERIPHERAL build closed.  All three
  ESP32 build modes (STANDALONE / REPLAY_STANDALONE /
  HIL_PERIPHERAL) gated on every PR via `build-esp32` matrix
  with size budgets.  PTY-driven integration test
  (`test_hil_serial.py`) closes the daemon-side BSP coverage
  gap.  Bench integration documented as manual / nightly.
- **Stage 15** — host-vs-target byte-for-byte SHA-256
  parity gate (pending).

## Documentation

- [PRD](docs/thermal-core-prd.md) — the product spec and the
  source of truth for every wire format, signal ID, and
  numeric constant.
- [Implementation plan](docs/thermal-core-implementation-plan.md)
  — stage-by-stage roll-out with exit gates.
- [Getting-started guides](docs/getting-started/) — full
  deployment writeups for each of the three demo topologies.
- [White paper](docs/paper/) — work in progress.

## License + scope

Concept piece; license file pending the white paper publication.
Not for production use.
