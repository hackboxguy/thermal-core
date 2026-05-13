"""tools/thermalcore-scenario/plant_ffi.py — ctypes binding to libplant.so.

Mirrors the C structs + public API from tools/thermalcore-scenario/
plant.h (Stage 12 12a) so the scenario runner (12b) can advance
the deterministic plant in lockstep with the daemon.

The shared library is built by the project Makefile at
build/tools/thermalcore-scenario/libplant.so.  Layout parity
between the Python ctypes Structures and the C structs is the
load-bearing invariant; if it drifts, plant_step writes garbage
into the wrong fields.
"""
from __future__ import annotations

import ctypes
from pathlib import Path

# === Layout constants (must match plant.h) ===========================

PLANT_MAX_ZONES      = 4
PLANT_MAX_FAN_POINTS = 8


class ThermalCurvePoint(ctypes.Structure):
    """Mirror of thermal_curve_point_t in core/thermal_types.h."""
    _fields_ = [
        ("x",      ctypes.c_int32),
        ("value0", ctypes.c_int32),
        ("value1", ctypes.c_int32),
    ]


class PlantZone(ctypes.Structure):
    """Mirror of plant_zone_t in plant.h.

    Field order + types MUST match the C struct exactly; ctypes
    pads to natural alignment which matches what gcc emits for
    this struct on every Linux ABI we target."""
    _fields_ = [
        ("temp_mc",              ctypes.c_int32),
        ("ambient_mc",           ctypes.c_int32),
        ("load_w_q16",           ctypes.c_int32),
        ("heat_capacity_q16",    ctypes.c_int32),
        ("ambient_drift_q16",    ctypes.c_int32),
        ("fan_max_q16",          ctypes.c_int32),
        ("fan_curve",            ThermalCurvePoint * PLANT_MAX_FAN_POINTS),
        ("fan_curve_count",      ctypes.c_uint8),
        ("coupling_neighbor",    ctypes.c_int8),
        ("coupling_q16",         ctypes.c_int32),
    ]


class PlantState(ctypes.Structure):
    """Mirror of plant_t in plant.h."""
    _fields_ = [
        ("zones",                PlantZone * PLANT_MAX_ZONES),
        ("zone_count",           ctypes.c_uint8),
        ("prng_state",           ctypes.c_uint32),
        ("noise_amplitude_mc",   ctypes.c_int32),
    ]


class PlantInputs(ctypes.Structure):
    """Mirror of plant_inputs_t in plant.h."""
    _fields_ = [
        ("pwm", ctypes.c_uint8 * PLANT_MAX_ZONES),
    ]


# === Library loader ==================================================

_REPO_ROOT = Path(__file__).resolve().parents[2]
LIBPLANT_PATH = _REPO_ROOT / "build" / "tools" / "thermalcore-scenario" / "libplant.so"


def _bind_lib() -> ctypes.CDLL:
    if not LIBPLANT_PATH.exists():
        raise SystemExit(
            f"plant_ffi: {LIBPLANT_PATH} missing -- "
            f"run `make build` and ensure libplant.so was built")
    lib = ctypes.CDLL(str(LIBPLANT_PATH))

    lib.plant_init.argtypes = [ctypes.POINTER(PlantState), ctypes.c_uint32]
    lib.plant_init.restype = None

    lib.plant_set_zone_count.argtypes = [ctypes.POINTER(PlantState), ctypes.c_uint8]
    lib.plant_set_zone_count.restype = None

    for setter in ("plant_zone_set_temperature",
                   "plant_zone_set_ambient",
                   "plant_zone_set_load_w_q16",
                   "plant_zone_set_heat_capacity",
                   "plant_zone_set_ambient_drift",
                   "plant_zone_set_fan_max"):
        f = getattr(lib, setter)
        f.argtypes = [ctypes.POINTER(PlantState),
                      ctypes.c_uint8, ctypes.c_int32]
        f.restype = None

    lib.plant_zone_set_fan_curve.argtypes = [
        ctypes.POINTER(PlantState), ctypes.c_uint8,
        ctypes.POINTER(ThermalCurvePoint), ctypes.c_uint8]
    lib.plant_zone_set_fan_curve.restype = None

    lib.plant_zone_set_coupling.argtypes = [
        ctypes.POINTER(PlantState), ctypes.c_uint8,
        ctypes.c_int8, ctypes.c_int32]
    lib.plant_zone_set_coupling.restype = None

    lib.plant_set_noise_amplitude_mc.argtypes = [
        ctypes.POINTER(PlantState), ctypes.c_int32]
    lib.plant_set_noise_amplitude_mc.restype = None

    lib.plant_step.argtypes = [
        ctypes.POINTER(PlantState),
        ctypes.POINTER(PlantInputs),
        ctypes.c_uint32]
    lib.plant_step.restype = None

    lib.plant_zone_temp_mc.argtypes = [
        ctypes.POINTER(PlantState), ctypes.c_uint8]
    lib.plant_zone_temp_mc.restype = ctypes.c_int32

    lib.plant_zone_load_w_q16.argtypes = [
        ctypes.POINTER(PlantState), ctypes.c_uint8]
    lib.plant_zone_load_w_q16.restype = ctypes.c_int32

    return lib


# === Pythonic wrapper ================================================

class Plant:
    """Thin OO wrapper around the C plant API.

    Methods map 1:1 to plant_* functions; the wrapper holds the
    PlantState struct + a ctypes-loaded CDLL handle so the runner
    doesn't have to thread `ctypes.byref(state)` through every
    call."""

    def __init__(self, seed: int = 42):
        self._lib = _bind_lib()
        self._state = PlantState()
        self._lib.plant_init(ctypes.byref(self._state), seed)

    def set_zone_count(self, n: int) -> None:
        self._lib.plant_set_zone_count(ctypes.byref(self._state), n)

    def set_temperature(self, zone: int, mc: int) -> None:
        self._lib.plant_zone_set_temperature(
            ctypes.byref(self._state), zone, mc)

    def set_ambient(self, zone: int, mc: int) -> None:
        self._lib.plant_zone_set_ambient(
            ctypes.byref(self._state), zone, mc)

    def set_load_w_q16(self, zone: int, load_q16: int) -> None:
        self._lib.plant_zone_set_load_w_q16(
            ctypes.byref(self._state), zone, load_q16)

    def set_heat_capacity_q16(self, zone: int, cap_q16: int) -> None:
        self._lib.plant_zone_set_heat_capacity(
            ctypes.byref(self._state), zone, cap_q16)

    def set_ambient_drift_q16(self, zone: int, drift_q16: int) -> None:
        self._lib.plant_zone_set_ambient_drift(
            ctypes.byref(self._state), zone, drift_q16)

    def set_fan_max_q16(self, zone: int, fan_max_q16: int) -> None:
        self._lib.plant_zone_set_fan_max(
            ctypes.byref(self._state), zone, fan_max_q16)

    def set_fan_curve(self, zone: int,
                      points: list[tuple[int, int, int]]) -> None:
        """`points` is a list of (x, value0, value1) tuples."""
        arr = (ThermalCurvePoint * len(points))(
            *[ThermalCurvePoint(x=x, value0=v0, value1=v1)
              for (x, v0, v1) in points])
        self._lib.plant_zone_set_fan_curve(
            ctypes.byref(self._state), zone, arr, len(points))

    def set_coupling(self, zone: int, neighbor: int,
                     coupling_q16: int) -> None:
        self._lib.plant_zone_set_coupling(
            ctypes.byref(self._state), zone, neighbor, coupling_q16)

    def set_noise_amplitude_mc(self, amp_mc: int) -> None:
        self._lib.plant_set_noise_amplitude_mc(
            ctypes.byref(self._state), amp_mc)

    def step(self, pwms: list[int], dt_ms: int) -> None:
        """Step every zone by `dt_ms`.  `pwms[i]` is the PWM for
        zone i (0..255).  Missing zones default to PWM=0."""
        inputs = PlantInputs()
        for i, pwm in enumerate(pwms[:PLANT_MAX_ZONES]):
            inputs.pwm[i] = pwm & 0xFF
        self._lib.plant_step(ctypes.byref(self._state),
                              ctypes.byref(inputs), dt_ms)

    def zone_temp_mc(self, zone: int) -> int:
        return int(self._lib.plant_zone_temp_mc(
            ctypes.byref(self._state), zone))

    def zone_load_w_q16(self, zone: int) -> int:
        return int(self._lib.plant_zone_load_w_q16(
            ctypes.byref(self._state), zone))
