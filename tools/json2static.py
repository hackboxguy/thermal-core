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
MAX_FAN_HEALTH_POINTS   = 8
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

# Fan-health baseline provenance (mirror core/thermal_fan_health.h).
FAN_BASELINE_SRC_MAP = {
    "field":   0,
    "factory": 1,
    "model":   2,
}

# === Telemetry signal IDs (mirror core/thermal_signals.h) ===========

TSIG_ZONE_BASE       = 0x0100
TSIG_ACTUATOR_BASE   = 0x0200
TSIG_PID_BASE        = 0x0300
TSIG_CONTEXT_BASE    = 0x0400
TSIG_MODIFIER_BASE   = 0x0500
TSIG_FAULT_BASE      = 0x0600
TSIG_FAN_HEALTH_BASE = 0x0800
TSIG_PLATFORM_BASE   = 0x0900

def tsig_zone_temp(slot):              return TSIG_ZONE_BASE + 0x00 + slot
def tsig_zone_cooling_state(slot):     return TSIG_ZONE_BASE + 0x10 + slot
def tsig_zone_aggregation_valid(slot): return TSIG_ZONE_BASE + 0x40 + slot
def tsig_actuator_duty(slot):          return TSIG_ACTUATOR_BASE + 0x00 + slot
def tsig_actuator_rpm(slot):           return TSIG_ACTUATOR_BASE + 0x10 + slot
def tsig_pid(zone, term):              return TSIG_PID_BASE + zone * 4 + term
def tsig_context_value(slot):          return TSIG_CONTEXT_BASE + 0x00 + slot
def tsig_modifier_pwm_cap(slot):       return TSIG_MODIFIER_BASE + 0x00 + slot
def tsig_fault_stall(slot):            return TSIG_FAULT_BASE + 0x00 + slot
def tsig_fault_stuck_sensor(slot):     return TSIG_FAULT_BASE + 0x10 + slot
def tsig_fault_runaway(slot):          return TSIG_FAULT_BASE + 0x20 + slot
def tsig_fault_stale_context(slot):    return TSIG_FAULT_BASE + 0x30 + slot
def tsig_fan_health_delta(slot):           return TSIG_FAN_HEALTH_BASE + 0x00 + slot
def tsig_fan_health_severity(slot):        return TSIG_FAN_HEALTH_BASE + 0x10 + slot
def tsig_fan_health_baseline_source(slot): return TSIG_FAN_HEALTH_BASE + 0x20 + slot
def tsig_fan_health_confidence(slot):      return TSIG_FAN_HEALTH_BASE + 0x30 + slot

# === Errors ==========================================================

class ConfigError(Exception):
    pass


# === Strict allowlists =================================================
# Mirror the C loader's per-object allowed-key sets.  Unknown keys at
# any level are rejected; this matches platform/linux/config_jsmn.c
# strictness and keeps the static path from silently dropping a typo
# the dynamic loader would have caught.

TOP_LEVEL_ALLOWED = {
    "config_version", "control_period_ms",
    "sensors", "context_signals", "actuators", "zones",
    "policy_modifiers", "fault_detection", "telemetry", "control",
    "mcu_pinmap",                         # MCU-target-only, optional
}

# `mcu_pinmap` is the MCU-target-agnostic JSON key (CH32V003,
# RH850, etc. would reuse it).  The emitted C struct stays
# platform-scoped (esp32_pinmap_t / G_ESP32_PINMAP), since each
# MCU port owns its own pinmap header under platform/<target>/.
MCU_PINMAP_TOP_ALLOWED      = {"sensors", "actuators"}
MCU_PINMAP_SENSOR_ALLOWED   = {"name", "onewire_gpio"}
MCU_PINMAP_ACTUATOR_ALLOWED = {"name", "pwm_gpio", "tach_gpio",
                                "pwm_freq_hz"}

SENSOR_ALLOWED = {
    "id", "name", "iir_alpha_q16", "max_staleness_ms",
    "source",                           # platform-only
}

CONTEXT_ALLOWED = {
    "id", "name", "unit", "iir_alpha_q16", "timeout_ms", "fail_safe",
    "source", "fail_safe_value",        # platform-only
}

ACTUATOR_ALLOWED = {
    "id", "name", "pwm_min", "pwm_max", "slew_per_tick",
    "spinup_pwm", "spinup_ms", "min_on_ticks", "min_off_ticks", "state_pwm",
    "pwm", "tach", "pwm_freq_hz", "tach_pulses_per_rev",   # platform-only
    "fan_health",                       # Stage 17, --enable-fan-health only
}

FAN_HEALTH_ALLOWED = {
    "enable", "fan_model", "baseline_source", "baseline",
    "stable_pwm_ticks", "stable_pwm_tolerance",
    "stable_rpm_ticks", "stable_rpm_tolerance_pct",
    "min_points_observed", "severity_pct",
}

SEVERITY_PCT_ALLOWED = {"aging", "degraded", "failing"}

ZONE_ALLOWED = {
    "name", "sensors", "sensor_weights_q16", "aggregation",
    "fallback_temp_mc", "governor", "pid",
    "actuators", "trips",
}

TRIP_ALLOWED = {
    "temp_mc", "hyst_mc", "severity", "cooling_state",
}

PID_ALLOWED = {
    "kp_q16", "ki_q16", "kd_q16", "setpoint_mc",
    "kp_min_q16", "kp_max_q16", "ki_min_q16", "ki_max_q16",
    "kd_min_q16", "kd_max_q16", "setpoint_min_mc", "setpoint_max_mc",
    "dt_min_ms", "dt_max_ms",
}

MODIFIER_ALLOWED = {
    "name", "context", "stages", "curve", "fail_safe",
}

CURVE_POINT_ALLOWED = {"x", "value0", "value1"}

FAULT_DETECTION_ALLOWED = {"stall", "stuck_sensor", "runaway", "stale_context"}

DETECTOR_COMMON = {"enabled", "severity", "action", "persist_ticks", "recovery_ticks"}
DETECTOR_ALLOWED = {
    "stall":         DETECTOR_COMMON | {"stall_rpm", "stall_pwm_threshold"},
    "stuck_sensor":  DETECTOR_COMMON | {"delta_mc", "window_ticks", "correlated_context"},
    "runaway":       DETECTOR_COMMON | {"rise_mc_threshold", "cooling_pwm_threshold"},
    "stale_context": DETECTOR_COMMON,
}

TELEMETRY_ALLOWED = {"enable", "period_ticks", "signals", "transport"}

CONTROL_ALLOWED = {"listen", "enable"}


def reject_unknown(obj, allowed, where):
    if not isinstance(obj, dict):
        raise ConfigError(f"{where}: expected object, got {type(obj).__name__}")
    extras = set(obj.keys()) - allowed
    if extras:
        raise ConfigError(f"{where}: unknown key(s) {sorted(extras)}")


def require_int(v, where):
    """Strict integer: rejects bool, float, and numeric strings. The
    Linux loader (config_jsmn.c) rejects all three for these fields, so
    the static path must not silently coerce them (e.g. 64.9 -> 64)."""
    if isinstance(v, bool) or not isinstance(v, int):
        raise ConfigError(f"{where}: expected an integer, got "
                           f"{type(v).__name__}")
    return v


def require_bool(v, where):
    """Accept a JSON boolean or the integers 0/1 -- mirroring the Linux
    loader's tok_parse_bool, which takes both. Rejects floats, strings,
    and ints other than 0/1."""
    if isinstance(v, bool):
        return v
    if isinstance(v, int) and v in (0, 1):
        return bool(v)
    raise ConfigError(f"{where}: expected a boolean (true/false or 0/1)")


# Zeroed fan-health block -- emitted for an actuator with no (or a
# disabled) fan_health block. Matches config_jsmn's parse_fan_health,
# which memsets the block to zero when it is absent or disabled.
FAN_HEALTH_ZEROED = {
    "enable": 0, "baseline_source": 0, "baseline": [], "baseline_count": 0,
    "stable_pwm_ticks": 0, "stable_pwm_tolerance": 0,
    "stable_rpm_ticks": 0, "stable_rpm_tolerance_pct": 0,
    "min_points_observed": 0,
    "aging_pct": 0, "degraded_pct": 0, "failing_pct": 0,
}


def normalise_fan_health(fh_raw, actuator_raw, idx, has_mcu_tach=False):
    """Parse + validate a per-actuator fan_health block (Stage 17, PRD
    Appendix C). Returns a normalised dict; a zeroed block when the
    section is absent or disabled. Raises ConfigError on any malformed
    or physically implausible baseline -- mirrors config_jsmn.c
    parse_fan_health plus thermal_core_validate_config."""
    if fh_raw is None:
        return dict(FAN_HEALTH_ZEROED)
    where = f"actuators[{idx}].fan_health"
    reject_unknown(fh_raw, FAN_HEALTH_ALLOWED, where)
    if "enable" in fh_raw:
        require_bool(fh_raw["enable"], f"{where}.enable")
    if not fh_raw.get("enable", False):
        return dict(FAN_HEALTH_ZEROED)

    for key in ("baseline_source", "baseline", "stable_pwm_ticks",
                "stable_pwm_tolerance", "stable_rpm_ticks",
                "stable_rpm_tolerance_pct", "min_points_observed",
                "severity_pct"):
        if key not in fh_raw:
            raise ConfigError(f"{where}: missing required key '{key}'")
    # The actuator's tach lives either as a Linux-style file path
    # (`actuator.tach`) or, on MCU targets (Stage 20 onward), as a
    # `tach_gpio` in `mcu_pinmap.actuators[]`. Either counts.
    if "tach" not in actuator_raw and not has_mcu_tach:
        raise ConfigError(f"{where}: enabled but actuator has no tach source")

    src_name = fh_raw["baseline_source"]
    if src_name not in FAN_BASELINE_SRC_MAP:
        raise ConfigError(f"{where}.baseline_source: unknown value '{src_name}'")

    base_raw = fh_raw["baseline"]
    if not isinstance(base_raw, list):
        raise ConfigError(f"{where}.baseline: expected array")
    if not 2 <= len(base_raw) <= MAX_FAN_HEALTH_POINTS:
        raise ConfigError(
            f"{where}.baseline: needs 2..{MAX_FAN_HEALTH_POINTS} points")
    baseline = []
    for j, pt in enumerate(base_raw):
        if not isinstance(pt, list) or len(pt) != 2:
            raise ConfigError(f"{where}.baseline[{j}]: expected [pwm, rpm] pair")
        pwm = require_int(pt[0], f"{where}.baseline[{j}].pwm")
        rpm = require_int(pt[1], f"{where}.baseline[{j}].rpm")
        if not 0 <= pwm <= 255:
            raise ConfigError(f"{where}.baseline[{j}]: pwm out of [0, 255]")
        if not 1 <= rpm <= 65535:
            raise ConfigError(f"{where}.baseline[{j}]: rpm out of [1, 65535]")
        if j > 0:
            if pwm <= baseline[-1][0]:
                raise ConfigError(
                    f"{where}.baseline[{j}]: pwm not strictly ascending")
            if rpm < baseline[-1][1]:
                raise ConfigError(
                    f"{where}.baseline[{j}]: rpm not monotonic non-decreasing")
        baseline.append([pwm, rpm])

    # PRD C.4: the baseline must reach the actuator's pwm_max so
    # high-duty operation is never scored against a clamped expected RPM.
    pwm_max = require_int(actuator_raw["pwm_max"], f"actuators[{idx}].pwm_max")
    if baseline[-1][0] < pwm_max:
        raise ConfigError(
            f"{where}.baseline: highest PWM point {baseline[-1][0]} is "
            f"below the actuator's pwm_max {pwm_max}")

    sev = fh_raw["severity_pct"]
    reject_unknown(sev, SEVERITY_PCT_ALLOWED, f"{where}.severity_pct")
    for key in ("aging", "degraded", "failing"):
        if key not in sev:
            raise ConfigError(f"{where}.severity_pct: missing '{key}'")
    aging   = require_int(sev["aging"],    f"{where}.severity_pct.aging")
    degraded = require_int(sev["degraded"], f"{where}.severity_pct.degraded")
    failing = require_int(sev["failing"],  f"{where}.severity_pct.failing")
    for nm, v in (("aging", aging), ("degraded", degraded), ("failing", failing)):
        if not -100 <= v <= -1:
            raise ConfigError(f"{where}.severity_pct.{nm}: out of [-100, -1]")
    if not 0 > aging > degraded > failing:
        raise ConfigError(
            f"{where}.severity_pct: must satisfy 0 > aging > degraded > failing")

    spt = require_int(fh_raw["stable_pwm_ticks"], f"{where}.stable_pwm_ticks")
    if not 1 <= spt <= 65535:
        raise ConfigError(f"{where}.stable_pwm_ticks: out of [1, 65535]")
    sptol = require_int(fh_raw["stable_pwm_tolerance"],
                        f"{where}.stable_pwm_tolerance")
    if not 0 <= sptol <= 255:
        raise ConfigError(f"{where}.stable_pwm_tolerance: out of [0, 255]")
    srt = require_int(fh_raw["stable_rpm_ticks"], f"{where}.stable_rpm_ticks")
    if not 1 <= srt <= 65535:
        raise ConfigError(f"{where}.stable_rpm_ticks: out of [1, 65535]")
    srtol = require_int(fh_raw["stable_rpm_tolerance_pct"],
                        f"{where}.stable_rpm_tolerance_pct")
    if not 0 <= srtol <= 100:
        raise ConfigError(f"{where}.stable_rpm_tolerance_pct: out of [0, 100]")
    mpo = require_int(fh_raw["min_points_observed"],
                      f"{where}.min_points_observed")
    if not 1 <= mpo <= len(baseline):
        raise ConfigError(
            f"{where}.min_points_observed: out of [1, baseline point count]")

    if "fan_model" in fh_raw and not isinstance(fh_raw["fan_model"], str):
        raise ConfigError(f"{where}.fan_model: expected string")

    return {
        "enable": 1,
        "baseline_source": FAN_BASELINE_SRC_MAP[src_name],
        "baseline": baseline,
        "baseline_count": len(baseline),
        "stable_pwm_ticks": spt,
        "stable_pwm_tolerance": sptol,
        "stable_rpm_ticks": srt,
        "stable_rpm_tolerance_pct": srtol,
        "min_points_observed": mpo,
        "aging_pct": aging,
        "degraded_pct": degraded,
        "failing_pct": failing,
    }


# === Loader (mirrors config_jsmn.c semantics) ========================

def expand_signal(sel: str, cfg, enable_fan_health=False) -> list[int]:
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
    if sel == "zone_aggregation_valid_*":
        if zone_count == 0: raise ConfigError("wildcard expanded to nothing")
        return [tsig_zone_aggregation_valid(s) for s in range(zone_count)]
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
    if sel == "fan_health_*":
        # When --enable-fan-health is off, expand to nothing -- the
        # same MCU config can list fan_health_* signals and still be
        # consumed by a no-fan-health build (Stage 20).
        if not enable_fan_health:
            return []
        if actuator_count == 0: raise ConfigError("wildcard expanded to nothing")
        out = []
        for s in range(actuator_count):
            out += [tsig_fan_health_delta(s), tsig_fan_health_severity(s),
                    tsig_fan_health_baseline_source(s),
                    tsig_fan_health_confidence(s)]
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
    if sel.startswith("zone_aggregation_valid_"):
        name = sel[len("zone_aggregation_valid_"):]
        for i, z in enumerate(cfg["zones"]):
            if z["name"] == name: return [tsig_zone_aggregation_valid(i)]
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


def normalise(raw: dict, enable_fan_health: bool = False) -> dict:
    """Pull CORE-owned fields out of `raw` (the JSON document) into a
    normalised dict that mirrors thermal_config_t.  Platform-only
    fields are dropped (json2static is for CORE only; the daemon's
    runtime cfg lives elsewhere).

    `enable_fan_health` mirrors the THERMALCORE_ENABLE_FAN_HEALTH
    compile gate: when set, per-actuator `fan_health` blocks are
    parsed and emitted; when unset, a `fan_health` block is a hard
    error (the feature is not compiled in)."""
    reject_unknown(raw, TOP_LEVEL_ALLOWED, "$")
    if "config_version" not in raw:
        raise ConfigError("missing config_version")
    if "control_period_ms" not in raw:
        raise ConfigError("missing control_period_ms")

    def pop_enum(name, m, where):
        v = m.get(name)
        if v is None: raise ConfigError(f"{where}: unknown enum '{name}'")
        return v

    sensors = []
    for i, s in enumerate(raw.get("sensors", [])):
        reject_unknown(s, SENSOR_ALLOWED, f"sensors[{i}]")
        sensors.append({
            "id":               int(s["id"]),
            "name":             s["name"],
            "iir_alpha_q16":    int(s["iir_alpha_q16"]),
            "max_staleness_ms": int(s["max_staleness_ms"]),
        })

    contexts = []
    for i, c in enumerate(raw.get("context_signals", [])):
        reject_unknown(c, CONTEXT_ALLOWED, f"context_signals[{i}]")
        contexts.append({
            "id":            int(c["id"]),
            "name":          c["name"],
            "unit":          pop_enum(c["unit"], CONTEXT_UNIT_MAP, f"context_signals[{i}].unit"),
            "iir_alpha_q16": int(c["iir_alpha_q16"]),
            "timeout_ms":    int(c["timeout_ms"]),
            "fail_safe":     pop_enum(c["fail_safe"], FAILSAFE_MAP, f"context_signals[{i}].fail_safe"),
        })

    actuators = []
    state_pwm_lens = []  # JSON-explicit length per actuator slot (for #6)
    fan_health = []      # one entry per actuator (Stage 17); zeroed if absent
    for i, a in enumerate(raw.get("actuators", [])):
        reject_unknown(a, ACTUATOR_ALLOWED, f"actuators[{i}]")
        sp = a["state_pwm"]
        if len(sp) > MAX_COOLING_STATES:
            raise ConfigError(f"actuators[{i}].state_pwm: too many cooling states")
        state_pwm_lens.append(len(sp))
        actuators.append({
            "id":             int(a["id"]),
            "name":           a["name"],
            "pwm_min":        int(a["pwm_min"]),
            "pwm_max":        int(a["pwm_max"]),
            "slew_per_tick":  int(a.get("slew_per_tick", 0)),
            "spinup_pwm":     int(a.get("spinup_pwm", 0)),
            "spinup_ms":      int(a.get("spinup_ms", 0)),
            "min_on_ticks":   int(a.get("min_on_ticks", 0)),
            "min_off_ticks":  int(a.get("min_off_ticks", 0)),
            "state_pwm":      [int(x) for x in sp] + [0] * (MAX_COOLING_STATES - len(sp)),
        })
        # When --enable-fan-health is off, any fan_health block in
        # the JSON is silently ignored: the same MCU config supports
        # both the default no-telemetry build and the telemetry
        # build (which adds the detector). config_jsmn already
        # behaves this way on the Linux side.
        if enable_fan_health:
            has_mcu_tach = any(
                mca.get("name") == a["name"] and "tach_gpio" in mca
                for mca in raw.get("mcu_pinmap", {}).get("actuators", []))
            fan_health.append(normalise_fan_health(
                a.get("fan_health"), a, i, has_mcu_tach))

    def find_slot(arr, name, kind, where):
        for i, e in enumerate(arr):
            if e["name"] == name:
                return i
        raise ConfigError(f"{where}: unknown {kind} '{name}'")

    zones = []
    for zi, z in enumerate(raw.get("zones", [])):
        reject_unknown(z, ZONE_ALLOWED, f"zones[{zi}]")
        sens_names = z.get("sensors", [])
        sens_ids   = [sensors[find_slot(sensors, n, "sensor", f"zones[{zi}].sensors[{j}]")]["id"]
                      for j, n in enumerate(sens_names)]
        act_names  = z.get("actuators", [])
        act_slots  = [find_slot(actuators, n, "actuator", f"zones[{zi}].actuators[{j}]")
                      for j, n in enumerate(act_names)]
        act_ids    = [actuators[s]["id"] for s in act_slots]
        raw_weights = z.get("sensor_weights_q16", [])
        weights     = [int(x) for x in raw_weights]
        trips = []
        for ti, t in enumerate(z.get("trips", [])):
            reject_unknown(t, TRIP_ALLOWED, f"zones[{zi}].trips[{ti}]")
            trips.append({
                "temp_mc":       int(t["temp_mc"]),
                "hyst_mc":       int(t["hyst_mc"]),
                "severity":      pop_enum(t["severity"], TRIP_SEVERITY_MAP,
                                          f"zones[{zi}].trips[{ti}].severity"),
                "cooling_state": int(t["cooling_state"]),
            })
        pid = z.get("pid", {})
        reject_unknown(pid, PID_ALLOWED, f"zones[{zi}].pid")
        aggregation = pop_enum(z["aggregation"], AGGREGATION_MAP, f"zones[{zi}].aggregation")

        # PRD §5.3 line 800: weighted aggregation requires len(weights) == len(sensors).
        if aggregation == AGGREGATION_MAP["weighted"]:
            if len(weights) != len(sens_ids):
                raise ConfigError(
                    f"zones[{zi}]: weighted aggregation needs sensor_weights_q16 "
                    f"length {len(sens_ids)}, got {len(weights)}"
                )

        # PRD §5.3 line 805: trip cooling_state must fall inside the
        # JSON-explicit state_pwm length of every referenced actuator.
        for ti, t in enumerate(trips):
            cs = t["cooling_state"]
            for j, slot in enumerate(act_slots):
                if cs >= state_pwm_lens[slot]:
                    raise ConfigError(
                        f"zones[{zi}].trips[{ti}]: cooling_state {cs} past "
                        f"actuator '{actuators[slot]['name']}' state_pwm "
                        f"length {state_pwm_lens[slot]}"
                    )

        zones.append({
            "name":               z["name"],
            "sensor_ids":         sens_ids,
            "sensor_weights_q16": weights,
            "sensor_count":       len(sens_ids),
            "aggregation":        aggregation,
            "fallback_temp_mc":   int(z["fallback_temp_mc"]),
            "governor":           pop_enum(z["governor"], GOVERNOR_MAP, f"zones[{zi}].governor"),
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
    for mi, m in enumerate(raw.get("policy_modifiers", [])):
        reject_unknown(m, MODIFIER_ALLOWED, f"policy_modifiers[{mi}]")
        ctx_name = m["context"]
        ctx_id   = contexts[find_slot(contexts, ctx_name, "context",
                                       f"policy_modifiers[{mi}].context")]["id"]
        stages = 0
        for st in m["stages"]:
            stages |= pop_enum(st, MOD_STAGE_MAP, f"policy_modifiers[{mi}].stages")
        curve = []
        for ci, cp in enumerate(m.get("curve", [])):
            reject_unknown(cp, CURVE_POINT_ALLOWED, f"policy_modifiers[{mi}].curve[{ci}]")
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
            "fail_safe":   pop_enum(m["fail_safe"], FAILSAFE_MAP,
                                    f"policy_modifiers[{mi}].fail_safe"),
        })

    # PRD §5.1: top-level key is `fault_detection`, not `faults`.
    if "faults" in raw:
        raise ConfigError("$: unknown key 'faults' (use 'fault_detection')")
    f_raw = raw.get("fault_detection", {})
    if f_raw:
        reject_unknown(f_raw, FAULT_DETECTION_ALLOWED, "fault_detection")

    def mk_det(d, kind):
        if d is None:
            return None
        reject_unknown(d, DETECTOR_ALLOWED[kind], f"fault_detection.{kind}")
        # Descriptive thresholds -> generic threshold0/1 (PRD §5 line 611).
        if kind == "stall":
            t0 = d.get("stall_rpm",            0)
            t1 = d.get("stall_pwm_threshold",  0)
        elif kind == "stuck_sensor":
            t0 = d.get("delta_mc",     0)
            t1 = d.get("window_ticks", 0)
        elif kind == "runaway":
            t0 = d.get("rise_mc_threshold",     0)
            t1 = d.get("cooling_pwm_threshold", 0)
        else:  # stale_context
            t0 = 0
            t1 = 0

        # correlated_context: only meaningful for stuck_sensor.  null /
        # absent → 0xFFFF (advisory; PRD §4.7 line 632).
        cc_id = 0xFFFF
        if kind == "stuck_sensor":
            cc_raw = d.get("correlated_context", None)
            if cc_raw is None:
                cc_id = 0xFFFF
            elif isinstance(cc_raw, str):
                cc_id = contexts[find_slot(contexts, cc_raw, "context",
                                            f"fault_detection.stuck_sensor.correlated_context")]["id"]
            else:
                raise ConfigError(
                    f"fault_detection.stuck_sensor.correlated_context: "
                    f"expected string or null, got {type(cc_raw).__name__}"
                )

        return {
            "enabled":               1 if d.get("enabled", False) else 0,
            "severity":              pop_enum(d["severity"], FAULT_SEVERITY_MAP,
                                              f"fault_detection.{kind}.severity")
                                       if "severity" in d else 0,
            "action":                pop_enum(d["action"], FAULT_ACTION_MAP,
                                              f"fault_detection.{kind}.action")
                                       if "action" in d else 0,
            "persist_ticks":         int(d.get("persist_ticks", 0)),
            "recovery_ticks":        int(d.get("recovery_ticks", 0)),
            "threshold0":            int(t0),
            "threshold1":            int(t1),
            "correlated_context_id": int(cc_id),
        }

    faults = {
        "stall":         mk_det(f_raw.get("stall"),         "stall"),
        "stuck_sensor":  mk_det(f_raw.get("stuck_sensor"),  "stuck_sensor"),
        "runaway":       mk_det(f_raw.get("runaway"),       "runaway"),
        "stale_context": mk_det(f_raw.get("stale_context"), "stale_context"),
    }

    # Telemetry: expand wildcards now that counts are known.
    cfg_view = {"sensors": sensors, "actuators": actuators, "zones": zones,
                "contexts": contexts, "modifiers": modifiers}
    t_raw = raw.get("telemetry", {})
    if t_raw:
        reject_unknown(t_raw, TELEMETRY_ALLOWED, "telemetry")
    # `control` is platform-only; just sanity-check the allowlist if present.
    if "control" in raw and raw["control"]:
        reject_unknown(raw["control"], CONTROL_ALLOWED, "control")
    enabled_ids = []
    for sel in t_raw.get("signals", []):
        enabled_ids.extend(expand_signal(sel, cfg_view, enable_fan_health))
    if len(enabled_ids) > MAX_TELEMETRY_SIGNALS:
        raise ConfigError("too many telemetry signals")
    telemetry = {
        "enable":              1 if t_raw.get("enable", False) else 0,
        "period_ticks":        int(t_raw.get("period_ticks", 0)),
        "enabled_signal_ids":  enabled_ids,
    }

    pinmap = normalise_pinmap(raw.get("mcu_pinmap"), sensors, actuators)

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
        "mcu_pinmap":         pinmap,   # None if section omitted
        # None when --enable-fan-health is off; else one block per actuator.
        "fan_health":         fan_health if enable_fan_health else None,
    }


def normalise_pinmap(raw_pinmap, sensors, actuators):
    """Parse the optional `mcu_pinmap` JSON section into a dict
    indexed by slot (matching the order of `sensors` + `actuators`).

    Returns None if the section is absent (non-MCU builds don't
    need it).  Raises ConfigError on:
      - typos in keys (reject_unknown);
      - missing entry for a declared sensor/actuator name;
      - multi-bus 1-Wire (different `onewire_gpio` across sensor
        slots -- v1 supports one shared bus only).
    """
    if raw_pinmap is None:
        return None

    reject_unknown(raw_pinmap, MCU_PINMAP_TOP_ALLOWED, "mcu_pinmap")

    sensor_by_name = {}
    onewire_gpio_seen = None
    for i, s in enumerate(raw_pinmap.get("sensors", [])):
        where = f"mcu_pinmap.sensors[{i}]"
        reject_unknown(s, MCU_PINMAP_SENSOR_ALLOWED, where)
        for required in ("name", "onewire_gpio"):
            if required not in s:
                raise ConfigError(f"{where}: missing '{required}'")
        gpio = int(s["onewire_gpio"])
        if onewire_gpio_seen is None:
            onewire_gpio_seen = gpio
        elif onewire_gpio_seen != gpio:
            raise ConfigError(
                f"mcu_pinmap: v1 supports a single shared 1-Wire bus; "
                f"all sensors must declare the same `onewire_gpio` "
                f"(got {onewire_gpio_seen} and {gpio})")
        sensor_by_name[s["name"]] = {"onewire_gpio": gpio}

    sensors_out = []
    for s in sensors:
        name = s["name"]
        if name not in sensor_by_name:
            raise ConfigError(
                f"mcu_pinmap.sensors: missing entry for '{name}' "
                f"(declared in sensors[] but not in mcu_pinmap.sensors[])")
        sensors_out.append(sensor_by_name[name])

    actuator_by_name = {}
    for i, a in enumerate(raw_pinmap.get("actuators", [])):
        where = f"mcu_pinmap.actuators[{i}]"
        reject_unknown(a, MCU_PINMAP_ACTUATOR_ALLOWED, where)
        for required in ("name", "pwm_gpio", "tach_gpio", "pwm_freq_hz"):
            if required not in a:
                raise ConfigError(f"{where}: missing '{required}'")
        actuator_by_name[a["name"]] = {
            "pwm_gpio":    int(a["pwm_gpio"]),
            "tach_gpio":   int(a["tach_gpio"]),
            "pwm_freq_hz": int(a["pwm_freq_hz"]),
        }

    actuators_out = []
    for a in actuators:
        name = a["name"]
        if name not in actuator_by_name:
            raise ConfigError(
                f"mcu_pinmap.actuators: missing entry for '{name}' "
                f"(declared in actuators[] but not in mcu_pinmap.actuators[])")
        actuators_out.append(actuator_by_name[name])

    return {
        "sensors":   sensors_out,
        "actuators": actuators_out,
    }


# CH32V003 silicon-fixed pins (ch32fun pin id = (port_index << 4) | pin).
# TIM2 channel 1 is hardwired to PD4; EXTI line 0 to PD0.  The PWM and
# tach pins are not free choices -- a wrong pin still builds and fits
# the part but flashes a firmware that cannot drive PWM or count tach.
# The 1-Wire pin is bit-banged and therefore unconstrained.
CH32_PWM_PIN_PD4  = 52
CH32_TACH_PIN_PD0 = 48


def validate_ch32_pinmap(pinmap):
    """Reject CH32V003 pin assignments the silicon cannot honour.
    Called for `--pinmap-prefix ch32` builds so a JSON pin typo
    fails `make build-ch32` rather than flashing dead firmware."""
    if pinmap is None:
        return
    for i, a in enumerate(pinmap["actuators"]):
        if a["pwm_gpio"] != CH32_PWM_PIN_PD4:
            raise ConfigError(
                f"mcu_pinmap.actuators[{i}]: CH32V003 pwm_gpio must be "
                f"PD4 ({CH32_PWM_PIN_PD4}) -- TIM2_CH1 is hardwired "
                f"there; got {a['pwm_gpio']}")
        if a["tach_gpio"] != CH32_TACH_PIN_PD0:
            raise ConfigError(
                f"mcu_pinmap.actuators[{i}]: CH32V003 tach_gpio must be "
                f"PD0 ({CH32_TACH_PIN_PD0}) -- EXTI line 0 is hardwired "
                f"there; got {a['tach_gpio']}")


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
            f" .min_on_ticks = {a['min_on_ticks']},"
            f" .min_off_ticks = {a['min_off_ticks']},"
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


def emit_fan_health(fh):
    base_inner = ", ".join(
        f"{{ .x = {p[0]}, .value0 = {p[1]} }}" for p in fh["baseline"])
    base_lit = "{ " + base_inner + " }" if base_inner else "{0}"
    return ("{ "
            f".enable = {fh['enable']}, "
            f".baseline_source = {fh['baseline_source']}, "
            f".baseline = {base_lit}, "
            f".baseline_count = {fh['baseline_count']}, "
            f".stable_pwm_ticks = {fh['stable_pwm_ticks']}, "
            f".stable_pwm_tolerance = {fh['stable_pwm_tolerance']}, "
            f".stable_rpm_ticks = {fh['stable_rpm_ticks']}, "
            f".stable_rpm_tolerance_pct = {fh['stable_rpm_tolerance_pct']}, "
            f".min_points_observed = {fh['min_points_observed']}, "
            f".aging_pct = {fh['aging_pct']}, "
            f".degraded_pct = {fh['degraded_pct']}, "
            f".failing_pct = {fh['failing_pct']} }}")


def emit_pinmap(pinmap, prefix="esp32"):
    """Render the optional `<prefix>_pinmap_t G_<PREFIX>_PINMAP` block.
    Caller is responsible for emitting the matching `#include`.
    `prefix` selects the platform-scoped pinmap header/struct so each
    MCU port (esp32, ch32, ...) reuses the same `mcu_pinmap` JSON key."""
    out = []
    out.append(f"const {prefix}_pinmap_t G_{prefix.upper()}_PINMAP = {{")
    out.append(f"    .sensor_count = {len(pinmap['sensors'])},")
    if pinmap["sensors"]:
        out.append("    .sensors = {")
        for s in pinmap["sensors"]:
            out.append(f"        {{ .onewire_gpio = {s['onewire_gpio']} }},")
        out.append("    },")
    out.append(f"    .actuator_count = {len(pinmap['actuators'])},")
    if pinmap["actuators"]:
        out.append("    .actuators = {")
        for a in pinmap["actuators"]:
            out.append(f"        {{ .pwm_gpio = {a['pwm_gpio']},"
                       f" .tach_gpio = {a['tach_gpio']},"
                       f" .pwm_freq_hz = {a['pwm_freq_hz']} }},")
        out.append("    },")
    out.append("};")
    return "\n".join(out)


def emit_static(cfg, symbol="G_THERMAL_CFG", source_path="",
                pinmap_prefix="esp32"):
    out = []
    out.append("/* AUTOGENERATED by tools/json2static.py — DO NOT EDIT. */")
    if source_path:
        out.append(f"/* Source: {source_path} */")
    out.append("")
    out.append('#include "thermal_core.h"')
    if cfg.get("mcu_pinmap") is not None:
        out.append(f'#include "{pinmap_prefix}_pinmap.h"')
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

    if cfg.get("fan_health") is not None:
        out.append("    .fan_health = {")
        for fh in cfg["fan_health"]:
            out.append("        " + emit_fan_health(fh) + ",")
        out.append("    },")

    out.append("};")
    out.append("")
    if cfg.get("mcu_pinmap") is not None:
        out.append(emit_pinmap(cfg["mcu_pinmap"], pinmap_prefix))
        out.append("")
    return "\n".join(out)


# === CLI =============================================================

def main(argv=None):
    p = argparse.ArgumentParser(description="JSON to const thermal_config_t.")
    p.add_argument("config", help="Input JSON config path")
    p.add_argument("-o", "--output", help="Output C file (default: stdout)")
    p.add_argument("--symbol", default="G_THERMAL_CFG",
                   help="Name of the emitted const thermal_config_t symbol")
    p.add_argument("--pinmap-prefix", default="esp32",
                   help="Platform prefix for the emitted mcu_pinmap struct, "
                        "symbol, and header (esp32 -> esp32_pinmap_t "
                        "G_ESP32_PINMAP / esp32_pinmap.h)")
    p.add_argument("--enable-fan-health", action="store_true",
                   help="Mirror the THERMALCORE_ENABLE_FAN_HEALTH compile "
                        "gate: parse and emit per-actuator fan_health blocks "
                        "(Stage 17, PRD Appendix C)")
    args = p.parse_args(argv)

    src = Path(args.config)
    raw = json.loads(src.read_text())
    cfg = normalise(raw, enable_fan_health=args.enable_fan_health)
    if args.pinmap_prefix == "ch32":
        validate_ch32_pinmap(cfg.get("mcu_pinmap"))
    text = emit_static(cfg, symbol=args.symbol, source_path=str(src),
                       pinmap_prefix=args.pinmap_prefix)

    if args.output:
        Path(args.output).write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
