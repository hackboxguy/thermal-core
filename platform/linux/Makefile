# thermal-core — platform/linux/Makefile (Stage 0 stub)
# Replaced with the real daemon build at Stage 9.

CC ?= gcc
CFLAGS_BASE = -std=c99 -Wall -Wextra -Werror -pedantic -I ../../core

BUILD_DIR = ../../build/platform-linux

.PHONY: all clean

all: $(BUILD_DIR)/stage0_sanity

$(BUILD_DIR)/stage0_sanity: stage0_sanity.c ../../core/thermal_config.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $<

clean:
	rm -rf $(BUILD_DIR)
