#!/usr/bin/env python3
"""tools/thermalcore-scenario/run.py — scenario runner orchestrator.

Stage 12 main entry point.  Drives one .scn file end-to-end:

  1. Parse .scn (scenario.py) + load daemon config (JSON; from the
     scenario's `config <path>` directive, or from the CLI fallback).
  2. Set up tmpfs (sensors / pwm / tach) under the daemon-config
     paths.
  3. Init the C plant via ctypes (plant_ffi.py) using per-zone
     settings from the scenario.
  4. Spawn thermalcored under --clock=scenario; connect to the
     AF_UNIX TICK socket.
  5. Listen on UDP for telemetry via ProbeRecorder.
  6. Tick loop: apply scheduled commands -> read PWM(s) ->
     plant.step -> write sensor(s) (honouring frozen overrides) ->
     write tach(s) -> TICK -> drain telemetry.
  7. Write the captured CSV (named after the scenario stem).
  8. Evaluate every assertion against the captured records.
  9. Print scenario: PASS / FAIL <reason>; exit 0 / 1.

CLI (12c):

    python3 run.py <scenario.scn>
        # config path read from `config <path>` directive in the .scn

    python3 run.py <scenario.scn> <daemon_config.json>
        # 12b fallback: explicit daemon config arg
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
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools"))

import thermalcore_probe as probe   # noqa: E402
from plant_ffi import Plant         # noqa: E402
from scenario import parse_scenario, ZoneInit   # noqa: E402


# === Tunables ========================================================

DAEMON_PATH        = ROOT / "build" / "platform-linux" / "thermalcored"
EMULATOR_PATH      = ROOT / "build" / "car-can-emulator" / "car-can-emulator"
CLOCK_SOCKET_PATH  = "/tmp/thermalcored-scenario-clock.sock"
EMULATOR_TCP_PORT  = 8080
DONE_TIMEOUT_S     = 5.0   # max wait for daemon DONE handshake after TICK

# Default fan curve installed on every zone unless a scenario
# overrides it (no override grammar in 12c).  Linear from
# (0, 0) -> (255, Q16_ONE) so PWM=0 means no cooling and PWM=255
# means full-strength cooling.
Q16_ONE = 1 << 16
DEFAULT_FAN_CURVE = [(0, 0, 0), (255, Q16_ONE, 0)]


# === Override plumbing ===============================================

@dataclass
class Overrides:
    """Frozen sensor / tach values; runner writes these to tmpfs
    instead of the plant temp / tach."""
    sensors: dict = None    # name -> mc
    tachs:   dict = None    # name -> rpm

    def __post_init__(self):
        self.sensors = self.sensors or {}
        self.tachs = self.tachs or {}


# === Helpers =========================================================

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


def write_int_file(path: Path, value: int) -> None:
    path.write_text(f"{value}\n")


def setup_tmpfs(daemon_cfg: dict, zones: list[ZoneInit]) -> None:
    """Create sensor / pwm / tach files at the paths the daemon
    will read.  Initial sensor values come from the per-zone plant
    init (in declaration order; zone 0 -> sensors[0], etc.)."""
    sensors   = daemon_cfg.get("sensors", [])
    actuators = daemon_cfg.get("actuators", [])
    for i, s in enumerate(sensors):
        Path(s["source"]).parent.mkdir(parents=True, exist_ok=True)
        zone = zones[i] if i < len(zones) else ZoneInit()
        initial_mc = (zone.initial_temp_mc
                      if zone.initial_temp_mc is not None
                      else (zone.ambient_mc
                            if zone.ambient_mc is not None
                            else 50000))
        Path(s["source"]).write_text(f"{initial_mc}\n")
    for a in actuators:
        Path(a["pwm"]).parent.mkdir(parents=True, exist_ok=True)
        Path(a["pwm"]).write_text("0\n")
        if a.get("tach"):
            Path(a["tach"]).parent.mkdir(parents=True, exist_ok=True)
            Path(a["tach"]).write_text("1500\n")
    # Context signal source files (e.g. vehicle_speed used by the
    # stuck_sensor detector's correlated_context).  v1: seed with 0;
    # the scenario can override via freeze_input if it cares.
    for c in daemon_cfg.get("context_signals", []):
        Path(c["source"]).parent.mkdir(parents=True, exist_ok=True)
        Path(c["source"]).write_text("0\n")
    try:
        os.unlink(CLOCK_SOCKET_PATH)
    except FileNotFoundError:
        pass


def init_plant_from_scenario(plant: Plant,
                              zones: list[ZoneInit]) -> None:
    """Apply per-zone init from the scenario to the C plant."""
    plant.set_zone_count(max(1, len(zones)))
    for z, zinit in enumerate(zones):
        if zinit.initial_temp_mc is not None:
            plant.set_temperature(z, zinit.initial_temp_mc)
        if zinit.ambient_mc is not None:
            plant.set_ambient(z, zinit.ambient_mc)
        if zinit.load_w_q16 is not None:
            plant.set_load_w_q16(z, zinit.load_w_q16)
        if zinit.heat_capacity_q16 is not None:
            plant.set_heat_capacity_q16(z, zinit.heat_capacity_q16)
        if zinit.ambient_drift_q16 is not None:
            plant.set_ambient_drift_q16(z, zinit.ambient_drift_q16)
        if zinit.fan_max_q16 is not None:
            plant.set_fan_max_q16(z, zinit.fan_max_q16)
        if (zinit.coupling_neighbor is not None
                and zinit.coupling_q16 is not None):
            plant.set_coupling(z, zinit.coupling_neighbor,
                               zinit.coupling_q16)
        # Every zone gets a default linear fan curve.
        plant.set_fan_curve(z, DEFAULT_FAN_CURVE)


def _set_emulator_speed(kmh: int) -> None:
    """Open a short-lived TCP connection to the emulator's control
    port and send `speed N`.  Silently swallows ConnectionRefused
    (e.g. after kill_emulator -- emulator is gone)."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(("127.0.0.1", EMULATOR_TCP_PORT))
        s.sendall(f"speed {kmh}\n".encode())
        try:
            s.recv(256)
        except socket.timeout:
            pass
        s.close()
    except (ConnectionRefusedError, OSError):
        pass


def apply_commands(commands: list, now_ms: int, dt_ms: int,
                    plant: Plant, overrides: Overrides,
                    emulator_ref: dict) -> None:
    """Fire any scheduled commands whose timestamp falls in
    [now_ms, now_ms + dt_ms).  `emulator_ref` is a single-entry
    dict {'proc': subprocess.Popen|None} so the kill_emulator
    handler can mutate it (otherwise the caller wouldn't see the
    update)."""
    while commands and commands[0][0] < now_ms + dt_ms:
        ts, cmd, args = commands.pop(0)
        if cmd == "freeze_input":
            overrides.sensors[args[0]] = int(args[1])
        elif cmd == "unfreeze_input":
            overrides.sensors.pop(args[0], None)
        elif cmd == "freeze_tach":
            overrides.tachs[args[0]] = int(args[1])
        elif cmd == "unfreeze_tach":
            overrides.tachs.pop(args[0], None)
        elif cmd == "set_plant_load_w_q16":
            plant.set_load_w_q16(int(args[0]), int(args[1]))
        elif cmd == "set_plant_fan_max_q16":
            plant.set_fan_max_q16(int(args[0]), int(args[1]))
        elif cmd == "set_emulator_speed":
            _set_emulator_speed(int(args[0]))
        elif cmd == "kill_emulator":
            proc = emulator_ref.get("proc")
            if proc is not None and proc.poll() is None:
                proc.send_signal(pysignal.SIGTERM)
                try:
                    proc.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                emulator_ref["proc"] = None
        # Unknown commands already rejected by the parser.


def resolve_paths(daemon_cfg: dict) -> tuple[list, list, list]:
    """Return (sensor_paths, actuator_pwm_paths, actuator_tach_paths)
    aligned with the daemon's slot order."""
    sensors = daemon_cfg.get("sensors", [])
    actuators = daemon_cfg.get("actuators", [])
    return (
        [Path(s["source"]) for s in sensors],
        [Path(a["pwm"]) for a in actuators],
        [Path(a.get("tach", "")) if a.get("tach") else None
         for a in actuators],
    )


def detect_can_iface(daemon_cfg: dict) -> str | None:
    """Return the kernel CAN iface name (e.g. 'vcan0') iff the
    daemon config has a context source of the form 'canbus:<iface>'.
    None otherwise."""
    for c in daemon_cfg.get("context_signals", []):
        src = str(c.get("source", ""))
        if src.startswith("canbus:"):
            return src.split(":", 1)[1]
    return None


def vcan_available(iface: str) -> bool:
    return Path(f"/sys/class/net/{iface}").exists()


def recv_done(clk: socket.socket) -> None:
    """Block until the daemon writes 'DONE\\n' on the scenario
    clock socket.  Replaces the prior wall-clock SETTLE_MS sleep:
    once DONE arrives, the daemon has finished thermal_core_step
    (telemetry UDP sends already enqueued on the loopback) and
    has written its actuator files, so the runner can drain
    telemetry deterministically and advance to the next tick.

    Raises RuntimeError on EOF (daemon died) or socket timeout
    (DONE_TIMEOUT_S expired -- daemon is hung)."""
    clk.settimeout(DONE_TIMEOUT_S)
    buf = b""
    while b"\n" not in buf:
        chunk = clk.recv(64)
        if not chunk:
            raise RuntimeError("scenario clock socket EOF "
                               "(daemon died before DONE)")
        buf += chunk
    line = buf.split(b"\n", 1)[0]
    if line != b"DONE":
        raise RuntimeError(f"scenario clock: expected 'DONE', got {line!r}")


def name_to_slot(daemon_cfg: dict, kind: str, name: str) -> int | None:
    """Map a sensor or actuator name to its slot index for override
    plumbing.  Returns None on miss."""
    arr = daemon_cfg.get(kind, [])
    for i, obj in enumerate(arr):
        if obj.get("name") == name:
            return i
    return None


# === Main ============================================================

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="thermalcore-scenario-run")
    parser.add_argument("scenario", help="path to .scn file")
    parser.add_argument("config",   nargs="?", default=None,
                        help="(optional) daemon JSON config; overridden "
                             "by the .scn's `config` directive")
    parser.add_argument("--csv-out", default=None,
                        help="telemetry CSV output path; defaults to "
                             "/tmp/scenario-<stem>.csv")
    args = parser.parse_args(argv)

    if not DAEMON_PATH.exists():
        sys.stderr.write(
            f"scenario-runner: daemon binary missing at {DAEMON_PATH} "
            f"(run `make build` first)\n")
        return 1

    scn = parse_scenario(args.scenario)

    # Resolve daemon config path: scenario `config` directive wins;
    # fall back to CLI arg; error if neither.
    cfg_path = scn.config_path or args.config
    if cfg_path is None:
        sys.stderr.write(
            f"scenario-runner: {args.scenario} has no `config <path>` "
            f"directive and no CLI config arg was supplied\n")
        return 1
    cfg_path = Path(cfg_path)
    if not cfg_path.is_absolute():
        cfg_path = (ROOT / cfg_path).resolve()
    daemon_cfg = json.loads(cfg_path.read_text())
    control_period_ms = int(daemon_cfg.get("control_period_ms", 100))

    # CAN dependency: if the daemon config sources vehicle_speed (or
    # any context) from `canbus:<iface>`, this scenario needs vcan.
    can_iface = detect_can_iface(daemon_cfg)
    if can_iface is not None and not vcan_available(can_iface):
        scn_name = Path(args.scenario).stem
        print(f"scenario: SKIP {scn_name} (no {can_iface}; install with "
              f"`sudo modprobe vcan && sudo ip link add dev {can_iface} "
              f"type vcan && sudo ip link set up {can_iface}`)")
        return 0
    if can_iface is not None and not EMULATOR_PATH.exists():
        sys.stderr.write(
            f"scenario-runner: emulator binary missing at {EMULATOR_PATH}\n"
            f"  run `cmake -S tools/car-can-emulator -B "
            f"build/car-can-emulator && cmake --build "
            f"build/car-can-emulator` first\n")
        return 1

    duration_ms = scn.duration_ms
    zones = scn.zones[:scn.zone_count] or [ZoneInit()]

    # === Setup =========================================================
    setup_tmpfs(daemon_cfg, zones)

    sensor_paths, pwm_paths, tach_paths = resolve_paths(daemon_cfg)

    telem_uri = daemon_cfg["telemetry"]["transport"]
    udp_host, udp_port = parse_telemetry_uri(telem_uri)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind((udp_host, udp_port))
    recorder = probe.ProbeRecorder(udp)

    plant = Plant(seed=42)
    init_plant_from_scenario(plant, zones)

    overrides = Overrides()
    pending_commands = sorted(scn.commands, key=lambda c: c[0])

    # === Spawn car-can-emulator if this scenario needs CAN ===========
    emulator_ref = {"proc": None}
    emu_log = None
    if can_iface is not None:
        emu_log = Path(f"/tmp/car-can-emulator-scenario.log").open("w")
        emulator_ref["proc"] = subprocess.Popen(
            [str(EMULATOR_PATH),
             f"--node={can_iface}",
             f"--port={EMULATOR_TCP_PORT}"],
            stdout=emu_log, stderr=emu_log)
        # Let the emulator come up before the daemon tries to open
        # its CAN socket.
        time.sleep(0.5)
        if emulator_ref["proc"].poll() is not None:
            emu_log.close()
            sys.stderr.write(
                f"scenario-runner: emulator exited early "
                f"(rc={emulator_ref['proc'].returncode})\n")
            return 1

    # === Spawn daemon =================================================
    log_path = Path(f"/tmp/thermalcored-scenario.log")
    log = log_path.open("w")
    daemon = subprocess.Popen(
        [str(DAEMON_PATH),
         f"--config={cfg_path}",
         "--clock=scenario",
         f"--scenario-clock-uri=unix:{CLOCK_SOCKET_PATH}"],
        stdout=log, stderr=log)

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
    rc = 0
    try:
        now_ms = 0
        while now_ms <= duration_ms:
            # 1. Apply any commands scheduled in [now_ms, now_ms+dt).
            apply_commands(pending_commands, now_ms, control_period_ms,
                            plant, overrides, emulator_ref)

            # 2. Read PWM(s) the daemon wrote at the end of its
            #    previous tick.  pwms[i] = actuator i's PWM (0..255).
            pwms = [read_pwm(p) for p in pwm_paths]

            # 3. Plant step.  PWM array padded for plant_inputs_t.
            plant.step(pwms, control_period_ms)

            # 4. Write per-zone sensor files (or frozen overrides).
            sensors = daemon_cfg.get("sensors", [])
            for i, s in enumerate(sensors):
                if s["name"] in overrides.sensors:
                    write_int_file(sensor_paths[i],
                                    overrides.sensors[s["name"]])
                else:
                    write_int_file(sensor_paths[i],
                                    plant.zone_temp_mc(i))

            # 5. Tach overrides (no plant model for tach in v1; default
            #    is 1500 from setup_tmpfs; override if frozen).
            actuators = daemon_cfg.get("actuators", [])
            for i, a in enumerate(actuators):
                if a["name"] in overrides.tachs and tach_paths[i]:
                    write_int_file(tach_paths[i],
                                    overrides.tachs[a["name"]])

            # 6. TICK; wait for daemon DONE handshake.  Explicit
            #    handshake replaces the prior SETTLE_MS wall-clock
            #    sleep that raced under CI runner load (see
            #    recv_done docstring).
            clk.sendall(f"TICK {now_ms}\n".encode("ascii"))
            recv_done(clk)

            # 7. Drain.
            recorder.drain()
            now_ms += control_period_ms

        recorder.drain()

    finally:
        clk.close()
        daemon.send_signal(pysignal.SIGTERM)
        try:
            daemon.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            daemon.kill()
        log.close()
        # Reap emulator if the scenario didn't already kill it.
        emu_proc = emulator_ref.get("proc")
        if emu_proc is not None and emu_proc.poll() is None:
            emu_proc.send_signal(pysignal.SIGTERM)
            try:
                emu_proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                emu_proc.kill()
        if emu_log is not None:
            emu_log.close()
        try:
            os.unlink(CLOCK_SOCKET_PATH)
        except FileNotFoundError:
            pass
        udp.close()

    # === Persist CSV + evaluate assertions ============================
    if args.csv_out is None:
        scn_stem = Path(args.scenario).stem
        csv_path = Path(f"/tmp/scenario-{scn_stem}.csv")
    else:
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
