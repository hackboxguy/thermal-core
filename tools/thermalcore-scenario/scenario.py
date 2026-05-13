"""tools/thermalcore-scenario/scenario.py — .scn parser + assertion grammar.

Stage 12 12b ships a deliberately small subset of the PRD section
7.6 grammar -- enough for idle_steady_state.  12c extends it with
freeze_input / set_setpoint / inject_fault commands and the
`within / eventually / except` assertion forms.

12b grammar:

    # comments
    plant ambient_mc      <int>
    plant initial_temp_mc <int>
    plant duration_ms     <int>

    assert max <signal_kind> <slot> <op> <int> between <t0_ms> <t1_ms>
    assert no_faults

Where:
  - <signal_kind> is one of: actuator_pwm, zone_temp
  - <slot> is an integer 0..15
  - <op>   is one of: <=, <, >=, >, ==, !=

Unknown directives raise ScenarioParseError immediately so 12c
gets a loud signal about what needs extending.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

# === Signal-ID mapping ===============================================
# Mirrors core/thermal_signals.h.  Only the slots we evaluate in 12b.

TSIG_ZONE_BASE       = 0x0100
TSIG_ZONE_SUB_TEMP   = 0x00
TSIG_ACTUATOR_BASE   = 0x0200
TSIG_ACTUATOR_SUB_DUTY = 0x00

# Fault event codes from core/thermal_events.h that `no_faults`
# should react to.  Other event codes (e.g. TEVENT_COMMAND_APPLIED
# = 0x1200) are intentionally not in this set.
FAULT_EVENT_CODES = frozenset({
    0x4000,   # TEVENT_FAULT_ENTER
    0x4001,   # TEVENT_FAULT_RECOVERING
    0x4002,   # TEVENT_FAULT_CLEAR
})


def signal_id_for(signal_kind: str, slot: int) -> int:
    if signal_kind == "actuator_pwm":
        return TSIG_ACTUATOR_BASE + TSIG_ACTUATOR_SUB_DUTY + slot
    if signal_kind == "zone_temp":
        return TSIG_ZONE_BASE + TSIG_ZONE_SUB_TEMP + slot
    raise ScenarioParseError(
        f"unknown signal kind '{signal_kind}'; "
        f"v1 12b supports only: actuator_pwm, zone_temp "
        f"(12c will extend)")


_OPS: dict[str, Callable[[int, int], bool]] = {
    "<=": lambda a, b: a <= b,
    "<":  lambda a, b: a <  b,
    ">=": lambda a, b: a >= b,
    ">":  lambda a, b: a >  b,
    "==": lambda a, b: a == b,
    "!=": lambda a, b: a != b,
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
class AssertMax(Assertion):
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
        if not values:
            return False, (f"assert max {self.signal_kind} {self.slot}: "
                            f"no samples in [{self.t0_ms},{self.t1_ms}]")
        observed = max(values)
        ok = _OPS[self.op](observed, self.value)
        return ok, (f"assert max {self.signal_kind} {self.slot}: "
                     f"observed max = {observed}, op = {self.op}, "
                     f"threshold = {self.value}, "
                     f"{'PASS' if ok else 'FAIL'}")


@dataclass
class AssertNoFaults(Assertion):
    def evaluate(self, records) -> tuple[bool, str]:
        fault_records = [r for r in records
                         if r.kind == "event"
                         and r.id in FAULT_EVENT_CODES]
        ok = not fault_records
        return ok, (f"assert no_faults: "
                     f"{len(fault_records)} fault event(s) recorded, "
                     f"{'PASS' if ok else 'FAIL'}")


@dataclass
class Scenario:
    """Parsed scenario."""
    plant_config: dict = field(default_factory=dict)
    commands: list[tuple[int, str, list[str]]] = field(default_factory=list)
    assertions: list[Assertion] = field(default_factory=list)

    @property
    def duration_ms(self) -> int:
        if "duration_ms" not in self.plant_config:
            raise ScenarioParseError(
                "scenario missing required `plant duration_ms <int>`")
        return int(self.plant_config["duration_ms"])


# === Parser ==========================================================

_PLANT_FIELDS_12B = {"ambient_mc", "initial_temp_mc", "duration_ms"}


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
            if head == "plant":
                _parse_plant(scn, tokens)
            elif head == "assert":
                _parse_assert(scn, tokens, raw_line=line)
            elif head.isdigit() or (head.startswith("-") and head[1:].isdigit()):
                _parse_command(scn, tokens)
            else:
                raise ScenarioParseError(
                    f"unknown directive '{head}'; expected "
                    f"'plant', 'assert', or '<int_ms> <cmd> <args>'")
        except ScenarioParseError as e:
            raise ScenarioParseError(
                f"{path}:{lineno}: {e}\n  line: {raw.rstrip()}") from None
    return scn


def _parse_plant(scn: Scenario, tokens: list[str]) -> None:
    # `plant <field> <int>`
    if len(tokens) != 3:
        raise ScenarioParseError(
            f"plant directive expects 2 args (field + int), "
            f"got {len(tokens) - 1}")
    field_name, raw_value = tokens[1], tokens[2]
    if field_name not in _PLANT_FIELDS_12B:
        raise ScenarioParseError(
            f"plant field '{field_name}' not supported in 12b; "
            f"v1 12b accepts: {sorted(_PLANT_FIELDS_12B)} (12c extends)")
    try:
        value = int(raw_value)
    except ValueError:
        raise ScenarioParseError(
            f"plant {field_name}: expected integer, got '{raw_value}'")
    scn.plant_config[field_name] = value


def _parse_command(scn: Scenario, tokens: list[str]) -> None:
    # `<ms> <cmd> <args...>` -- v1 12b has no commands; reject loudly.
    raise ScenarioParseError(
        f"timestamped scenario commands not supported in 12b; "
        f"v1 12b has no commands -- 12c adds freeze_input, "
        f"set_setpoint, inject_fault")


def _parse_assert(scn: Scenario, tokens: list[str], raw_line: str) -> None:
    if len(tokens) < 2:
        raise ScenarioParseError("`assert` requires a kind")
    kind = tokens[1]
    if kind == "no_faults":
        if len(tokens) != 2:
            raise ScenarioParseError(
                "`assert no_faults` takes no args in 12b; "
                "12c adds `assert no_faults except <type>`")
        scn.assertions.append(AssertNoFaults(raw_line=raw_line))
    elif kind == "max":
        # assert max <signal_kind> <slot> <op> <int> between <t0> <t1>
        if len(tokens) != 9 or tokens[6] != "between":
            raise ScenarioParseError(
                "`assert max` shape: "
                "max <signal_kind> <slot> <op> <int> between <t0> <t1>")
        signal_kind, raw_slot, op, raw_value = tokens[2:6]
        raw_t0, raw_t1 = tokens[7], tokens[8]
        if op not in _OPS:
            raise ScenarioParseError(
                f"unknown op '{op}'; expected one of: {sorted(_OPS)}")
        try:
            slot = int(raw_slot)
            value = int(raw_value)
            t0_ms = int(raw_t0)
            t1_ms = int(raw_t1)
        except ValueError as e:
            raise ScenarioParseError(f"bad integer in `assert max`: {e}")
        scn.assertions.append(
            AssertMax(raw_line=raw_line,
                      signal_kind=signal_kind, slot=slot,
                      op=op, value=value,
                      t0_ms=t0_ms, t1_ms=t1_ms))
    else:
        raise ScenarioParseError(
            f"assertion kind '{kind}' not supported in 12b; "
            f"v1 12b accepts: max, no_faults (12c extends with "
            f"within, eventually, no_faults except <type>)")
