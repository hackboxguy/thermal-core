# Linux daemon on a Raspberry Pi 4

The Pi4 deployment runs the same `core/` C99 source as the
ESP32-C3 STANDALONE build, but as a Linux daemon
(`thermalcored`).  The BSP layer talks to:

- **DS18B20** via the kernel's 1-Wire subsystem (`/sys/bus/w1/`).
- **PWM fan** via the kernel's `pwm-fan` driver, which exposes
  `/sys/class/hwmon/.../pwm1` + `fan1_input` after a small
  device-tree overlay binds the hardware PWM channel and the
  tach GPIO interrupt together into one hwmon node.

The benefit of the DT-overlay path is that there is **no extra
runtime daemon** required for tach reading — the kernel counts
edges on the GPIO interrupt for you and exposes the rolling RPM
directly through hwmon.  The downside is that you have to write
+ compile + load a DT overlay once.  This page walks through
that.

## Bill of materials

| Item              | Notes                                                 |
|-------------------|-------------------------------------------------------|
| Raspberry Pi 4    | Raspberry Pi OS Bookworm or later (kernel >= 6.1).    |
| Noctua NF-A8 5V PWM| 4-wire fan: +5V, GND, PWM in, tach out.              |
| DS18B20           | TO-92 package, 1-Wire digital temperature sensor.     |
| 4.7 kΩ resistor   | Pull-up for the DS18B20 1-Wire data line.             |
| 10 kΩ resistor    | Pull-up for the fan tach (open-collector output).     |
| 5 V supply        | Fan red wire → Pi 5V pin (the Pi can source plenty).  |
| USB-C / micro-USB | Power the Pi.                                         |

The Noctua NF-A8 needs 5 V on its red wire; the Pi4's pin-2 / pin-4
(5V pin) work fine.  PWM + tach signals are 3.3 V on the Pi side.

## Pin map

| Signal       | Pi4 header pin | BCM GPIO | Wire to                       |
|--------------|----------------|----------|-------------------------------|
| Fan +5 V     | pin 2 (5V)     | —        | Fan red wire                  |
| Fan GND      | pin 6 (GND)    | —        | Fan black wire                |
| Fan PWM out  | pin 32         | GPIO 12  | Fan blue wire                 |
| Fan tach in  | pin 11         | GPIO 17  | Fan yellow wire + 10 kΩ pull-up to 3V3 |
| DS18B20 +    | pin 1 (3V3)    | —        | DS18B20 pin 3 (VDD)           |
| DS18B20 GND  | pin 9 (GND)    | —        | DS18B20 pin 1                 |
| DS18B20 data | pin 7          | GPIO 4   | DS18B20 pin 2 + 4.7 kΩ pull-up to 3V3 |

GPIO 12 is used because it supports hardware PWM channel 0
(alt function 4).  GPIO 17 has nothing special about it — any
GPIO that the kernel can attach an edge interrupt to will do for
tach; 17 is convenient.  GPIO 4 is the kernel's 1-Wire default.

## One-time setup

### 1. Enable 1-Wire on GPIO 4

Edit `/boot/firmware/config.txt` (or `/boot/config.txt` on
older RPi OS):

```ini
# 1-Wire on GPIO 4 (the kernel default).
dtoverlay=w1-gpio
```

Reboot.  Verify with:

```bash
ls /sys/bus/w1/devices/
# Expect: 28-XXXXXXXXXXXX  w1_bus_master1
```

The `28-...` directory is your DS18B20.  Read the temperature:

```bash
cat /sys/bus/w1/devices/28-XXXXXXXXXXXX/temperature
# Expect: 25125    (millidegrees Celsius)
```

### 2. Build + load the pwm-fan DT overlay

The stock Raspberry Pi OS does **not** include a `pwm-fan`
overlay that combines hardware PWM + tach, so we write a small
one.  Save the following as `thermal-core-pwm-fan.dts` somewhere
on the Pi:

```dts
/dts-v1/;
/plugin/;

/ {
    compatible = "brcm,bcm2711";

    /* Enable hardware PWM channel 0 on GPIO 12, alt func 4. */
    fragment@0 {
        target = <&gpio>;
        __overlay__ {
            pwm_pins: pwm_pins {
                brcm,pins = <12>;
                brcm,function = <4>;     /* alt0 */
            };
            tach_pin: tach_pin {
                brcm,pins = <17>;
                brcm,function = <0>;     /* input */
                brcm,pull = <2>;         /* pull-up */
            };
        };
    };

    fragment@1 {
        target = <&pwm>;
        __overlay__ {
            pinctrl-names = "default";
            pinctrl-0 = <&pwm_pins>;
            status = "okay";
        };
    };

    fragment@2 {
        target-path = "/";
        __overlay__ {
            pwm-fan {
                compatible = "pwm-fan";
                pwms = <&pwm 0 40000 0>;   /* channel 0, 25 kHz period (40 us) */
                interrupt-parent = <&gpio>;
                interrupts = <17 1>;       /* GPIO 17, rising-edge */
                pinctrl-names = "default";
                pinctrl-0 = <&tach_pin>;
                #cooling-cells = <2>;
                cooling-levels = <0 64 128 192 255>;
                pulses-per-revolution = <2>;
                status = "okay";
            };
        };
    };
};
```

Compile + install:

```bash
dtc -@ -I dts -O dtb -o thermal-core-pwm-fan.dtbo thermal-core-pwm-fan.dts
sudo cp thermal-core-pwm-fan.dtbo /boot/firmware/overlays/
```

Add to `/boot/firmware/config.txt`:

```ini
dtoverlay=thermal-core-pwm-fan
```

Reboot.  Verify:

```bash
ls /sys/class/hwmon/
# Expect: hwmon0  hwmon1  hwmon2  hwmon3   (the count depends on Pi peripherals)

# Find the one that's `pwm-fan`:
for h in /sys/class/hwmon/hwmon*; do
    name=$(cat "$h/name" 2>/dev/null)
    echo "$h: $name"
done
# Expect a line like: /sys/class/hwmon/hwmon3: pwm-fan

# Sanity check: read RPM (will be 0 if the fan isn't spinning):
cat /sys/class/hwmon/hwmon3/fan1_input
# Set a duty manually:
echo 128 | sudo tee /sys/class/hwmon/hwmon3/pwm1
# Re-read RPM after a second:
cat /sys/class/hwmon/hwmon3/fan1_input
# Expect: ~1500 RPM for an NF-A8 at duty 128.
```

If `fan1_input` reads 0 while the fan is spinning audibly, the
tach pull-up is missing or the wire is on the wrong GPIO.  If
`pwm1` writes have no audible effect, the PWM overlay isn't
loaded.

### 3. Build the daemon

```bash
sudo apt-get install build-essential
git clone <repo-url> thermal-core
cd thermal-core
make build
```

The `thermalcored` binary lands at
`build/platform-linux/thermalcored`.

### 4. Write the Pi4 daemon config

The daemon JSON config has to:

1. Name the actual DS18B20 path in `sensors[].source`.
2. Name the actual hwmon `pwm1` path in `actuators[].pwm`.
3. Name the actual hwmon `fan1_input` path in `actuators[].tach`.

Substitute the `28-XXXXXXXXXXXX` ID you saw above for the
sensor, and the right `hwmonN` for the actuator.  A
ready-to-edit template:

```json
{
  "config_version": 1,
  "control_period_ms": 100,

  "sensors": [
    {
      "id": 0,
      "name": "soc",
      "iir_alpha_q16": 16384,
      "max_staleness_ms": 5000,
      "source": "/sys/bus/w1/devices/28-XXXXXXXXXXXX/temperature"
    }
  ],

  "context_signals": [],

  "actuators": [
    {
      "id": 0,
      "name": "main_fan",
      "pwm_min": 0,
      "pwm_max": 255,
      "slew_per_tick": 8,
      "spinup_pwm": 0,
      "spinup_ms": 0,
      "state_pwm": [0, 100, 160, 220, 255],
      "pwm": "/sys/class/hwmon/hwmon3/pwm1",
      "tach": "/sys/class/hwmon/hwmon3/fan1_input",
      "pwm_freq_hz": 25000,
      "tach_pulses_per_rev": 2
    }
  ],

  "zones": [
    {
      "name": "soc_zone",
      "sensors": ["soc"],
      "aggregation": "max",
      "fallback_temp_mc": 85000,
      "governor": "step_wise",
      "actuators": ["main_fan"],
      "trips": [
        { "temp_mc": 30000, "hyst_mc": 2000, "severity": "warn",     "cooling_state": 1 },
        { "temp_mc": 45000, "hyst_mc": 2000, "severity": "warn",     "cooling_state": 2 },
        { "temp_mc": 60000, "hyst_mc": 2000, "severity": "critical", "cooling_state": 3 },
        { "temp_mc": 85000, "hyst_mc": 2000, "severity": "critical", "cooling_state": 4 }
      ]
    }
  ],

  "policy_modifiers": [],

  "fault_detection": {
    "stall": {
      "enabled": true,
      "severity": "critical",
      "action": "force_pwm_max_until_recovered",
      "persist_ticks": 5,
      "recovery_ticks": 10,
      "stall_rpm": 200,
      "stall_pwm_threshold": 80
    }
  },

  "telemetry": {
    "enable": true,
    "period_ticks": 1,
    "signals": ["zone_temp_*", "actuator_pwm_*", "actuator_rpm_*"],
    "transport": "udp:127.0.0.1:9030"
  },

  "control": {
    "listen": "",
    "enable": false
  }
}
```

Save as `~/pi4-config.json`.

### 5. Run the daemon

```bash
sudo build/platform-linux/thermalcored --config=$HOME/pi4-config.json
```

(`sudo` is needed because the hwmon `pwm1` file is root-writable
by default.  Add yourself to a `gpio` group + a udev rule if you
prefer to run unprivileged.)

The daemon prints nothing on stdout in the happy path; UDP
telemetry goes to `127.0.0.1:9030`.  Press `Ctrl-C` to stop.

## Monitoring

Start the probe in another terminal:

```bash
python3 tools/thermalcore_probe.py \
    --listen udp:127.0.0.1:9030 \
    --log /tmp/pi4-telemetry.csv \
    --duration 60
```

While that runs, heat the DS18B20 (a finger works for ~32 °C;
a hairdryer or a soldering-iron tip nearby for higher).  After
60 s the probe writes `/tmp/pi4-telemetry.csv` containing one
row per `TELEM_SAMPLE` frame:

```
ts_ms,kind,id,value,a1,a2,a3,a4
0,sample,256,28125,,,,
100,sample,256,28125,,,,
...
3500,sample,256,30200,,,,
3500,sample,512,100,,,,           ← duty steps to 100 at 30.2 °C
...
```

Signal ID `0x0100 = 256` is `TSIG_ZONE_TEMP(0)`; `0x0200 = 512`
is `TSIG_ACTUATOR_DUTY(0)`.  See [`core/thermal_signals.h`](../../core/thermal_signals.h)
for the full table.

Plot in `gnuplot`, Python, or any spreadsheet:

```bash
python3 - <<'PY'
import csv, matplotlib.pyplot as plt
rows = [r for r in csv.DictReader(open("/tmp/pi4-telemetry.csv"))]
t = [int(r["ts_ms"])/1000 for r in rows if r["id"] == "256"]
T = [int(r["value"])/1000 for r in rows if r["id"] == "256"]
p = [int(r["value"]) for r in rows if r["id"] == "512"]
pt= [int(r["ts_ms"])/1000 for r in rows if r["id"] == "512"]
fig, ax1 = plt.subplots()
ax1.plot(t, T, "r"); ax1.set_ylabel("Temp (°C)")
ax2 = ax1.twinx()
ax2.plot(pt, p, "b"); ax2.set_ylabel("Duty (0..255)")
plt.show()
PY
```

## Tuning

Edit the trips, `state_pwm`, or `slew_per_tick` in
`~/pi4-config.json`; restart the daemon.  No re-build needed —
the daemon parses the JSON at startup.

For a quieter fan curve, raise the first trip to 40 °C and lower
`state_pwm[1]` to ~60.  For a more aggressive curve, drop the
hysteresis bands (`hyst_mc`) — but watch for chattering at the
boundary.

## Troubleshooting

| Symptom                                  | Likely cause + fix                                              |
|------------------------------------------|-----------------------------------------------------------------|
| `thermalcored: config: source / pwm / tach path doesn't exist` | DT overlay didn't load.  Check `dmesg | grep pwm-fan`. |
| `cat /sys/.../fan1_input` returns 0 with fan spinning | Tach pull-up missing, or GPIO 17 not pulled up by the overlay.  Add 10 kΩ to 3V3 externally. |
| Daemon runs but PWM never changes        | `pwm1_enable` is 0.  `echo 1 | sudo tee /sys/class/hwmon/hwmonN/pwm1_enable`. Or set it via the overlay's `pwm-fan` node. |
| Daemon exits immediately with "permission denied" | hwmon files are root-only by default.  Run with `sudo`, or write a udev rule that chowns them to a non-root group. |
| `T= 85.00 C` plateau                     | DS18B20 returned its 85 °C power-on default; the kernel timed out on the conversion.  Check the 4.7 kΩ pull-up + the data wire integrity. |
| Stall fault fires when fan is running    | `stall_rpm = 200` is below the fan's idle RPM at low duty.  Either lower `stall_rpm` (e.g., to 100) or raise `stall_pwm_threshold` so it doesn't fire at low duties. |

## What's happening on the host

The Linux daemon's main loop ([`thermalcored.c`](../../platform/linux/thermalcored.c))
does the same five steps the ESP32 STANDALONE firmware does,
just with file-backed BSPs instead of GPIO drivers.  The
`thermal_core_step()` call is byte-identical between the two
platforms — same C source, same arithmetic, same fault state-
machine.

## What to read next

- [PRD §5.1](../thermal-core-prd.md) — full JSON config schema
  (every field, default, range).
- [`docs/getting-started/esp32-standalone.md`](esp32-standalone.md) —
  same core, on bare metal.
- [`docs/getting-started/hil-peripheral.md`](hil-peripheral.md) —
  the Pi4 (or any Linux host) driving an ESP32-C3's hardware
  over USB-CDC.
