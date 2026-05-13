"""tools/thermalcore-scenario/scenario.py — .scn parser + assertion grammar.

Stage 12 12c grammar (extends 12b's idle-only subset):

    # comments
    config <path>                                  # daemon JSON (optional)

    plant duration_ms     <int>                    # global
    plant ambient_mc      <int>                    # zone-0 shorthand (12b)
    plant initial_temp_mc <int>                    # zone-0 shorthand (12b)

    plant zone_count      <int>                    # multi-zone
    plant zone <z> <field> <int>                   # per-zone (any field)
    plant zone <z> coupling <neighbor> <q16>       # special form (2 args)

    <ms> freeze_input          <sensor_name>   <int_mc>
    <ms> unfreeze_input        <sensor_name>
    <ms> freeze_tach           <actuator_name> <rpm>
    <ms> unfreeze_tach         <actuator_name>
    <ms> set_plant_load_w_q16  <zone_idx> <q16>
    <ms> set_plant_fan_max_q16 <zone_idx> <q16>

    assert max <kind> <slot> <op> <int> between <t0_ms> <t1_ms>
    assert min <kind> <slot> <op> <int> between <t0_ms> <t1_ms>
    assert eventually <kind> <slot> <op> <int> within <ms>
    assert within <ms> fault_active <fault_kind> <target_slot>
    assert no_faults
    assert no_faults except <fault_kind>

Where <kind>       in {actuator_pwm, zone_temp}
      <op>         in {<=, <, >=, >, ==, !=}
      <fault_kind> in {stall, stuck_sensor, runaway, stale_context}

Unknown directives raise ScenarioParseError immediately.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

# === Signal-ID mapping ===============================================
# Mirrors core/thermal_signals.h.

TSIG_ZONE_BASE         = 0x0100
TSIG_ZONE_SUB_TEMP     = 0x00
TSIG_ACTUATOR_BASE     = 0x0200
TSIG_ACTUATOR_SUB_DUTY = 0x00

# Fault event codes from core/thermal_events.h.
# CORRECTED in 12c: 12b shipped with {0x4000, ...} -- spurious-true
# `no_faults` for every fault scenario.
TEVENT_FAULT_ENTER      = 0x1000
TEVENT_FAULT_RECOVERING = 0x1001
TEVENT_FAULT_CLEAR      = 0x1002

FAULT_EVENT_CODES = frozenset({
    TEVENT_FAULT_ENTER,
    TEVENT_FAULT_RECOVERING,
    TEVENT_FAULT_CLEAR,
})

# thermal_fault_type_t values from core/thermal_fault.h, surfaced via
# the event payload's a1 field.
FAULT_TYPES: dict[str, int] = {
    "stall":          1,    # THERMAL_FAULT_TYPE_STALL
    "stuck_sensor":   2,    # THERMAL_FAULT_TYPE_STUCK_SENSOR
    "runaway":        3,    # THERMAL_FAULT_TYPE_RUNAWAY
    "stale_context":  4,    # THERMAL_FAULT_TYPE_STALE_CONTEXT
}


def signal_id_for(signal_kind: str, slot: int) -> int:
    if signal_kind == "actuator_pwm":
        return TSIG_ACTUATOR_BASE + TSIG_ACTUATOR_SUB_DUTY + slot
    if signal_kind == "zone_temp":
        return TSIG_ZONE_BASE + TSIG_ZONE_SUB_TEMP + slot
    raise ScenarioParseError(
        f"unknown signal kind '{signal_kind}'; "
        f"supported: actuator_pwm, zone_temp")


_OPS: dict[str, Callable[[int, int], bool]] = {
    "<=": lambda a, b: a <= b,
    "<":  lambda a, b: a <  b,
    ">=": lambda a, b: a >= b,
    ">":  lambda a, b: a >  b,
    "==": lambda a, b: a == b,
    "!=": lambda a, b: a != b,
}

# Plant fields that accept a single int via per-zone `plant zone <z>
# <field> <int>`.
_PER_ZONE_INT_FIELDS = {
    "initial_temp_mc",
    "ambient_mc",
    "load_w_q16",
    "heat_capacity_q16",
    "ambient_drift_q16",
    "fan_max_q16",
}

# 12b shorthand fields (apply to zone 0 implicitly).
_PLANT_SHORTHAND_FIELDS = {"ambient_mc", "initial_temp_mc"}

# 12c commands the runner knows how to apply.
_KNOWN_COMMANDS = {
    "freeze_input",
    "unfreeze_input",
    "freeze_tach",
    "unfreeze_tach",
    "set_plant_load_w_q16",
    "set_plant_fan_max_q16",
}


# === Exceptions ======================================================

class ScenarioParseError(Exception):
    pass


# === Data classes ====================================================

@dataclass
class Assertion:
    """Abstract base; subclasses implement evaluate(records)."""
    raw_line: str = ""

    def evaluate(self, records) -> tuple[bool, str]:
        raise NotImplementedError


@dataclass
class AssertMaxMin(Assertion):
    """Shared shape for `assert max` and `assert min`."""
    kind_str: str = "max"     # "max" or "min"
    signal_kind: str = ""
    slot: int = 0
    op: str = "<="
    value: int = 0
    t0_ms: int = 0
    t1_ms: int = 0

    def evaluate(self, records) -> tuple[bool, str]:
        sig_id = signal_id_for(self.signal_kind, self.slot)
        values = [r.value for r in records
                  if r.kind == "sample" and r.id == sig_id
                     and self.t0_ms <= r.ts_ms <= self.t1_ms]
        label = f"assert {self.kind_str} {self.signal_kind} {self.slot}"
        if not values:
            return False, f"{label}: no samples in [{self.t0_ms},{self.t1_ms}]"
        observed = max(values) if self.kind_str == "max" else min(values)
        ok = _OPS[self.op](observed, self.value)
        return ok, (f"{label}: observed = {observed}, op = {self.op}, "
                     f"threshold = {self.value}, "
                     f"{'PASS' if ok else 'FAIL'}")


@dataclass
class AssertEventually(Assertion):
    signal_kind: str = ""
    slot: int = 0
    op: str = ">="
    value: int = 0
    within_ms: int = 0

    def evaluate(self, records) -> tuple[bool, str]:
        sig_id = signal_id_for(self.signal_kind, self.slot)
        for r in records:
            if (r.kind == "sample" and r.id == sig_id
                    and r.ts_ms <= self.within_ms
                    and _OPS[self.op](r.value, self.value)):
                return True, (f"assert eventually {self.signal_kind} "
                               f"{self.slot} {self.op} {self.value}: "
                               f"first satisfying sample at "
                               f"{r.ts_ms}ms = {r.value}, PASS")
        return False, (f"assert eventually {self.signal_kind} "
                        f"{self.slot} {self.op} {self.value} within "
                        f"{self.within_ms}: never satisfied, FAIL")


@dataclass
class AssertWithinFaultActive(Assertion):
    within_ms: int = 0
    fault_kind: str = ""
    target_slot: int = 0

    def evaluate(self, records) -> tuple[bool, str]:
        ftype = FAULT_TYPES.get(self.fault_kind)
        if ftype is None:
            return False, (f"assert within fault_active: unknown fault "
                            f"kind '{self.fault_kind}'")
        for r in records:
            if (r.kind == "event" and r.id == TEVENT_FAULT_ENTER
                    and r.ts_ms <= self.within_ms
                    and r.a1 == ftype
                    and r.a2 == self.target_slot):
                return True, (f"assert within {self.within_ms} "
                               f"fault_active {self.fault_kind} "
                               f"{self.target_slot}: "
                               f"TEVENT_FAULT_ENTER at {r.ts_ms}ms, PASS")
        return False, (f"assert within {self.within_ms} fault_active "
                        f"{self.fault_kind} {self.target_slot}: "
                        f"no matching TEVENT_FAULT_ENTER, FAIL")


@dataclass
class AssertNoFaults(Assertion):
    except_kind: str = ""    # "" = strict, or one of FAULT_TYPES

    def evaluate(self, records) -> tuple[bool, str]:
        allowed_ftype = (FAULT_TYPES[self.except_kind]
                         if self.except_kind else None)
        fault_records = [r for r in records
                         if r.kind == "event"
                         and r.id in FAULT_EVENT_CODES
                         and (allowed_ftype is None
                              or r.a1 != allowed_ftype)]
        ok = not fault_records
        suffix = f" except {self.except_kind}" if self.except_kind else ""
        return ok, (f"assert no_faults{suffix}: "
                     f"{len(fault_records)} unexpected fault event(s), "
                     f"{'PASS' if ok else 'FAIL'}")


# Per-zone plant config snapshot used by the runner.
@dataclass
class ZoneInit:
    initial_temp_mc:    int | None = None
    ambient_mc:         int | None = None
    load_w_q16:         int | None = None
    heat_capacity_q16:  int | None = None
    ambient_drift_q16:  int | None = None
    fan_max_q16:        int | None = None
    coupling_neighbor:  int | None = None
    coupling_q16:       int | None = None


@dataclass
class Scenario:
    """Parsed scenario."""
    config_path: str | None = None
    plant_config: dict = field(default_factory=dict)
    zone_count: int = 1
    zones: list[ZoneInit] = field(default_factory=lambda: [ZoneInit()])
    commands: list[tuple[int, str, list[str]]] = field(default_factory=list)
    assertions: list[Assertion] = field(default_factory=list)

    @property
    def duration_ms(self) -> int:
        if "duration_ms" not in self.plant_config:
            raise ScenarioParseError(
                "scenario missing required `plant duration_ms <int>`")
        return int(self.plant_config["duration_ms"])


# === Parser ==========================================================

def parse_scenario(path) -> Scenario:
    text = Path(path).read_text(encoding="utf-8")
    scn = Scenario()
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        tokens = line.split()
        head = tokens[0]
        try:
            if head == "config":
                if len(tokens) != 2:
                    raise ScenarioParseError(
                        "`config <path>` takes exactly one arg")
                scn.config_path = tokens[1]
            elif head == "plant":
                _parse_plant(scn, tokens)
            elif head == "assert":
                _parse_assert(scn, tokens, raw_line=line)
            elif head.isdigit() or (head.startswith("-")
                                     and head[1:].isdigit()):
                _parse_command(scn, tokens)
            else:
                raise ScenarioParseError(
                    f"unknown directive '{head}'; expected "
                    f"'config', 'plant', 'assert', or "
                    f"'<int_ms> <cmd> <args>'")
        except ScenarioParseError as e:
            raise ScenarioParseError(
                f"{path}:{lineno}: {e}\n  line: {raw.rstrip()}") from None
    return scn


def _ensure_zone(scn: Scenario, zone: int) -> ZoneInit:
    while len(scn.zones) <= zone:
        scn.zones.append(ZoneInit())
    if zone + 1 > scn.zone_count:
        scn.zone_count = zone + 1
    return scn.zones[zone]


def _parse_plant(scn: Scenario, tokens: list[str]) -> None:
    if len(tokens) < 3:
        raise ScenarioParseError(
            "plant directive needs at least 2 args (field + int)")
    second = tokens[1]
    # plant zone_count <int>
    if second == "zone_count":
        if len(tokens) != 3:
            raise ScenarioParseError("`plant zone_count` takes one int")
        n = _to_int(tokens[2], "plant zone_count")
        if n < 1:
            raise ScenarioParseError("zone_count must be >= 1")
        scn.zone_count = n
        while len(scn.zones) < n:
            scn.zones.append(ZoneInit())
        return
    # plant zone <z> <field> <int> [<int>]
    if second == "zone":
        if len(tokens) < 5:
            raise ScenarioParseError(
                "`plant zone <z> <field> <int>` needs >= 4 args")
        z = _to_int(tokens[2], "plant zone idx")
        field_name = tokens[3]
        zinit = _ensure_zone(scn, z)
        if field_name == "coupling":
            if len(tokens) != 6:
                raise ScenarioParseError(
                    "`plant zone <z> coupling <neighbor> <q16>` "
                    "needs 2 args after `coupling`")
            neighbor = _to_int(tokens[4], "coupling neighbor")
            coup_q16 = _to_int(tokens[5], "coupling q16")
            zinit.coupling_neighbor = neighbor
            zinit.coupling_q16 = coup_q16
            return
        if field_name in _PER_ZONE_INT_FIELDS:
            if len(tokens) != 5:
                raise ScenarioParseError(
                    f"`plant zone <z> {field_name} <int>` takes one int")
            value = _to_int(tokens[4], f"plant zone {z} {field_name}")
            setattr(zinit, field_name, value)
            return
        raise ScenarioParseError(
            f"unknown per-zone field '{field_name}'; "
            f"accepted: {sorted(_PER_ZONE_INT_FIELDS)} + 'coupling'")
    # Shorthand: `plant <field> <int>` (12b form).
    if len(tokens) != 3:
        raise ScenarioParseError(
            f"plant {second}: expected `plant {second} <int>` "
            f"(2 args), got {len(tokens) - 1}")
    field_name = second
    value = _to_int(tokens[2], f"plant {field_name}")
    if field_name == "duration_ms":
        scn.plant_config["duration_ms"] = value
    elif field_name in _PLANT_SHORTHAND_FIELDS:
        # 12b shorthand: apply to zone 0.
        zinit = _ensure_zone(scn, 0)
        setattr(zinit, field_name, value)
    else:
        raise ScenarioParseError(
            f"plant field '{field_name}' not recognised; "
            f"shorthand fields: duration_ms, "
            f"{sorted(_PLANT_SHORTHAND_FIELDS)}; "
            f"per-zone: `plant zone <z> <field> <int>`")


def _parse_command(scn: Scenario, tokens: list[str]) -> None:
    # `<ms> <cmd> <args...>`
    ts_ms = _to_int(tokens[0], "command timestamp")
    if len(tokens) < 2:
        raise ScenarioParseError("timestamped command missing kind")
    cmd = tokens[1]
    args = tokens[2:]
    if cmd not in _KNOWN_COMMANDS:
        raise ScenarioParseError(
            f"unknown command '{cmd}'; accepted: "
            f"{sorted(_KNOWN_COMMANDS)}")
    # Type-check arg shapes per command.  Detailed validation here so
    # the runner doesn't have to repeat it.
    if cmd in ("freeze_input", "freeze_tach"):
        if len(args) != 2:
            raise ScenarioParseError(
                f"`{cmd} <name> <int>` needs exactly 2 args")
        _to_int(args[1], f"{cmd} value")
    elif cmd in ("unfreeze_input", "unfreeze_tach"):
        if len(args) != 1:
            raise ScenarioParseError(
                f"`{cmd} <name>` needs exactly 1 arg")
    elif cmd in ("set_plant_load_w_q16", "set_plant_fan_max_q16"):
        if len(args) != 2:
            raise ScenarioParseError(
                f"`{cmd} <zone> <q16>` needs exactly 2 args")
        _to_int(args[0], f"{cmd} zone")
        _to_int(args[1], f"{cmd} q16")
    scn.commands.append((ts_ms, cmd, args))


def _parse_assert(scn: Scenario, tokens: list[str], raw_line: str) -> None:
    if len(tokens) < 2:
        raise ScenarioParseError("`assert` requires a kind")
    kind = tokens[1]
    if kind == "no_faults":
        if len(tokens) == 2:
            scn.assertions.append(AssertNoFaults(raw_line=raw_line))
        elif len(tokens) == 4 and tokens[2] == "except":
            except_kind = tokens[3]
            if except_kind not in FAULT_TYPES:
                raise ScenarioParseError(
                    f"unknown fault kind '{except_kind}' in `except`; "
                    f"accepted: {sorted(FAULT_TYPES)}")
            scn.assertions.append(
                AssertNoFaults(raw_line=raw_line, except_kind=except_kind))
        else:
            raise ScenarioParseError(
                "`assert no_faults [except <kind>]` shape mismatch")
    elif kind in ("max", "min"):
        # assert <max|min> <signal_kind> <slot> <op> <int> between <t0> <t1>
        if len(tokens) != 9 or tokens[6] != "between":
            raise ScenarioParseError(
                f"`assert {kind}` shape: "
                f"{kind} <signal_kind> <slot> <op> <int> "
                f"between <t0> <t1>")
        signal_kind, raw_slot, op, raw_value = tokens[2:6]
        raw_t0, raw_t1 = tokens[7], tokens[8]
        if op not in _OPS:
            raise ScenarioParseError(
                f"unknown op '{op}'; expected one of: {sorted(_OPS)}")
        slot   = _to_int(raw_slot,  f"assert {kind} slot")
        value  = _to_int(raw_value, f"assert {kind} threshold")
        t0_ms  = _to_int(raw_t0,    f"assert {kind} t0")
        t1_ms  = _to_int(raw_t1,    f"assert {kind} t1")
        scn.assertions.append(
            AssertMaxMin(raw_line=raw_line, kind_str=kind,
                         signal_kind=signal_kind, slot=slot,
                         op=op, value=value,
                         t0_ms=t0_ms, t1_ms=t1_ms))
    elif kind == "eventually":
        # assert eventually <signal_kind> <slot> <op> <int> within <ms>
        if len(tokens) != 8 or tokens[6] != "within":
            raise ScenarioParseError(
                "`assert eventually` shape: "
                "eventually <signal_kind> <slot> <op> <int> within <ms>")
        signal_kind, raw_slot, op, raw_value = tokens[2:6]
        raw_within = tokens[7]
        if op not in _OPS:
            raise ScenarioParseError(
                f"unknown op '{op}'; expected one of: {sorted(_OPS)}")
        slot      = _to_int(raw_slot,   "assert eventually slot")
        value     = _to_int(raw_value,  "assert eventually threshold")
        within_ms = _to_int(raw_within, "assert eventually within_ms")
        scn.assertions.append(
            AssertEventually(raw_line=raw_line,
                             signal_kind=signal_kind, slot=slot,
                             op=op, value=value, within_ms=within_ms))
    elif kind == "within":
        # assert within <ms> fault_active <kind> <target_slot>
        if len(tokens) != 6 or tokens[3] != "fault_active":
            raise ScenarioParseError(
                "`assert within` shape: "
                "within <ms> fault_active <fault_kind> <target_slot>")
        within_ms   = _to_int(tokens[2], "assert within ms")
        fault_kind  = tokens[4]
        target_slot = _to_int(tokens[5], "assert within target_slot")
        if fault_kind not in FAULT_TYPES:
            raise ScenarioParseError(
                f"unknown fault kind '{fault_kind}'; "
                f"accepted: {sorted(FAULT_TYPES)}")
        scn.assertions.append(
            AssertWithinFaultActive(raw_line=raw_line,
                                     within_ms=within_ms,
                                     fault_kind=fault_kind,
                                     target_slot=target_slot))
    else:
        raise ScenarioParseError(
            f"assertion kind '{kind}' not supported; "
            f"accepted: max, min, eventually, within, no_faults")


# === Helpers =========================================================

def _to_int(s: str, where: str) -> int:
    try:
        return int(s)
    except ValueError:
        raise ScenarioParseError(
            f"{where}: expected integer, got '{s}'") from None
