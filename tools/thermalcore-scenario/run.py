#!/usr/bin/env python3
"""tools/thermalcore-scenario/run.py — scenario runner orchestrator.

Stage 12 12b main entry point.  Drives one .scn file end-to-end:

  1. Parse .scn (scenario.py) + load daemon config (JSON).
  2. Set up /tmp/thermal-core-scenario tmpfs (sensors / pwm).
  3. Init the C plant via ctypes (plant_ffi.py).
  4. Spawn thermalcored under --clock=scenario with the daemon
     config; connect to its AF_UNIX TICK socket.
  5. Listen on UDP for telemetry via ProbeRecorder.
  6. Tick loop: read PWM -> plant.step -> write sensor -> TICK ->
     drain telemetry.
  7. Write the captured CSV.
  8. Evaluate every assertion against the captured records.
  9. Print scenario: PASS / FAIL <reason>; exit 0 / 1.

Locally `make scenario` invokes this:

    python3 tools/thermalcore-scenario/run.py \
        scenarios/idle_steady_state.scn \
        test/integration/scenario-config.json
"""
from __future__ import annotations

import argparse
import json
import os
import signal as pysignal
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools"))

import thermalcore_probe as probe   # noqa: E402
from plant_ffi import Plant         # noqa: E402
from scenario import parse_scenario # noqa: E402


# === Tunables ========================================================

DAEMON_PATH        = ROOT / "build" / "platform-linux" / "thermalcored"
CLOCK_SOCKET_PATH  = "/tmp/thermalcored-scenario-clock.sock"
TMPFS_DIR          = Path("/tmp/thermal-core-scenario")
CSV_OUT_PATH       = Path("/tmp/scenario-idle.csv")
SETTLE_MS          = 20    # post-TICK wait for daemon's actuator write


# === Helpers =========================================================

def setup_tmpfs(daemon_cfg: dict, initial_temp_mc: int) -> None:
    """Create the sensor + actuator tmpfs tree the daemon expects."""
    sensors = daemon_cfg.get("sensors", [])
    actuators = daemon_cfg.get("actuators", [])
    for s in sensors:
        Path(s["source"]).parent.mkdir(parents=True, exist_ok=True)
        Path(s["source"]).write_text(f"{initial_temp_mc}\n")
    for a in actuators:
        Path(a["pwm"]).parent.mkdir(parents=True, exist_ok=True)
        Path(a["pwm"]).write_text("0\n")
        if a.get("tach"):
            Path(a["tach"]).parent.mkdir(parents=True, exist_ok=True)
            Path(a["tach"]).write_text("1500\n")
    try:
        os.unlink(CLOCK_SOCKET_PATH)
    except FileNotFoundError:
        pass


def parse_telemetry_uri(uri: str) -> tuple[str, int]:
    if not uri.startswith("udp:"):
        raise SystemExit(f"scenario-runner: unsupported telemetry "
                         f"transport {uri!r}; expected udp:HOST:PORT")
    rest = uri[4:]
    host, _, port = rest.rpartition(":")
    return host, int(port)


def read_pwm(actuator_path: Path) -> int:
    try:
        raw = actuator_path.read_text().strip()
        return int(raw)
    except (OSError, ValueError):
        return 0


def write_sensor(sensor_path: Path, mc: int) -> None:
    sensor_path.write_text(f"{mc}\n")


# === Main ============================================================

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="thermalcore-scenario-run")
    parser.add_argument("scenario", help="path to .scn file")
    parser.add_argument("config",   help="path to daemon JSON config")
    parser.add_argument("--csv-out", default=str(CSV_OUT_PATH),
                        help=f"telemetry CSV output path "
                             f"(default: {CSV_OUT_PATH})")
    args = parser.parse_args(argv)

    if not DAEMON_PATH.exists():
        sys.stderr.write(
            f"scenario-runner: daemon binary missing at {DAEMON_PATH} "
            f"(run `make build` first)\n")
        return 1

    scn = parse_scenario(args.scenario)
    daemon_cfg = json.loads(Path(args.config).read_text())
    control_period_ms = int(daemon_cfg.get("control_period_ms", 100))

    ambient_mc      = int(scn.plant_config.get("ambient_mc",
                                                 50000))
    initial_temp_mc = int(scn.plant_config.get("initial_temp_mc",
                                                 ambient_mc))
    duration_ms     = scn.duration_ms

    # === Setup =========================================================
    setup_tmpfs(daemon_cfg, initial_temp_mc)

    # Telemetry UDP listener.
    telem_uri = daemon_cfg["telemetry"]["transport"]
    udp_host, udp_port = parse_telemetry_uri(telem_uri)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind((udp_host, udp_port))

    recorder = probe.ProbeRecorder(udp)

    # Init plant.
    plant = Plant(seed=42)
    plant.set_zone_count(1)
    plant.set_temperature(0, initial_temp_mc)
    plant.set_ambient(0, ambient_mc)
    # No load, no fan curve in 12b's idle scenario.  12c adds
    # plant_load_w_q16 / plant_fan_max / plant_fan_curve plant
    # directives.

    # === Spawn daemon =================================================
    log_path = Path("/tmp/thermalcored-scenario.log")
    log = log_path.open("w")
    daemon = subprocess.Popen(
        [str(DAEMON_PATH),
         f"--config={args.config}",
         "--clock=scenario",
         f"--scenario-clock-uri=unix:{CLOCK_SOCKET_PATH}"],
        stdout=log, stderr=log)

    # Wait for the daemon to create + bind the scenario clock
    # socket; then connect.
    deadline = time.time() + 2.0
    while not Path(CLOCK_SOCKET_PATH).exists():
        if time.time() > deadline or daemon.poll() is not None:
            udp.close()
            log.close()
            sys.stderr.write(
                f"scenario-runner: daemon never created clock socket "
                f"{CLOCK_SOCKET_PATH} (see {log_path})\n")
            return 1
        time.sleep(0.02)

    clk = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    clk.connect(CLOCK_SOCKET_PATH)

    # === Tick loop ====================================================
    actuator_path = Path(daemon_cfg["actuators"][0]["pwm"])
    sensor_path   = Path(daemon_cfg["sensors"][0]["source"])

    rc = 0
    try:
        now_ms = 0
        while now_ms <= duration_ms:
            # Read PWM the daemon wrote at the end of its previous
            # tick.  On the first tick this is the bootstrap 0
            # that setup_tmpfs() wrote.
            pwm = read_pwm(actuator_path)

            # Advance plant.  dt_ms = control_period_ms.  The
            # first plant.step at now_ms=0 effectively advances
            # "from t=-dt to t=0" -- harmless under steady state.
            plant.step([pwm], control_period_ms)

            # Write the new plant temp into the sensor file.
            write_sensor(sensor_path, plant.zone_temp_mc(0))

            # Send TICK; let the daemon do one control-loop pass.
            clk.sendall(f"TICK {now_ms}\n".encode("ascii"))
            time.sleep(SETTLE_MS / 1000.0)

            # Drain any telemetry the daemon emitted this tick.
            recorder.drain()

            now_ms += control_period_ms

        # Final drain: catch any trailing frames the kernel queued.
        recorder.drain()

    finally:
        clk.close()
        daemon.send_signal(pysignal.SIGTERM)
        try:
            daemon.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            daemon.kill()
        log.close()
        try:
            os.unlink(CLOCK_SOCKET_PATH)
        except FileNotFoundError:
            pass
        udp.close()

    # === Persist CSV + evaluate assertions ============================
    csv_path = Path(args.csv_out)
    recorder.write_csv(csv_path)
    print(f"scenario-runner: wrote {len(recorder.records)} records "
          f"to {csv_path}")

    failures = []
    for ass in scn.assertions:
        ok, detail = ass.evaluate(recorder.records)
        print(f"  {detail}")
        if not ok:
            failures.append(detail)

    if failures:
        print(f"scenario: FAIL ({len(failures)} assertion(s) failed)")
        rc = 1
    else:
        print("scenario: PASS")

    return rc


if __name__ == "__main__":
    sys.exit(main())
