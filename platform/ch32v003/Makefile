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

ADDITIONAL_C_FILES := $(GENERATED_CONFIG) $(CORE_SRCS) $(BSP_SRCS) \
                      $(TELEMETRY_SRCS)

# -include force-applies the tiny build profile (PRD Appendix D.3):
# the core's THERMAL_MAX_* maxima collapse to the constrained values
# without editing core/thermal_config.h.
EXTRA_CFLAGS += -I. -I../../core -I../../test/parity -Igenerated \
                -include ../../core/thermal_profile_tiny.h

# Stage 18e: CH32_TELEMETRY=1 builds the canonical-CSV telemetry tap
# (USART1 PD5/PD6) into the firmware. CH32_TELEMETRY_BAUD overrides
# the default 9600 telemetry baud (e.g. CH32_TELEMETRY_BAUD=115200).
ifeq ($(CH32_TELEMETRY),1)
EXTRA_CFLAGS += -DTHERMALCORE_CH32_TELEMETRY=1
endif
ifdef CH32_TELEMETRY_BAUD
EXTRA_CFLAGS += -DTHERMALCORE_CH32_TELEMETRY_BAUD=$(CH32_TELEMETRY_BAUD)
endif

# CH32_STEP_TIMING=1 compiles in the per-tick execution-time probe
# (main.c): thermal_core_step() is bracketed by a SysTick read and its
# cost printed over SWIO. A characterisation diagnostic, default off.
ifeq ($(CH32_STEP_TIMING),1)
EXTRA_CFLAGS += -DTHERMALCORE_CH32_STEP_TIMING=1
endif

include $(CH32FUN)/ch32fun.mk

# Generate the static thermal_config_t (+ ch32 pinmap) from JSON --
# the same json2static.py path the ESP32 build uses, with the CH32
# pinmap prefix so it emits ch32_pinmap_t G_CH32_PINMAP.
$(GENERATED_CONFIG) : configs/ch32v003-standalone.json \
                      ../../tools/json2static.py
	@mkdir -p generated
	python3 ../../tools/json2static.py $< -o $@ \
	    --symbol G_THERMAL_CFG --pinmap-prefix ch32

clean : cv_clean
	rm -rf generated

# Flash the firmware over a WCH-LinkE (ch32fun's minichlink).
flash : cv_flash

.PHONY : all build clean flash
