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

Pin map source: the `mcu_pinmap` section of
[`configs/esp32-c3-standalone.json`](../../platform/esp32_idf/configs/esp32-c3-standalone.json).
Change the GPIO numbers there and rebuild (`make build-esp32`)
if your wiring differs -- no C source edit needed.

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

## Adding a second fan or sensor

The pin map lives entirely in the JSON config since the
"JSON-driven pin map" refactor.  No C source edits required.

### Hardware

Recommended GPIO allocations on the C3 SuperMini (free pins
not used by the default 1-fan setup):

| Signal           | GPIO | Notes                                  |
|------------------|------|----------------------------------------|
| Fan 2 PWM out    | 7    | 25 kHz, 3.3 V                          |
| Fan 2 tach in    | 8    | 10 kΩ pull-up to 3.3 V                 |
| DS18B20 #2 data  | 6    | **Shared** with sensor 1 -- 1-Wire is multi-drop.  No extra pin or pull-up needed; the existing 4.7 kΩ on GPIO 6 covers both DS18B20s. |

Wire the second Noctua's red / black to the same 5 V / GND as
the first; tach + PWM go to the new GPIOs.  Wire the second
DS18B20's DQ in parallel with the first on GPIO 6 (VDD on 3V3,
GND on GND).

5 V current budget: two NF-A8s draw ~250 mA combined at full
duty.  USB bus power is usually fine; if the SuperMini browns
out during simultaneous spin-up, use a powered USB hub or
external 5 V for the fans.

### JSON edits

Three pieces in [`configs/esp32-c3-standalone.json`](../../platform/esp32_idf/configs/esp32-c3-standalone.json):

1. **Second sensor** under `"sensors"`:

   ```json
   { "id": 1, "name": "amp",
     "iir_alpha_q16": 16384, "max_staleness_ms": 5000 }
   ```

2. **Second actuator** under `"actuators"`:

   ```json
   { "id": 1, "name": "aux_fan",
     "pwm_min": 0, "pwm_max": 255, "slew_per_tick": 8,
     "spinup_pwm": 0, "spinup_ms": 0,
     "state_pwm": [0, 100, 160, 220, 255] }
   ```

   GPIO + PWM-frequency for the ESP32 build live in
   `mcu_pinmap` (step 4) -- the `sensors[]` / `actuators[]`
   blocks carry only the core control parameters.

3. **Pick a zone topology.**  Two common shapes:

   - **Two independent zones**, each owning one sensor + one
     fan (canonical "CPU + GPU" demo).  Each zone gets its own
     `trips` block.

   - **One zone aggregating both sensors with `"max"`**, both
     fans listed under `actuators`.  Hottest sensor drives both
     fans together.

4. **Extend `mcu_pinmap`** with the new GPIO assignments
   (this is the bit the firmware reads from):

   ```json
   "mcu_pinmap": {
     "sensors": [
       { "name": "soc", "onewire_gpio": 6 },
       { "name": "amp", "onewire_gpio": 6 }
     ],
     "actuators": [
       { "name": "main_fan", "pwm_gpio": 4, "tach_gpio": 5, "pwm_freq_hz": 25000 },
       { "name": "aux_fan",  "pwm_gpio": 7, "tach_gpio": 8, "pwm_freq_hz": 25000 }
     ]
   }
   ```

   The pinmap is **keyed by name** (matches `sensors[].name` /
   `actuators[].name`), so the JSON-side order doesn't matter
   and a typo is caught at build time.  All `onewire_gpio`
   values must be identical (v1 supports one shared bus).  The
   key is `mcu_pinmap` (not `esp32_pinmap`) so the same JSON
   schema carries over to future MCU targets — each port emits
   its own platform-scoped pinmap struct.

### Build + flash

```bash
. ~/esp/esp-idf/export.sh
cd platform/esp32_idf
idf.py build flash monitor
```

`json2static.py` regenerates the static config on every build,
so the new sensor / actuator slots are baked into the firmware.
The boot log lists the DS18B20 ROM addresses in slot order so
you can correlate them to your bench labels:

```
I (xxx) bsp_sensor: DS18B20 slot 0 on GPIO6, ROM=0x28ABCD...
I (xxx) bsp_sensor: DS18B20 slot 1 on GPIO6, ROM=0x28EF01...
I (xxx) bsp_pwm: PWM slot 0 on GPIO4 @ 25000 Hz, 8-bit
I (xxx) bsp_pwm: PWM slot 1 on GPIO7 @ 25000 Hz, 8-bit
I (xxx) bsp_tach: tach slot 0 on GPIO5 (negedge, 1000 us filter)
I (xxx) bsp_tach: tach slot 1 on GPIO8 (negedge, 1000 us filter)
```

ROM order is deterministic across reboots (the espressif/ds18b20
driver sorts by ROM address), so slot 0 ↔ ROM-A ↔ "soc" stays
stable.  If you swap which physical DS18B20 you want to call
"soc", reorder the names in the JSON's `sensors[]` block — the
ROM-to-slot mapping is fixed by the bus, but the slot-to-name
mapping is what the firmware uses.

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
