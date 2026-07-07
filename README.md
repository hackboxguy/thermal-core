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
| CH32V003 firmware| [`platform/ch32v003/`](platform/ch32v003) | STANDALONE port on a ~10-cent RV32EC MCU (post-v1)          |
| Scenario harness | [`tools/thermalcore-scenario/`](tools/thermalcore-scenario) | Plant simulator + Python runner + determinism gate          |
| Probe            | [`tools/thermalcore_probe.py`](tools/thermalcore_probe.py) | UDP -> CSV telemetry recorder for plots and SHA-256 gates  |
| Telemetry tool   | [`tools/thermal-telemetry-tool/`](tools/thermal-telemetry-tool) | C++ host driver for the CH32 command channel + PWM->RPM sweep (post-v1) |
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

Trips fire at 30 / 45 / 60 / 85 °C (state_pwm 100 / 160 / 220 /
255).  Heat the sensor with a finger or hairdryer; the fan steps
up.  Full
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
Normal status appears in the daemon's UDP telemetry (probe on
port 9030); ACK/NACK accounting is printed when the daemon
closes, and a `bsp_hil_serial: NACK ...` line appears on stderr
only if the peripheral rejects a command -- an error signal, not
routine output.  Full walkthrough including frame anatomy:
[`docs/getting-started/hil-peripheral.md`](docs/getting-started/hil-peripheral.md).

## Quick start: CH32V003 STANDALONE (post-v1)

Stage 18 ports the full thermal-core onto the WCH CH32V003 -- a
~10-cent RISC-V (RV32EC) microcontroller with 16 KB flash and
2 KB SRAM.  A compile-time *tiny profile* shrinks the static
maxima so the identical `core/` source fits the part; the
regulator is headless -- no display, no CAN (PRD Appendix D).

Hardware: a CH32V003 board (e.g. CH32V003F4P6) + DS18B20
(4.7 kΩ pull-up) + Noctua NF-A8 PWM fan with tach, plus a
WCH-LinkE programmer.

```bash
sudo apt-get install gcc-riscv64-unknown-elf            # RV32EC cross-toolchain
git submodule update --init platform/ch32v003/ch32fun   # vendored ch32fun

make build-ch32                                          # cross-compile + size-budget gate
make flash-ch32                                          # build + flash via WCH-LinkE
```

`make build-ch32` cross-compiles the firmware and asserts the
PRD Appendix D.2 budget (flash ≤ 16 KB, SRAM ≤ 2 KB).  The
current STANDALONE build links at 12 912 B flash / 1 044 B SRAM.

Expected pin map (see [`configs/ch32v003-standalone.json`](platform/ch32v003/configs/ch32v003-standalone.json)):

| Signal       | Pin | Notes                                  |
|--------------|-----|----------------------------------------|
| Fan PWM out  | PD4 | TIM2_CH1, 25 kHz                       |
| Fan tach in  | PD0 | EXTI0, 10 kΩ pull-up + ~10 nF filter cap to GND |
| DS18B20 data | PD3 | 4.7 kΩ pull-up (1-Wire)                |

An optional once-per-second status line is printed over the SWIO
debug channel (`make monitor`; compile-gated by
`THERMALCORE_CH32_STATUS`, on by default).

### Telemetry tap over UART

`make build-ch32 CH32_TELEMETRY=1` adds a USART1 telemetry tap:
the regulator emits the **canonical 9-column telemetry CSV** --
zone temperature, fan duty, fan RPM, fault events -- the same
projection the host scenario runner and determinism gate produce.
Wire a USB-serial adapter to USART1 (PD5 TX / PD6 RX) -- including
a **common ground** between the adapter and the board, or the
stream corrupts -- and capture it:

```bash
make build-ch32 CH32_TELEMETRY=1                   # build + size gate
make flash-ch32 CH32_TELEMETRY=1                   # build + flash via WCH-LinkE
tio /dev/ttyUSB0 -b 9600 -l /tmp/ch32-telemetry.csv
```

The telemetry baud defaults to 9600 -- a conservative choice for
the crystal-less CH32V003. Override it at build time with
`CH32_TELEMETRY_BAUD` (passed through `build-ch32` / `flash-ch32`),
matching `tio -b` to it:

```bash
make flash-ch32 CH32_TELEMETRY=1 CH32_TELEMETRY_BAUD=115200
tio /dev/ttyUSB0 -b 115200 -l /tmp/ch32-telemetry.csv
```

Pass `CH32_TELEMETRY=1` to **both** commands: ch32fun relinks the
firmware from source on every invocation, so `make flash-ch32`
without it would flash the default (non-telemetry) variant.

The capture is a canonical CSV you can analyse exactly like the
ESP32 bench captures. The telemetry tap itself is transmit-only;
the RX line carries the host-command channel of the bench build
(below). The SWIO status line above is independent and stays
available.

If the CH32 telemetry callback ring overflows, the firmware emits
`TSIG_PLATFORM_CH32_TELEMETRY_DROPS` (`0x0900`) as a cumulative
canonical sample. Treat any non-zero value as a lossy capture.

The live `tio` view may look stair-stepped — the canonical CSV
uses `\n` line endings (no `\r`), and `tio`'s raw mode renders
them without a carriage return. This is cosmetic: the logged
`-l` file is correct, LF-only canonical CSV (confirm with
`cat`). For a tidy live view, `tail -f` the log from a second
terminal, where a normal terminal supplies the carriage return.

**Status:** the firmware is verified to cross-compile, link, and
fit the part -- the default, `CH32_TELEMETRY=1`, and `CH32_COMMAND=1`
builds are gated in CI by `build-ch32` -- and has been brought up on
real hardware: a heat/cool capture of the regulator running
standalone on a CH32V003 (DS18B20 + Noctua NF-A8 fan) is in the
white paper's Evaluation section.  A calibrated benchmark sweep is
still follow-on bench work.

### Bench characterisation: host-command channel (post-v1)

`make build-ch32 CH32_COMMAND=1` builds a **bench variant** of the
firmware that accepts host commands over the USART1 RX line: a host
PC can read PWM duty and tach RPM, and switch the control loop off
to drive the fan manually. This is the characterisation path for a
fan's PWM-to-RPM curve -- the baseline the fan-health detector needs.

It is a bench / characterisation build, **not** shipping firmware:
the regulation-disable command is absent from the default image.
`CH32_COMMAND=1`
forces `CH32_TELEMETRY=1` on -- command responses share the USART1 tap.
Fan-health is compiled out of this bench-control image by default to
preserve flash headroom; the telemetry image below is the normal
fan-health runtime build.

The host side is [`thermal-telemetry-tool`](tools/thermal-telemetry-tool),
a self-contained C++ tool:

```bash
make flash-ch32 CH32_COMMAND=1 CH32_TELEMETRY_BAUD=115200   # bench firmware
make telemetry-tool                                         # build the host tool
cd tools/thermal-telemetry-tool

./thermal-telemetry-tool --device=/dev/ttyUSB0 --baud=115200 --action=ping
./thermal-telemetry-tool --device=/dev/ttyUSB0 --baud=115200 --action=pwmsweep \
    --value=/tmp/nf-a8-sweep-table.csv      # sweep duty 1-100%, log PWM->RPM
```

`pwmsweep` switches the control loop off, steps the duty across
1-100 %, records the settled tach RPM at each point, and writes a
`pwm_pct,duty_0_255,rpm` table -- then resumes regulation. Other
actions: `log` (stream telemetry to a file), `pwmset` / `pwmget`,
`rpmget`, `loop on|off`.

**Bench mode:** `loop off` suspends thermal regulation -- the fan
obeys `pwmset`, not the temperature. Run it with an operator
present; `loop on` (or `pwmsweep`, which restores it) re-arms the
regulator.

### Fan-health detector (post-v1)

Stage 20 enables the advisory fan-health detector on the CH32V003,
gated together with `CH32_TELEMETRY=1`: the detector grades the fan's
RPM drift against a per-actuator PWM-to-RPM baseline and emits
`fan_health_*` telemetry (delta / severity / baseline source /
confidence). It never commands the fan -- it is observability, not
control.

Two reference fan baselines ship in this repo, each captured on a
deployed unit with the bench tool:

| Fan | Config | Sweep CSV(s) |
|---|---|---|
| Noctua NF-A8 (active) | [`ch32v003-standalone.json`](platform/ch32v003/configs/ch32v003-standalone.json) | [`docs/paper/data/ch32v003-nf-a8-sweep.csv`](docs/paper/data/ch32v003-nf-a8-sweep.csv) (1 run) |
| Arctic P8 PWM PST (alt) | [`ch32v003-standalone-arctic-p8.json`](platform/ch32v003/configs/ch32v003-standalone-arctic-p8.json) | `docs/paper/data/ch32v003-arctic-p8-pst-sweep-run{1,2,3}.csv` (3 runs) |

Swap configs at build time with `CH32_CONFIG=`:

```bash
make build-ch32 CH32_TELEMETRY=1 \
    CH32_CONFIG=configs/ch32v003-standalone-arctic-p8.json
```

Each baseline is an 8-point `fan_health.baseline` table, the path
from Stage 19 to that data is:

```bash
# 1. Flash the bench-channel firmware (Stage 19) on the target fan.
make flash-ch32 CH32_COMMAND=1 CH32_TELEMETRY_BAUD=115200
# 2. Sweep the fan and capture a PWM-to-RPM table.
./tools/thermal-telemetry-tool/thermal-telemetry-tool \
    --device=/dev/ttyUSB0 --baud=115200 \
    --action=pwmsweep --value=/tmp/sweep.csv
# 3. Pick <=8 points (spin-up threshold + governor-state duties),
#    update the fan_health block in the JSON, reflash with telemetry.
make flash-ch32 CH32_TELEMETRY=1 CH32_TELEMETRY_BAUD=115200
```

The detector remains compiled out of the default no-telemetry firmware,
which currently links at 12 912 B of 16 KB. The telemetry build (with
the detector and drop counter) sits at 15 016 B of 16 KB; the
`CH32_COMMAND=1` bench variant, with fan-health compiled out by default,
sits at 14 556 B.

## Build + test (host-side)

The Linux daemon, replay tests, scenario harness, and
determinism gate all build with `make`.  No system dependencies
beyond a C99 compiler (`gcc` or `clang`), `make`, and Python 3
for the scenario / probe tooling.

```bash
make build              # build daemon
make test               # host unit test suite
make replay             # byte-equal replay against committed goldens
make scenario           # 11 canonical PRD §9.1 scenarios on a sim plant
make determinism        # same scenarios x2 + gcc-vs-clang SHA-256 parity
make integration-can    # SocketCAN integration via car-can-emulator
                        # (auto-SKIPs if vcan0 isn't available)
make build-esp32        # ESP32-C3 STANDALONE + REPLAY firmware builds
                        # (auto-SKIPs if ESP-IDF isn't installed)
make build-ch32         # CH32V003 STANDALONE firmware cross-build
                        # (auto-SKIPs if the RISC-V toolchain is absent)
make telemetry-tool     # CH32 command-channel host tool (C++17)
```

PR CI runs the full gate set on every push; see
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
- **Stage 15** — cross-platform replay parity.  The host
  replay binary reproduces a committed golden telemetry CSV
  byte-for-byte (`replay-parity-host`, PR-gated); the full
  host-vs-target comparison is release-tag bench discipline.
- **Stage 16** — white-paper figure pipeline: canonical
  scenario plots regenerated from telemetry CSVs, a benchmark
  manifest, a `figure-freshness` PR gate, and a `release.yml`
  that builds the paper PDF on `v*` tags.  Closes the v1
  implementation plan (Stages 0-16).
- **Stage 17** (post-v1) — fan-health detector: an
  advisory-only `core/` module that grades fan bearing wear /
  contamination by comparing measured tach RPM against a
  per-actuator PWM-to-RPM baseline, emitting `fan_health_*`
  telemetry (delta / severity / baseline-source / confidence).
  It never commands an actuator.  Compile-gated by
  `THERMALCORE_ENABLE_FAN_HEALTH` (host-on, MCU-off, so the
  default ESP32/CH32 images do not include it unless a later
  target-specific gate opts in); enabled per actuator via a
  `fan_health` JSON block — see
  [`configs/fan-health-demo.json`](configs/fan-health-demo.json).
- **Stage 18** (post-v1) — CH32V003 STANDALONE port: the
  portable `core/` cross-builds self-contained on a ~10-cent
  RV32EC MCU, gated by `build-ch32` + `unit-tiny-profile`, and
  is brought up on real hardware — a heat/cool capture of the
  regulator running standalone on the chip is in the white
  paper's Evaluation section. A calibrated benchmark sweep is
  still follow-on bench work.
- **Stage 19** (post-v1) — CH32V003 host-command channel: a
  compile-gated bench build (`CH32_COMMAND=1`) that accepts host
  commands over USART1 RX, plus
  [`thermal-telemetry-tool`](tools/thermal-telemetry-tool), a C++
  host tool that drives a PWM-to-RPM sweep — the characterisation
  path for a fan-health baseline. The bench-only command path is
  absent from the shipping firmware; the bench build is gated by a third
  `build-ch32` matrix leg.
- **Stage 20** (post-v1) — fan-health detector enabled on the
  CH32V003 STANDALONE firmware. The Stage 17 detector now compiles
  on a 10-cent MCU with a real PWM-to-RPM baseline measured on the
  deployed fan via `thermal-telemetry-tool --action=pwmsweep`. The
  `build-ch32` telemetry and Arctic matrix legs exercise the detector;
  the command bench-control image keeps it compiled out by default for
  flash headroom, and the default no-telemetry image keeps it compiled
  out.
- **M6** (post-v1) — governor dispatch is centralized in a closed
  static ops table inside `core/thermal_core.c`. PID remains default-on
  for host/ESP32 builds but is compile-gated by `THERMALCORE_ENABLE_PID`;
  the CH32 tiny profile builds step-wise-only by default and can opt
  back in with `CH32_ENABLE_PID=1`.
- **M4** (post-v1) — PID zones can opt into a default-off D-term IIR
  (`d_filter_alpha_q16`) and per-actuator static duty linearization.
  Step-wise `state_pwm[]` output is untouched, and CH32 PID-off builds
  reject/omit the PID-only table during static config generation.

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
