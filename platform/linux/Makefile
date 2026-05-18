# thermal-core — platform/linux/Makefile
# Builds the thermalcored daemon.

CC ?= gcc

# Stage 17: the daemon must be built with the same fan-health compile
# flag as the core archive it links (../../build/core/libthermal_core.a,
# built by the root Makefile) -- otherwise thermal_config_t differs
# between the two and the layouts disagree.
FAN_HEALTH_FLAG = -DTHERMALCORE_ENABLE_FAN_HEALTH=1

CFLAGS_BASE = -std=c99 -Wall -Wextra -Werror -pedantic \
              -I ../../core -I ../../protocol -I . $(FAN_HEALTH_FLAG) $(CFLAGS_EXTRA)

BUILD_DIR  = ../../build/platform-linux
CORE_ARCH  = ../../build/core/libthermal_core.a

# Local platform sources.  thermal_wire.c + obd2.c live in
# ../../protocol/ and are compiled by explicit rules below.
LOCAL_SRCS = thermalcored.c \
             bsp_mock_tmpfs.c \
             bsp_socketcan.c \
             bsp_hil_serial.c \
             config_jsmn.c \
             jsmn.c
LOCAL_OBJS = $(LOCAL_SRCS:%.c=$(BUILD_DIR)/%.o)

# Canonical wire codec (Stage 10 10a) + OBD-II codec (Stage 11 11a).
# Compiled into the daemon directly rather than via a separate
# archive -- avoids cross-makefile coupling and keeps the build
# dependency simple.
PROTOCOL_OBJS = $(BUILD_DIR)/thermal_wire.o $(BUILD_DIR)/obd2.o

OBJS   = $(LOCAL_OBJS) $(PROTOCOL_OBJS)
DAEMON = $(BUILD_DIR)/thermalcored

.PHONY: all clean

all: $(DAEMON)

$(DAEMON): $(OBJS) $(CORE_ARCH)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $(OBJS) $(CORE_ARCH) $(LDFLAGS_EXTRA)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BASE) -c -o $@ $<

$(BUILD_DIR)/thermal_wire.o: ../../protocol/thermal_wire.c \
                              ../../protocol/thermal_wire.h \
                              ../../protocol/thermal_wire_opcodes.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BASE) -c -o $@ ../../protocol/thermal_wire.c

$(BUILD_DIR)/obd2.o: ../../protocol/obd2.c ../../protocol/obd2.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BASE) -c -o $@ ../../protocol/obd2.c

# The core archive is built by the top-level Makefile.  If a stale
# `make build` is invoked before `make all`, this rule explains
# clearly rather than silently failing.
$(CORE_ARCH):
	@echo "platform/linux: $(CORE_ARCH) missing; run 'make' from project root first"
	@exit 1

clean:
	rm -rf $(BUILD_DIR)
