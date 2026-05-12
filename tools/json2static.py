#!/usr/bin/env python3
"""tools/json2static.py — JSON → C const-thermal_config_t codegen.

Reads a PRD §5.1 JSON config and emits a single C99 file declaring

    const thermal_config_t G_THERMAL_CFG = { ... };

The output struct, when canonicalised via support/thermal_config_hash.c,
hashes to the same digest as the same JSON loaded through
platform/linux/config_jsmn.c.  That equivalence is checked by
test/unit/test_json2static_roundtrip.c.

This script mirrors the C loader's enum maps, name resolution, and
telemetry wildcard expansion in Python so the round-trip works
without coupling to the C source.

Usage:
    python3 tools/json2static.py <config.json> [-o <output.c>] [--symbol G_NAME]

If -o is omitted, output goes to stdout.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# === Compile-time maxima (mirror core/thermal_config.h) ==============

MAX_ZONES               = 4
MAX_SENSORS             = 8
MAX_ACTUATORS           = 2
MAX_CONTEXT_SIGNALS     = 4
MAX_TRIPS_PER_ZONE      = 4
MAX_MODIFIERS           = 2
MAX_SAMPLES_PER_SNAPSHOT = 16
MAX_SENSORS_PER_ZONE    = 4
MAX_ACTUATORS_PER_ZONE  = 2
MAX_COOLING_STATES      = 5
MAX_CURVE_POINTS        = 8
MAX_TELEMETRY_SIGNALS   = 128
NAME_MAX                = 24

# === Enum maps (mirror config_jsmn.c) ================================

AGGREGATION_MAP = {
    "max":      0x01,
    "avg":      0x02,
    "weighted": 0x03,
}
GOVERNOR_MAP = {
    "step_wise": 0x01,
    "pid":       0x02,
}
TRIP_SEVERITY_MAP = {
    "warn":     0x01,
    "critical": 0x02,
    "shutdown": 0x03,
}
CONTEXT_UNIT_MAP = {
    "none":       0x00,
    "kmh":        0x01,
    "bool":       0x02,
    "rpm":        0x03,
    "celsius_mc": 0x04,
}
FAILSAFE_MAP = {
    "assume_stationary": 0x01,
    "hold_last":         0x02,
    "assume_value":      0x03,
}
MOD_STAGE_MAP = {
    "pre_governor_trip_offset": 0x01,
    "post_governor_pwm_cap":    0x02,
}
FAULT_SEVERITY_MAP = {
    "degraded": 0x01,
    "critical": 0x02,
}
FAULT_ACTION_MAP = {
    "none":                          0x00,
    "mark_degraded":                 0x01,
    "use_zone_fallback":             0x02,
    "force_pwm_max_until_recovered": 0x03,
    "force_pwm_max_and_latch":       0x04,
    "request_shutdown":              0x05,
}

# === Telemetry signal IDs (mirror core/thermal_signals.h) ===========

TSIG_ZONE_BASE     = 0x0100
TSIG_ACTUATOR_BASE = 0x0200
TSIG_PID_BASE      = 0x0300
TSIG_CONTEXT_BASE  = 0x0400
TSIG_MODIFIER_BASE = 0x0500
TSIG_FAULT_BASE    = 0x0600

def tsig_zone_temp(slot):              return TSIG_ZONE_BASE + 0x00 + slot
def tsig_zone_cooling_state(slot):     return TSIG_ZONE_BASE + 0x10 + slot
def tsig_actuator_duty(slot):          return TSIG_ACTUATOR_BASE + 0x00 + slot
def tsig_actuator_rpm(slot):           return TSIG_ACTUATOR_BASE + 0x10 + slot
def tsig_pid(zone, term):              return TSIG_PID_BASE + zone * 4 + term
def tsig_context_value(slot):          return TSIG_CONTEXT_BASE + 0x00 + slot
def tsig_modifier_pwm_cap(slot):       return TSIG_MODIFIER_BASE + 0x00 + slot
def tsig_fault_stall(slot):            return TSIG_FAULT_BASE + 0x00 + slot
def tsig_fault_stuck_sensor(slot):     return TSIG_FAULT_BASE + 0x10 + slot
def tsig_fault_runaway(slot):          return TSIG_FAULT_BASE + 0x20 + slot
def tsig_fault_stale_context(slot):    return TSIG_FAULT_BASE + 0x30 + slot

# === Errors ==========================================================

class ConfigError(Exception):
    pass

# === Loader (mirrors config_jsmn.c semantics) ========================

def expand_signal(sel: str, cfg) -> list[int]:
    """Mirror telemetry_expand_one in config_jsmn.c."""
    actuator_count = len(cfg["actuators"])
    sensor_count   = len(cfg["sensors"])
    zone_count     = len(cfg["zones"])
    context_count  = len(cfg["contexts"])
    modifier_count = len(cfg["modifiers"])

    if sel == "zone_temp_*":
        if zone_count == 0: raise ConfigError("wildcard expanded to nothing")
        return [tsig_zone_temp(s) for s in range(zone_count)]
    if sel == "zone_cooling_state_*":
        if zone_count == 0: raise ConfigError("wildcard expanded to nothing")
        return [tsig_zone_cooling_state(s) for s in range(zone_count)]
    if sel == "actuator_pwm_*":
        if actuator_count == 0: raise ConfigError("wildcard expanded to nothing")
        return [tsig_actuator_duty(s) for s in range(actuator_count)]
    if sel == "actuator_rpm_*":
        if actuator_count == 0: raise ConfigError("wildcard expanded to nothing")
        return [tsig_actuator_rpm(s) for s in range(actuator_count)]
    if sel == "pid_terms_*":
        if zone_count == 0: raise ConfigError("wildcard expanded to nothing")
        return [tsig_pid(z, t) for z in range(zone_count) for t in range(4)]
    if sel == "modifier_pwm_cap_*":
        if modifier_count == 0: raise ConfigError("wildcard expanded to nothing")
        return [tsig_modifier_pwm_cap(s) for s in range(modifier_count)]
    if sel == "fault_*":
        out = []
        out += [tsig_fault_stall(s) for s in range(actuator_count)]
        out += [tsig_fault_stuck_sensor(s) for s in range(sensor_count)]
        out += [tsig_fault_runaway(s) for s in range(zone_count)]
        out += [tsig_fault_stale_context(s) for s in range(context_count)]
        if not out: raise ConfigError("wildcard expanded to nothing")
        return out

    if sel == "speed_kmh":
        for i, c in enumerate(cfg["contexts"]):
            if c["name"] == "vehicle_speed":
                return [tsig_context_value(i)]
        raise ConfigError("speed_kmh requires a 'vehicle_speed' context")

    if sel.startswith("context_"):
        name = sel[len("context_"):]
        for i, c in enumerate(cfg["contexts"]):
            if c["name"] == name: return [tsig_context_value(i)]
        raise ConfigError(f"unknown context '{name}'")
    if sel.startswith("zone_temp_"):
        name = sel[len("zone_temp_"):]
        for i, z in enumerate(cfg["zones"]):
            if z["name"] == name: return [tsig_zone_temp(i)]
        raise ConfigError(f"unknown zone '{name}'")
    if sel.startswith("actuator_pwm_"):
        name = sel[len("actuator_pwm_"):]
        for i, a in enumerate(cfg["actuators"]):
            if a["name"] == name: return [tsig_actuator_duty(i)]
        raise ConfigError(f"unknown actuator '{name}'")
    if sel.startswith("actuator_rpm_"):
        name = sel[len("actuator_rpm_"):]
        for i, a in enumerate(cfg["actuators"]):
            if a["name"] == name: return [tsig_actuator_rpm(i)]
        raise ConfigError(f"unknown actuator '{name}'")
    if sel.startswith("pid_terms_"):
        name = sel[len("pid_terms_"):]
        for i, z in enumerate(cfg["zones"]):
            if z["name"] == name: return [tsig_pid(i, t) for t in range(4)]
        raise ConfigError(f"unknown zone '{name}'")

    raise ConfigError(f"unknown telemetry selector '{sel}'")


def normalise(raw: dict) -> dict:
    """Pull CORE-owned fields out of `raw` (the JSON document) into a
    normalised dict that mirrors thermal_config_t.  Platform-only
    fields are dropped (json2static is for CORE only; the daemon's
    runtime cfg lives elsewhere)."""
    if "config_version" not in raw:
        raise ConfigError("missing config_version")
    if "control_period_ms" not in raw:
        raise ConfigError("missing control_period_ms")

    def pop_enum(name, m):
        v = m.get(name)
        if v is None: raise ConfigError(f"unknown enum '{name}'")
        return v

    sensors = []
    for s in raw.get("sensors", []):
        sensors.append({
            "id":               int(s["id"]),
            "name":             s["name"],
            "iir_alpha_q16":    int(s["iir_alpha_q16"]),
            "max_staleness_ms": int(s["max_staleness_ms"]),
        })

    contexts = []
    for c in raw.get("context_signals", []):
        contexts.append({
            "id":            int(c["id"]),
            "name":          c["name"],
            "unit":          pop_enum(c["unit"], CONTEXT_UNIT_MAP),
            "iir_alpha_q16": int(c["iir_alpha_q16"]),
            "timeout_ms":    int(c["timeout_ms"]),
            "fail_safe":     pop_enum(c["fail_safe"], FAILSAFE_MAP),
        })

    actuators = []
    for a in raw.get("actuators", []):
        sp = a["state_pwm"]
        if len(sp) > MAX_COOLING_STATES:
            raise ConfigError("too many cooling states")
        actuators.append({
            "id":             int(a["id"]),
            "name":           a["name"],
            "pwm_min":        int(a["pwm_min"]),
            "pwm_max":        int(a["pwm_max"]),
            "slew_per_tick":  int(a.get("slew_per_tick", 0)),
            "spinup_pwm":     int(a.get("spinup_pwm", 0)),
            "spinup_ms":      int(a.get("spinup_ms", 0)),
            "state_pwm":      [int(x) for x in sp] + [0] * (MAX_COOLING_STATES - len(sp)),
        })

    def find_slot(arr, name, kind):
        for i, e in enumerate(arr):
            if e["name"] == name:
                return i
        raise ConfigError(f"unknown {kind} '{name}'")

    zones = []
    for z in raw.get("zones", []):
        sens_names = z.get("sensors", [])
        sens_ids   = [sensors[find_slot(sensors, n, "sensor")]["id"] for n in sens_names]
        act_names  = z.get("actuators", [])
        act_ids    = [actuators[find_slot(actuators, n, "actuator")]["id"] for n in act_names]
        weights    = [int(x) for x in z.get("sensor_weights_q16", [])]
        trips = []
        for t in z.get("trips", []):
            trips.append({
                "temp_mc":       int(t["temp_mc"]),
                "hyst_mc":       int(t["hyst_mc"]),
                "severity":      pop_enum(t["severity"], TRIP_SEVERITY_MAP),
                "cooling_state": int(t["cooling_state"]),
            })
        pid = z.get("pid", {})
        zones.append({
            "name":               z["name"],
            "sensor_ids":         sens_ids,
            "sensor_weights_q16": weights,
            "sensor_count":       len(sens_ids),
            "aggregation":        pop_enum(z["aggregation"], AGGREGATION_MAP),
            "fallback_temp_mc":   int(z["fallback_temp_mc"]),
            "governor":           pop_enum(z["governor"], GOVERNOR_MAP),
            "pid": {
                "kp_q16":          int(pid.get("kp_q16", 0)),
                "ki_q16":          int(pid.get("ki_q16", 0)),
                "kd_q16":          int(pid.get("kd_q16", 0)),
                "setpoint_mc":     int(pid.get("setpoint_mc", 0)),
                "kp_min_q16":      int(pid.get("kp_min_q16", 0)),
                "kp_max_q16":      int(pid.get("kp_max_q16", 0)),
                "ki_min_q16":      int(pid.get("ki_min_q16", 0)),
                "ki_max_q16":      int(pid.get("ki_max_q16", 0)),
                "kd_min_q16":      int(pid.get("kd_min_q16", 0)),
                "kd_max_q16":      int(pid.get("kd_max_q16", 0)),
                "setpoint_min_mc": int(pid.get("setpoint_min_mc", 0)),
                "setpoint_max_mc": int(pid.get("setpoint_max_mc", 0)),
                "dt_min_ms":       int(pid.get("dt_min_ms", 0)),
                "dt_max_ms":       int(pid.get("dt_max_ms", 0)),
            },
            "actuator_ids":   act_ids,
            "actuator_count": len(act_ids),
            "trips":          trips,
            "trip_count":     len(trips),
        })

    modifiers = []
    for m in raw.get("policy_modifiers", []):
        ctx_name = m["context"]
        ctx_id   = contexts[find_slot(contexts, ctx_name, "context")]["id"]
        stages = 0
        for st in m["stages"]:
            stages |= pop_enum(st, MOD_STAGE_MAP)
        curve = []
        for cp in m.get("curve", []):
            curve.append({
                "x":      int(cp["x"]),
                "value0": int(cp["value0"]),
                "value1": int(cp["value1"]),
            })
        modifiers.append({
            "name":        m["name"],
            "context_id":  ctx_id,
            "stages":      stages,
            "curve":       curve,
            "curve_count": len(curve),
            "fail_safe":   pop_enum(m["fail_safe"], FAILSAFE_MAP),
        })

    # Faults (a single object with up to four detector defaults).
    f_raw = raw.get("faults", {})
    def mk_det(d):
        if d is None: return None
        return {
            "enabled":               1 if d.get("enabled", False) else 0,
            "severity":              pop_enum(d["severity"], FAULT_SEVERITY_MAP) if "severity" in d else 0,
            "action":                pop_enum(d["action"], FAULT_ACTION_MAP) if "action" in d else 0,
            "persist_ticks":         int(d.get("persist_ticks", 0)),
            "recovery_ticks":        int(d.get("recovery_ticks", 0)),
            "threshold0":            int(d.get("threshold0", 0)),
            "threshold1":            int(d.get("threshold1", 0)),
            "correlated_context_id": int(d.get("correlated_context", 0))
                                       if isinstance(d.get("correlated_context"), int)
                                       else (contexts[find_slot(contexts, d["correlated_context"], "context")]["id"]
                                             if "correlated_context" in d else 0),
        }
    faults = {
        "stall":         mk_det(f_raw.get("stall")),
        "stuck_sensor":  mk_det(f_raw.get("stuck_sensor")),
        "runaway":       mk_det(f_raw.get("runaway")),
        "stale_context": mk_det(f_raw.get("stale_context")),
    }

    # Telemetry: expand wildcards now that counts are known.
    cfg_view = {"sensors": sensors, "actuators": actuators, "zones": zones,
                "contexts": contexts, "modifiers": modifiers}
    t_raw = raw.get("telemetry", {})
    enabled_ids = []
    for sel in t_raw.get("signals", []):
        enabled_ids.extend(expand_signal(sel, cfg_view))
    if len(enabled_ids) > MAX_TELEMETRY_SIGNALS:
        raise ConfigError("too many telemetry signals")
    telemetry = {
        "enable":              1 if t_raw.get("enable", False) else 0,
        "period_ticks":        int(t_raw.get("period_ticks", 0)),
        "enabled_signal_ids":  enabled_ids,
    }

    return {
        "config_version":     int(raw["config_version"]),
        "control_period_ms":  int(raw["control_period_ms"]),
        "sensors":            sensors,
        "contexts":           contexts,
        "actuators":          actuators,
        "zones":              zones,
        "modifiers":          modifiers,
        "faults":             faults,
        "telemetry":          telemetry,
    }


# === Emitter =========================================================

def emit_name(s):
    """Emit a quoted C string literal that fits in NAME_MAX-1 chars
    plus a NUL byte.  Caller must pre-check length."""
    if len(s) >= NAME_MAX:
        raise ConfigError(f"name '{s}' >= NAME_MAX ({NAME_MAX})")
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def emit_state_pwm(arr):
    return "{ " + ", ".join(str(int(x)) for x in arr) + " }"


def emit_sensor(s, indent="        "):
    return (f"{{ .id = {s['id']}, .name = {emit_name(s['name'])},"
            f" .iir_alpha_q16 = {s['iir_alpha_q16']},"
            f" .max_staleness_ms = {s['max_staleness_ms']} }}")


def emit_context(c):
    return (f"{{ .id = {c['id']}, .name = {emit_name(c['name'])},"
            f" .unit = {c['unit']}, .iir_alpha_q16 = {c['iir_alpha_q16']},"
            f" .timeout_ms = {c['timeout_ms']}, .fail_safe = {c['fail_safe']} }}")


def emit_actuator(a):
    return (f"{{ .id = {a['id']}, .name = {emit_name(a['name'])},"
            f" .pwm_min = {a['pwm_min']}, .pwm_max = {a['pwm_max']},"
            f" .slew_per_tick = {a['slew_per_tick']},"
            f" .spinup_pwm = {a['spinup_pwm']}, .spinup_ms = {a['spinup_ms']},"
            f" .state_pwm = {emit_state_pwm(a['state_pwm'])} }}")


def emit_pid(p):
    return ("{ "
            f".kp_q16 = {p['kp_q16']}, .ki_q16 = {p['ki_q16']}, "
            f".kd_q16 = {p['kd_q16']}, .setpoint_mc = {p['setpoint_mc']}, "
            f".kp_min_q16 = {p['kp_min_q16']}, .kp_max_q16 = {p['kp_max_q16']}, "
            f".ki_min_q16 = {p['ki_min_q16']}, .ki_max_q16 = {p['ki_max_q16']}, "
            f".kd_min_q16 = {p['kd_min_q16']}, .kd_max_q16 = {p['kd_max_q16']}, "
            f".setpoint_min_mc = {p['setpoint_min_mc']}, "
            f".setpoint_max_mc = {p['setpoint_max_mc']}, "
            f".dt_min_ms = {p['dt_min_ms']}, .dt_max_ms = {p['dt_max_ms']} "
            "}")


def emit_zone(z):
    sens = "{ " + ", ".join(str(s) for s in z["sensor_ids"]) + " }"
    weights = "{ " + ", ".join(str(s) for s in z["sensor_weights_q16"]) + " }" \
              if z["sensor_weights_q16"] else "{0}"
    acts = "{ " + ", ".join(str(a) for a in z["actuator_ids"]) + " }"
    trips_inner = ", ".join(
        f"{{ .temp_mc = {t['temp_mc']}, .hyst_mc = {t['hyst_mc']},"
        f" .severity = {t['severity']}, .cooling_state = {t['cooling_state']} }}"
        for t in z["trips"]
    )
    trips_lit = "{ " + trips_inner + " }" if trips_inner else "{0}"
    return ("    {\n"
            f"        .name = {emit_name(z['name'])},\n"
            f"        .sensor_ids = {sens},\n"
            f"        .sensor_weights_q16 = {weights},\n"
            f"        .sensor_count = {z['sensor_count']},\n"
            f"        .aggregation = {z['aggregation']},\n"
            f"        .fallback_temp_mc = {z['fallback_temp_mc']},\n"
            f"        .governor = {z['governor']},\n"
            f"        .pid = {emit_pid(z['pid'])},\n"
            f"        .actuator_ids = {acts},\n"
            f"        .actuator_count = {z['actuator_count']},\n"
            f"        .trips = {trips_lit},\n"
            f"        .trip_count = {z['trip_count']},\n"
            "    }")


def emit_modifier(m):
    curve_inner = ", ".join(
        f"{{ .x = {c['x']}, .value0 = {c['value0']}, .value1 = {c['value1']} }}"
        for c in m["curve"]
    )
    curve_lit = "{ " + curve_inner + " }" if curve_inner else "{0}"
    return ("    {\n"
            f"        .name = {emit_name(m['name'])},\n"
            f"        .context_id = {m['context_id']},\n"
            f"        .stages = {m['stages']},\n"
            f"        .curve = {curve_lit},\n"
            f"        .curve_count = {m['curve_count']},\n"
            f"        .fail_safe = {m['fail_safe']},\n"
            "    }")


def emit_detector(d):
    if d is None:
        return "{0}"
    return ("{ "
            f".enabled = {d['enabled']}, .severity = {d['severity']}, "
            f".action = {d['action']}, .persist_ticks = {d['persist_ticks']}, "
            f".recovery_ticks = {d['recovery_ticks']}, "
            f".threshold0 = {d['threshold0']}, .threshold1 = {d['threshold1']}, "
            f".correlated_context_id = {d['correlated_context_id']} "
            "}")


def emit_telemetry(t):
    ids = ", ".join(f"0x{i:04x}" for i in t["enabled_signal_ids"])
    ids_lit = "{ " + ids + " }" if ids else "{0}"
    return ("    {\n"
            f"        .enable = {t['enable']},\n"
            f"        .period_ticks = {t['period_ticks']},\n"
            f"        .enabled_signal_ids = {ids_lit},\n"
            f"        .enabled_signal_count = {len(t['enabled_signal_ids'])},\n"
            "    }")


def emit_static(cfg, symbol="G_THERMAL_CFG", source_path=""):
    out = []
    out.append("/* AUTOGENERATED by tools/json2static.py — DO NOT EDIT. */")
    if source_path:
        out.append(f"/* Source: {source_path} */")
    out.append("")
    out.append('#include "thermal_core.h"')
    out.append("")
    out.append(f"const thermal_config_t {symbol} = {{")
    out.append(f"    .config_version    = {cfg['config_version']},")
    out.append(f"    .control_period_ms = {cfg['control_period_ms']},")

    if cfg["sensors"]:
        out.append("    .sensors = {")
        for s in cfg["sensors"]:
            out.append("        " + emit_sensor(s) + ",")
        out.append("    },")
    out.append(f"    .sensor_count = {len(cfg['sensors'])},")

    if cfg["contexts"]:
        out.append("    .contexts = {")
        for c in cfg["contexts"]:
            out.append("        " + emit_context(c) + ",")
        out.append("    },")
    out.append(f"    .context_count = {len(cfg['contexts'])},")

    if cfg["actuators"]:
        out.append("    .actuators = {")
        for a in cfg["actuators"]:
            out.append("        " + emit_actuator(a) + ",")
        out.append("    },")
    out.append(f"    .actuator_count = {len(cfg['actuators'])},")

    if cfg["zones"]:
        out.append("    .zones = {")
        for z in cfg["zones"]:
            out.append(emit_zone(z) + ",")
        out.append("    },")
    out.append(f"    .zone_count = {len(cfg['zones'])},")

    if cfg["modifiers"]:
        out.append("    .modifiers = {")
        for m in cfg["modifiers"]:
            out.append(emit_modifier(m) + ",")
        out.append("    },")
    out.append(f"    .modifier_count = {len(cfg['modifiers'])},")

    out.append("    .faults = {")
    out.append(f"        .stall_defaults         = {emit_detector(cfg['faults']['stall'])},")
    out.append(f"        .stuck_sensor_defaults  = {emit_detector(cfg['faults']['stuck_sensor'])},")
    out.append(f"        .runaway_defaults       = {emit_detector(cfg['faults']['runaway'])},")
    out.append(f"        .stale_context_defaults = {emit_detector(cfg['faults']['stale_context'])},")
    out.append("    },")

    out.append("    .telemetry =")
    out.append(emit_telemetry(cfg["telemetry"]) + ",")

    out.append("};")
    out.append("")
    return "\n".join(out)


# === CLI =============================================================

def main(argv=None):
    p = argparse.ArgumentParser(description="JSON to const thermal_config_t.")
    p.add_argument("config", help="Input JSON config path")
    p.add_argument("-o", "--output", help="Output C file (default: stdout)")
    p.add_argument("--symbol", default="G_THERMAL_CFG",
                   help="Name of the emitted const thermal_config_t symbol")
    args = p.parse_args(argv)

    src = Path(args.config)
    raw = json.loads(src.read_text())
    cfg = normalise(raw)
    text = emit_static(cfg, symbol=args.symbol, source_path=str(src))

    if args.output:
        Path(args.output).write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
