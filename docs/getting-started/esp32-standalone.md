# ESP32-C3 STANDALONE deployment

The STANDALONE build is the simplest of the three thermal-core
topologies: a single ESP32-C3 SuperMini runs the **full**
control loop (governor + filter + slew + fault SM) on its own,
talking directly to a Noctua NF-A8 PWM fan and a DS18B20
temperature sensor.  No daemon, no host, no network — just the
chip on a breadboard.

This is the configuration the white-paper benchmarks call
"MCU-only".  Same `core/` C99 source as the Linux daemon.

## Bill of materials

| Item              | Notes                                                 |
|-------------------|-------------------------------------------------------|
| ESP32-C3 SuperMini| Any C3-based dev board with USB-C; the SuperMini is what was used on the reference bench. |
| Noctua NF-A8 5V PWM| 4-wire fan: +5V, GND, PWM in, tach out.              |
| DS18B20           | TO-92 package, 1-Wire digital temperature sensor.     |
| 4.7 kΩ resistor   | Pull-up for the DS18B20 1-Wire data line.             |
| 10 kΩ resistor    | Pull-up for the fan tach (open-collector output).     |
| Breadboard + wires| For prototyping the harness.                          |
| USB-C cable       | Powers the C3 + carries the status log.               |

The fan needs **5 V** on its red wire; the C3 SuperMini exposes
5 V on the `VBUS`/`5V` pin (USB-powered).  PWM and tach are 3.3 V
signals on the GPIOs — within Noctua's documented tolerance.

## Pin map

| Signal       | C3 GPIO | Wire to                                |
|--------------|---------|----------------------------------------|
| Fan +5 V     | VBUS    | Fan red wire                           |
| Fan GND      | GND     | Fan black wire                         |
| Fan PWM out  | GPIO 4  | Fan blue wire                          |
| Fan tach in  | GPIO 5  | Fan yellow wire, with 10 kΩ pull-up to 3.3 V |
| DS18B20 +    | 3V3     | DS18B20 pin 3 (VDD)                    |
| DS18B20 GND  | GND     | DS18B20 pin 1                          |
| DS18B20 data | GPIO 6  | DS18B20 pin 2 (DQ), with 4.7 kΩ pull-up to 3.3 V |

Pin map source: [`platform/esp32_idf/main/main.c`](../../platform/esp32_idf/main/main.c)
(`PIN_FAN_PWM`, `PIN_FAN_TACH`, `PIN_ONEWIRE`).  Change those
constants and re-flash if your wiring is different.

```
   ┌─────────────────┐                     ┌──────────────┐
   │  ESP32-C3       │                     │  Noctua      │
   │  SuperMini      │                     │  NF-A8 5V    │
   │                 │                     │              │
   │       VBUS ●────┼─────────────────────┤●  RED  (5V)  │
   │        GND ●────┼─────────────────────┤●  BLK  (GND) │
   │     GPIO 4 ●────┼─────────────────────┤●  BLU  (PWM) │
   │     GPIO 5 ●─┬──┼─────────────────────┤●  YEL  (tach)│
   │              │                        │              │
   │        3V3 ●─┴───── 10 kΩ ─────┐      └──────────────┘
   │                                │
   │                                ▼ (pull-up on tach)
   │
   │              ┌──── 4.7 kΩ ─── 3V3
   │              │
   │     GPIO 6 ●─┴─────────────────────────┐
   │        3V3 ●───────────────────────┐   │
   │        GND ●───────────────────┐   │   │
   └─────────────────────────────────┼───┼───┼──
                                     │   │   │
                                     │   │   │
                                  ┌──┴───┴───┴──┐
                                  │  DS18B20    │
                                  │  GND VDD DQ │
                                  └─────────────┘
```

## Software requirements

- **ESP-IDF v5.5.2** installed at `~/esp/esp-idf/` (or override
  `IDF_PATH`).  Other 5.5.x versions probably work but only
  v5.5.2 is what the CI image pins.
- A USB-C cable that supports data.  Many "charge-only" cables
  exist — verify yours by running `ls /dev/ttyACM*` before and
  after plugging the C3 in.

## Build + flash + monitor

```bash
. ~/esp/esp-idf/export.sh           # sets PATH + IDF_PATH
cd platform/esp32_idf
idf.py set-target esp32c3           # one-time per checkout
idf.py build                        # ~30 s clean, ~2 s incremental
idf.py -p /dev/ttyACM0 flash        # ~10 s
idf.py -p /dev/ttyACM0 monitor      # Ctrl-] to exit
```

(Combine with `idf.py build flash monitor` for the loop.)

You should see boot output, then a sustained status line:

```
T= 28.31 C  duty=   0/255 (  0 %)  tach=   0 ticks/s (~   0 RPM)
T= 28.18 C  duty=   0/255 (  0 %)  tach=   0 ticks/s (~   0 RPM)
...
```

Below 30 °C the fan is off (state 0).  Heat the DS18B20 with
your finger — when `T` crosses 30.0 °C, the fan jumps to duty
100 (state 1).  Cross 45.0 °C → duty 160 (state 2).  Cross
60.0 °C → duty 220 (state 3).  Trips reset on hysteresis
(`hyst_mc = 2000`, i.e. -2 °C below each trip).

## What's happening on the chip

```
   tick 0 .. infinity, every 100 ms:
     1. Read DS18B20  (cadence-gated to 1 Hz; ~750 ms conversion).
     2. Read tach delta (GPIO ISR counter).
     3. Build thermal_input_snapshot_t with 2 samples.
     4. Call thermal_core_step(&core, &snap, &out).
     5. Apply out.actuator_cmds[0].duty_0_255 to LEDC PWM.
     6. Once per second, print the status line above.
```

Source: [`app_main_standalone()` in main.c](../../platform/esp32_idf/main/main.c).
The portable `core/` source is linked unchanged into the
firmware via the IDF component CMakeLists; size budget for
`core/` + `protocol/` is 64 KB `.text` / 16 KB `.bss` (PRD §9.2)
and currently sits at 11.7 KB / 0 B with 82 % headroom.

## Tuning the trip points

The trips live in [`platform/esp32_idf/configs/esp32-c3-standalone.json`](../../platform/esp32_idf/configs/esp32-c3-standalone.json).
The JSON is converted into a `const thermal_config_t` at build
time by `tools/json2static.py` — change the JSON, re-run
`idf.py build`, re-flash.

```json
"trips": [
  { "temp_mc": 30000, "hyst_mc": 2000, "severity": "warn",     "cooling_state": 1 },
  { "temp_mc": 45000, "hyst_mc": 2000, "severity": "warn",     "cooling_state": 2 },
  { "temp_mc": 60000, "hyst_mc": 2000, "severity": "critical", "cooling_state": 3 }
]
```

`state_pwm` on the actuator entry maps cooling state → duty:
`[0, 100, 160, 220, 255]`.  Edit those for different fan curves.
The slew limiter (`slew_per_tick = 8`) caps the per-tick PWM
delta so the fan ramps smoothly instead of jumping.

## Troubleshooting

| Symptom                                      | Likely cause + fix                                              |
|----------------------------------------------|-----------------------------------------------------------------|
| `T=  ERR` in the status line                 | DS18B20 isn't on the bus.  Check pull-up resistor + GPIO 6 wiring. |
| `T= 85.00 C` permanently (fallback temp)    | DS18B20 returned 85 °C power-on default, OR the sensor's 1-Wire address isn't being read.  Confirm `idf.py monitor` shows boot-time discovery. |
| `tach=    0 ticks/s` while fan is spinning   | Tach pull-up missing.  Add 10 kΩ from GPIO 5 to 3V3.            |
| Fan stays at duty 0 above 30 °C              | PWM wire not on GPIO 4, or fan rev is non-PWM.  Verify with a scope: GPIO 4 should show ~25 kHz at 3.3 V swing. |
| `idf.py: ESP-IDF v5.5.2 not found`           | `. ~/esp/esp-idf/export.sh` not run, or IDF installed elsewhere. Set `IDF_PATH`. |
| Status line ASCII looks corrupted in monitor | Wrong terminal encoding.  `idf.py monitor` uses UTF-8.  Use a real terminal, not a stripped-down VS Code embedded one. |
| Random reboot every few seconds              | USB cable can't deliver enough current for both C3 + fan during spin-up.  Use a powered hub or a separate 5 V supply for the fan. |

## What to read next

- [PRD §8.3](../thermal-core-prd.md) — the formal contract for
  STANDALONE + REPLAY_STANDALONE + HIL_PERIPHERAL build modes.
- [Implementation plan Stage 13](../thermal-core-implementation-plan.md) —
  how the firmware was brought up commit-by-commit.
- [`docs/getting-started/linux-pi4.md`](linux-pi4.md) — same
  core, different process.
- [`docs/getting-started/hil-peripheral.md`](hil-peripheral.md) —
  daemon-driven variant where the host runs the core.
