# HIL_PERIPHERAL deployment

> ⚠️ **Experimental.**  Stage 14d (CI HIL leg + PTY integration
> test scaffold) is not yet merged at the time this page was
> written.  The HIL loop is bench-tested but doesn't yet have
> automated regression coverage.  Wire-format stability is good
> — `TC` binary frames + canonical `TSIG_HIL_*` signal IDs — but
> expect rough edges around staleness handling, on-stop fan
> behaviour, and ACK retry logic.

In HIL_PERIPHERAL mode the **ESP32-C3 owns the hardware** (DS18B20
sensor + Noctua NF-A8 PWM fan, same wiring as the STANDALONE
build), while the **Linux daemon owns the control loop**
(`thermal_core_step()`).  The two sides talk binary `TC` frames
over a single USB-CDC cable:

```
   ┌───────────────────────────────────────────────────────────┐
   │ Linux host (Pi4 or x86)                                   │
   │   thermalcored  (test/integration/hil-config.json)        │
   │     ├─ build snapshot from TELEM_SAMPLE rx cache          │
   │     ├─ thermal_core_step(&core, &snap, &out)              │
   │     └─ emit CMD_REQUEST(HIL_CMD_SET_PWM_DUTY) frames      │
   └─────────────────────────┬─────────────────────────────────┘
                             │  USB-C cable
                             │
                  ┌──────────▼──────────────────────────────┐
                  │   /dev/ttyACM0  (USB-Serial-JTAG)       │
                  │   binary TC frames in both directions   │
                  └──────────┬──────────────────────────────┘
                             │
   ┌─────────────────────────▼─────────────────────────────────┐
   │ ESP32-C3 SuperMini  (HIL_PERIPHERAL firmware)             │
   │   app_main_hil_peripheral():                              │
   │     ├─ read DS18B20 + tach                                │
   │     ├─ encode TELEM_SAMPLE(TSIG_HIL_SENSOR_TEMP(0), ...)  │
   │     ├─ drain inbound CMD_REQUEST frames                   │
   │     └─ apply duty via LEDC PWM, emit CMD_ACK              │
   └───────────────────────────────────────────────────────────┘
```

Both sides share the **same TC binary framing** as the UDP
control plane — no second ad-hoc protocol.  See PRD §8.3 for
the formal contract.

## Why this topology matters

This is the white-paper "same probe, same plots" thesis: the
same `core/` C99 source runs on the ESP32 (STANDALONE), on
Linux/tmpfs (Pi4 deployment), and on Linux/serial-HIL, with the
same `thermalcore-probe` tool capturing UDP telemetry and the
same canonical scenarios generating the plots.  HIL_PERIPHERAL
proves that the daemon-runs-core configuration produces
indistinguishable behaviour from the firmware-runs-core
configuration, modulo USB-CDC + sensor jitter.

## Hardware

Identical to the [STANDALONE bench rig](esp32-standalone.md):
ESP32-C3 + DS18B20 + Noctua NF-A8 PWM fan.  Same GPIO pin map,
same pull-ups.  The only difference is which firmware is flashed.

Plus a USB-C cable from the ESP32 to a Linux host (Pi4 or x86).
The host needs to be able to read/write `/dev/ttyACM0` (you may
need to add yourself to the `dialout` group).

## Software

### On the ESP32: flash the HIL build

```bash
. ~/esp/esp-idf/export.sh
cd platform/esp32_idf

# Switch to HIL_PERIPHERAL mode (mutually exclusive with REPLAY).
idf.py fullclean
idf.py -DTHERMALCORE_HIL_PERIPHERAL=ON build

# Flash:
idf.py -p /dev/ttyACM0 flash
```

**Don't** start `idf.py monitor` — in HIL mode the firmware
suppresses `ESP_LOG` (binary frames can't share the channel
with ASCII log lines) and emits only binary TC frames.  If you
run the monitor, you'll see line noise.

### On the host: build the daemon

```bash
sudo apt-get install build-essential
git clone <repo-url> thermal-core
cd thermal-core
make build
```

### On the host: run the daemon with HIL config

A ready-made config lives at
[`test/integration/hil-config.json`](../../test/integration/hil-config.json).
It has the same step_wise zone as the ESP32 STANDALONE build
(trips at 30 / 45 / 60 °C), telemetry on UDP 9030, and
`hil.transport = "serial:/dev/ttyACM0"`.

```bash
sudo build/platform-linux/thermalcored --config=test/integration/hil-config.json
```

(`sudo` only needed if your user can't read/write `/dev/ttyACM0`.)

You should see:

```
bsp_hil_serial: opened /dev/ttyACM0 @ 115200 baud (HIL_PERIPHERAL)
```

… and nothing else on the daemon's stdout in the happy path.
NACK frames (if any) appear on stderr; on shutdown the daemon
prints a summary count of ACKs + NACKs.

## Monitoring the loop

The daemon **still** emits its UDP telemetry in HIL mode — the
ESP32-as-peripheral arrangement does not change the host
observability story.  Run the probe in another terminal:

```bash
python3 tools/thermalcore_probe.py \
    --listen udp:127.0.0.1:9030 \
    --log /tmp/hil-telemetry.csv \
    --duration 60
```

Heat the DS18B20 on the ESP32 side and watch the CSV:

```
ts_ms,kind,id,value,a1,a2,a3,a4
0,sample,256,28125,,,,                    ← TSIG_ZONE_TEMP(0)
0,sample,512,0,,,,                        ← TSIG_ACTUATOR_DUTY(0)
100,sample,256,28250,,,,
...
3500,sample,256,30200,,,,
3500,sample,512,100,,,,                   ← duty jumps as trip 30 °C crosses
```

The fan on the ESP32 audibly steps up.  Side-by-side comparison
with [the same plot from the STANDALONE build](esp32-standalone.md)
shows the curves overlap closely (settling-time tolerance bands
per PRD §15 are within ±10%).

## Wire-format anatomy

Each direction uses the canonical `TC` frame from PRD §7.2:

```
 0       1       2       3       4..5     6..7         8..11
+-------+-------+-------+-------+--------+------------+----------+
| 'T'   | 'C'   | ver=1 | opcode| seq LE | pld_len LE | ts_ms LE |
+-------+-------+-------+-------+--------+------------+----------+
                                       12..(12+pld_len-1)
                                       [ payload bytes ]
                              (12+pld_len) .. (12+pld_len+1)
                              [ crc16 LE -- CRC-16/CCITT-FALSE ]
```

**ESP32 → daemon (TELEM_SAMPLE, opcode 0x01):**

| Field            | Type    | Notes                                   |
|------------------|---------|-----------------------------------------|
| signal_id        | u16 LE  | `TSIG_HIL_SENSOR_TEMP(slot)` = `0x0700 + slot` or `TSIG_HIL_TACH_RPM(slot)` = `0x0710 + slot` |
| flags            | u16 LE  | reserved; 0 in v1                       |
| value            | i32 LE  | temp_mc or RPM                          |

**daemon → ESP32 (CMD_REQUEST, opcode 0x10):**

| Field            | Type    | Notes                                   |
|------------------|---------|-----------------------------------------|
| command_id       | u16 LE  | `HIL_CMD_SET_PWM_DUTY = 0x8001` (platform-private; PRD §8.3 reserves `0x8000+`) |
| duty_0_255       | u8      | maps to LEDC PWM register on the ESP32  |

**ESP32 → daemon (CMD_ACK, opcode 0x11):**

| Field            | Type    | Notes                                   |
|------------------|---------|-----------------------------------------|
| request_seq      | u16 LE  | echoes the CMD_REQUEST's seq field      |
| status           | u16 LE  | 0 = OK                                  |
| detail_code      | u32 LE  | duty value the firmware actually applied (sanity check) |

CRC is **required** on USB-CDC per PRD §7.2 — the channel isn't
lossless and a single corrupted byte must invalidate a frame
rather than be applied as a fan command.

## Capturing raw frames for debugging

If the daemon-side decoder isn't seeing samples, capture the
raw byte stream from the ESP32:

```bash
# Stop the daemon first (it's holding /dev/ttyACM0 exclusively).
sudo killall thermalcored

# Capture 5 s of raw frames:
sudo cat /dev/ttyACM0 > /tmp/hil-raw.bin &
sleep 5 && sudo kill %1
```

Decode them with the Python wire module:

```bash
python3 - <<'PY'
import sys
sys.path.insert(0, 'tools')
import thermalcore_wire as w
data = open('/tmp/hil-raw.bin','rb').read()
i = 0; frames = 0
while True:
    j = data.find(b'TC', i)
    if j < 0: break
    if j + 14 > len(data): break
    dec = w.decode_tc(data[j:j+256], crc=True)
    if dec:
        opcode, seq, ts_ms, payload = dec
        print(f"opcode=0x{opcode:02x} seq={seq} ts={ts_ms} payload={payload.hex()}")
        frames += 1
        i = j + 14 + len(payload) + 2
    else:
        i = j + 1
print(f"---\nDecoded {frames} frames.")
PY
```

You should see roughly 20 frames per second alternating between
opcode `0x01` (TELEM_SAMPLE) for sensor + tach.  If you see
zero, the firmware isn't in HIL mode (check that you flashed
the right build, not the STANDALONE one).

## Limitations (Stage 14c)

These all land in later stages:

- **No PTY integration test.**  Stage 14d will add
  `test/integration/test_hil_serial.py` that uses a Python
  `pty` pair to feed crafted TELEM_SAMPLE frames into the
  daemon and observe its CMD_REQUEST egress, closing the
  current coverage gap on `bsp_hil_serial.c`.
- **No staleness timeout on HIL samples.**  If the ESP32 stops
  sending, the daemon keeps using the last-seen temp.  Fallback
  kicks in only when the sample was never seen at all.
- **No on-stop "fan off" command.**  When the daemon exits, the
  ESP32 keeps the last commanded duty until power-cycled.
- **No guaranteed-delivery retry.**  The daemon re-sends the
  current duty every 100 ms, so a single lost CMD_REQUEST
  self-heals on the next tick.  But there's no "command queue"
  abstraction.
- **No multi-actuator support.**  The wire protocol only carries
  `HIL_CMD_SET_PWM_DUTY` for actuator slot 0.  PRD §8.3 leaves
  the 0x8000+ command-ID space open for future expansion
  (`SET_PWM_FREQ`, `RESET_TACH_COUNTER`, etc.).
- **No CI HIL build leg yet.**  Stage 14d adds the
  `HIL_PERIPHERAL` matrix entry to `build-esp32` and the
  size-budget assertion against it.

## Troubleshooting

| Symptom                                       | Likely cause + fix                                                |
|-----------------------------------------------|-------------------------------------------------------------------|
| `bsp_hil_serial: open ... failed: Permission denied` | Add your user to `dialout`: `sudo usermod -aG dialout $USER` + log out / in. |
| Daemon opens the device but no telemetry      | Wrong firmware — flashed STANDALONE not HIL.  Re-flash with `-DTHERMALCORE_HIL_PERIPHERAL=ON`. |
| ESP_LOG noise on the wire (ASCII garbage)     | Firmware is in STANDALONE mode (which prints status lines).  Same fix as above. |
| `NACK at <ms> req_seq=N status=0x8001` repeats| Length mismatch.  The daemon is sending a CMD_REQUEST with the wrong payload size.  Probably a thermal_wire_crc16 regression — check the wire codec hasn't drifted. |
| Daemon exits with "samples budget too small"  | `THERMAL_MAX_SAMPLES_PER_SNAPSHOT` too small for `sensor_count + actuator_count + context_count`.  Trim the config or bump the constant. |
| Fan never spins                               | The CMD_REQUEST frames aren't arriving on the ESP32 side.  Check both ends are on the same USB device (`lsof /dev/ttyACM0` should show only `thermalcored`). |
| `T= 85.00 C` plateau on the host telemetry    | DS18B20 returned the 85 °C power-on default on the ESP32 — the firmware then forwards that.  Fix the sensor wiring on the ESP32, not the daemon. |

## What to read next

- [PRD §8.3](../thermal-core-prd.md) — the formal
  HIL_PERIPHERAL contract.
- [Implementation plan Stage 14](../thermal-core-implementation-plan.md) —
  how the HIL loop was brought up commit-by-commit (14a
  outbound, 14b inbound, 14c daemon transport, 14d close).
- [`protocol/thermal_wire.h`](../../protocol/thermal_wire.h) —
  TC framing API.
- [`platform/linux/bsp_hil_serial.c`](../../platform/linux/bsp_hil_serial.c)
  — the daemon-side BSP (frame accumulator + cache + encoder).
