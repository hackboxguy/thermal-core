# thermal-core — top-level Makefile
# Delegates to test/ for unit tests and platform/linux/ for the daemon build.
# Stage 0: only the harness sanity test exists.

CC ?= gcc
CFLAGS_BASE = -std=c99 -Wall -Wextra -Werror -pedantic -I core -I test/unit

TEST_SRCS = $(wildcard test/unit/test_*.c)
TEST_BINS = $(patsubst test/unit/test_%.c,build/test/test_%,$(TEST_SRCS))

.PHONY: all test build clean

all: test build

test: $(TEST_BINS)
	@set -e; \
	pass=0; fail=0; \
	for t in $(TEST_BINS); do \
		if "$$t"; then pass=$$((pass+1)); else fail=$$((fail+1)); fi; \
	done; \
	echo "---"; \
	echo "Tests: $$pass passed, $$fail failed"; \
	[ "$$fail" = "0" ]

build/test/test_%: test/unit/test_%.c test/unit/harness.h core/thermal_config.h
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -o $@ $<

build:
	$(MAKE) -C platform/linux CC=$(CC)

clean:
	rm -rf build
	$(MAKE) -C platform/linux clean
