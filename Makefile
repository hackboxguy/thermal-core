# thermal-core — top-level Makefile
# Delegates to test/ for unit tests and platform/linux/ for the daemon build.
# Stage 0: harness sanity test, core-archive stub, portability guard.

CC ?= gcc
CFLAGS_BASE = -std=c99 -Wall -Wextra -Werror -pedantic -I core -I test/unit

# --- Unit test discovery (excludes core_only_runner.c — separate target) ---
TEST_SRCS = $(wildcard test/unit/test_*.c)
TEST_BINS = $(patsubst test/unit/test_%.c,build/test/test_%,$(TEST_SRCS))

# --- Core archive ---
CORE_SRCS = $(wildcard core/*.c)
CORE_OBJS = $(patsubst core/%.c,build/core/%.o,$(CORE_SRCS))
CORE_ARCHIVE = build/core/libthermal_core.a

# --- Portability-guard runtime runner ---
CORE_ONLY_RUNNER = build/test/core_only_runner
WRAP_FLAGS = -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc \
             -Wl,--wrap=free -Wl,--wrap=read -Wl,--wrap=write

.PHONY: all test build verify-portability clean

all: test build verify-portability

# --- Unit tests ---
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

# --- Core archive ---
build/core/%.o: core/%.c core/thermal_config.h
	@mkdir -p build/core
	$(CC) $(CFLAGS_BASE) -c -o $@ $<

$(CORE_ARCHIVE): $(CORE_OBJS)
	@mkdir -p build/core
	ar rcs $@ $^

# --- Portability guard: static nm -u + runtime --wrap'd runner ---
verify-portability: $(CORE_ARCHIVE) $(CORE_ONLY_RUNNER)
	@echo "--- Static check: nm -u vs ci/core-symbol-denylist.txt ---"
	@grep -v '^#' ci/core-symbol-denylist.txt | grep -v '^$$' > build/core/denylist.active
	@if nm -u --format=just-symbols $(CORE_ARCHIVE) \
		| grep -E -f build/core/denylist.active; then \
		echo "FAIL: core/ references forbidden symbols (above)"; \
		exit 1; \
	fi
	@echo "PASS: no forbidden undefined symbols in core/"
	@echo "--- Runtime check: core-only --wrap'd runner ---"
	@$(CORE_ONLY_RUNNER)

$(CORE_ONLY_RUNNER): test/unit/core_only_runner.c test/unit/harness.h \
                     core/thermal_config.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) $(WRAP_FLAGS) \
		-o $@ test/unit/core_only_runner.c $(CORE_ARCHIVE)

# --- Build (delegates to platform/linux) ---
build:
	$(MAKE) -C platform/linux CC=$(CC)

clean:
	rm -rf build
	$(MAKE) -C platform/linux clean
