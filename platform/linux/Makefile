# thermal-core — platform/linux/Makefile
# Builds the thermalcored daemon (Stage 9 9c).

CC ?= gcc
CFLAGS_BASE = -std=c99 -Wall -Wextra -Werror -pedantic \
              -I ../../core -I . $(CFLAGS_EXTRA)

BUILD_DIR  = ../../build/platform-linux
CORE_ARCH  = ../../build/core/libthermal_core.a

# Order matters only for readability; the linker sorts it out.
SRCS = thermalcored.c \
       bsp_mock_tmpfs.c \
       config_jsmn.c \
       jsmn.c \
       telem_wire.c
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)

DAEMON = $(BUILD_DIR)/thermalcored

.PHONY: all clean

all: $(DAEMON)

$(DAEMON): $(OBJS) $(CORE_ARCH)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $(OBJS) $(CORE_ARCH) $(LDFLAGS_EXTRA)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BASE) -c -o $@ $<

# The core archive is built by the top-level Makefile.  If a stale
# `make build` is invoked before `make all`, this rule explains
# clearly rather than silently failing.
$(CORE_ARCH):
	@echo "platform/linux: $(CORE_ARCH) missing; run 'make' from project root first"
	@exit 1

clean:
	rm -rf $(BUILD_DIR)
