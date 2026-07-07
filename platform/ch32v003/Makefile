# platform/ch32v003/Makefile
#
# thermal-core STANDALONE firmware for the WCH CH32V003 (Stage 18).
# Builds the portable core/ + the CH32 BSP wrappers + the generated
# static config into a single RV32EC firmware via ch32fun's build
# system. STANDALONE only (PRD Appendix D.5).
#
#   make build   -- cross-compile the firmware (main.bin / main.elf)
#   make clean   -- remove build artifacts + the generated config
#
# The RISC-V cross-compiler (riscv64-unknown-elf-gcc) is a system
# tool, pinned in ci/tool-versions.md. ch32fun is the pinned git
# submodule under ch32fun/.

all : build

TARGET     := main
TARGET_MCU := CH32V003

CH32FUN := ch32fun/ch32fun

# Source JSON config for the firmware. Stage 20 added a second
# reference fan baseline (Arctic P8 PWM PST) alongside the active
# NF-A8 one -- swap by overriding this knob:
#   make build-ch32 CH32_TELEMETRY=1 \
#       CH32_CONFIG=configs/ch32v003-standalone-arctic-p8.json
CH32_CONFIG ?= configs/ch32v003-standalone.json

# Portable core sources, the generated static config, the BSP
# wrappers, the callback glue, and the shared canonical-CSV
# serializer all compile alongside main.c (ch32fun.mk's $(TARGET).c).
# No protocol/ -- a STANDALONE node does no wire I/O; the telemetry
# tap (Stage 18e) emits canonical CSV *text*, not binary frames.
CORE_SRCS        := $(wildcard ../../core/*.c)
GENERATED_CONFIG := generated/ch32_config.c
BSP_SRCS         := bsp_ch32_pwm.c bsp_ch32_tach.c bsp_ch32_sensor.c \
                    bsp_ch32_uart.c
# ch32_callbacks.c + canonical.c carry the telemetry tap; with
# CH32_TELEMETRY unset they link but --gc-sections strips the
# unreferenced telemetry/UART code, so the default firmware is
# unaffected.
TELEMETRY_SRCS   := ch32_callbacks.c ../../test/parity/canonical.c
# ch32_command.c carries the Stage 19 host-command parser; like the
# telemetry sources above it always compiles, but --gc-sections strips
# it whole unless CH32_COMMAND=1 wires it into main.c.
COMMAND_SRCS     := ch32_command.c

# M6: the active CH32 configs are step-wise-only, so PID is compiled
# out by default to preserve flash headroom before PID-specific M4 work.
# Set CH32_ENABLE_PID=1 for local diagnostics with PID configs.
CH32_ENABLE_PID ?= 0
ifeq ($(CH32_ENABLE_PID),1)
EXTRA_CFLAGS += -DTHERMALCORE_ENABLE_PID=1
else
EXTRA_CFLAGS += -DTHERMALCORE_ENABLE_PID=0
CORE_SRCS := $(filter-out ../../core/thermal_pid.c,$(CORE_SRCS))
endif

ADDITIONAL_C_FILES := $(GENERATED_CONFIG) $(CORE_SRCS) $(BSP_SRCS) \
                      $(TELEMETRY_SRCS) $(COMMAND_SRCS)

# -include force-applies the tiny build profile (PRD Appendix D.3):
# the core's THERMAL_MAX_* maxima collapse to the constrained values
# without editing core/thermal_config.h.
EXTRA_CFLAGS += -I. -I../../core -I../../test/parity -Igenerated \
                -include ../../core/thermal_profile_tiny.h

# CH32_COMMAND=1 implies CH32_TELEMETRY=1 -- command responses share
# the telemetry UART. Resolve the implication before the per-feature
# gates below so they all see the unified flag.
ifeq ($(CH32_COMMAND),1)
CH32_TELEMETRY := 1
endif

# Stage 18e: CH32_TELEMETRY=1 builds the canonical-CSV telemetry tap
# (USART1 PD5/PD6) into the firmware. Stage 20 also enables the
# advisory fan-health detector in the telemetry-only gate -- the
# detector produces telemetry-only output, so it rides the tap, and
# json2static emits the per-actuator fan_health baseline. The
# CH32_COMMAND bench-control image keeps fan-health compiled out by
# default to preserve flash headroom; set CH32_COMMAND_FAN_HEALTH=1
# only for local diagnostics that can tolerate the larger image. The
# default (CH32_TELEMETRY unset) firmware stays byte-for-byte unchanged.
# CH32_TELEMETRY_BAUD= overrides the 9600 default baud (e.g. =115200).
ifeq ($(CH32_TELEMETRY),1)
EXTRA_CFLAGS += -DTHERMALCORE_CH32_TELEMETRY=1
ifneq ($(CH32_COMMAND),1)
EXTRA_CFLAGS += -DTHERMALCORE_ENABLE_FAN_HEALTH=1
JSON2STATIC_FLAGS += --enable-fan-health
else ifeq ($(CH32_COMMAND_FAN_HEALTH),1)
EXTRA_CFLAGS += -DTHERMALCORE_ENABLE_FAN_HEALTH=1
JSON2STATIC_FLAGS += --enable-fan-health
endif
endif
ifdef CH32_TELEMETRY_BAUD
EXTRA_CFLAGS += -DTHERMALCORE_CH32_TELEMETRY_BAUD=$(CH32_TELEMETRY_BAUD)
endif

# CH32_STEP_TIMING=1 compiles in the per-tick execution-time probe
# (main.c): thermal_core_step() is bracketed by a SysTick read and its
# cost emitted as a `#`-comment line on the telemetry UART, so it
# lands in the same host capture as the canonical CSV. It therefore
# requires CH32_TELEMETRY=1. A characterisation diagnostic, default off.
ifeq ($(CH32_STEP_TIMING),1)
EXTRA_CFLAGS += -DTHERMALCORE_CH32_STEP_TIMING=1
endif

# CH32_COMMAND=1 builds the Stage 19 host-command channel: a USART1-RX
# command parser (ch32_command.c) + a control-loop bypass mode in
# main.c, driven by tools/thermal-telemetry-tool to capture a PWM->RPM
# sweep. The implication block above turns CH32_TELEMETRY on (so the
# telemetry block adds its defines). Fan-health stays out unless
# CH32_COMMAND_FAN_HEALTH=1 is set. A bench/characterisation build,
# NOT the shipping firmware -- default off; with it off the firmware
# is the standard STANDALONE regulator.
ifeq ($(CH32_COMMAND),1)
EXTRA_CFLAGS += -DTHERMALCORE_CH32_COMMAND=1
endif

include $(CH32FUN)/ch32fun.mk

# Generate the static thermal_config_t (+ ch32 pinmap) from JSON --
# the same json2static.py path the ESP32 build uses, with the CH32
# pinmap prefix so it emits ch32_pinmap_t G_CH32_PINMAP. The source
# JSON is $(CH32_CONFIG) (see the knob near the top of this file).
$(GENERATED_CONFIG) : $(CH32_CONFIG) \
                      ../../tools/json2static.py
	@mkdir -p generated
	python3 ../../tools/json2static.py $< -o $@ \
	    --symbol G_THERMAL_CFG --pinmap-prefix ch32 \
	    $(JSON2STATIC_FLAGS)

clean : cv_clean
	rm -rf generated

# Flash the firmware over a WCH-LinkE (ch32fun's minichlink).
flash : cv_flash

.PHONY : all build clean flash
