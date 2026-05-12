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

# --- Replay tests ---
REPLAY_BIN_DIR     = build/replay
REPLAY_GOLDEN_DIR  = test/replay/golden
CURVE_REPLAY       = $(REPLAY_BIN_DIR)/curve_replay
FILTER_REPLAY      = $(REPLAY_BIN_DIR)/filter_replay
ZONE_REPLAY        = $(REPLAY_BIN_DIR)/zone_replay

# --- Property tests ---
PROPERTY_BIN_DIR   = build/property
PROPERTY_BIN       = $(PROPERTY_BIN_DIR)/property_config

.PHONY: all test build verify-portability replay regen-replay-goldens property clean

all: test build verify-portability replay property

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

build/test/test_%: test/unit/test_%.c test/unit/harness.h core/thermal_config.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

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

# --- Replay: C driver + golden + Python reference, all byte-equal ---
$(CURVE_REPLAY): test/replay/curve_replay.c core/thermal_curve.h \
                 core/thermal_types.h $(CORE_ARCHIVE)
	@mkdir -p $(REPLAY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

$(FILTER_REPLAY): test/replay/filter_replay.c core/thermal_filter.h \
                  $(CORE_ARCHIVE)
	@mkdir -p $(REPLAY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

$(ZONE_REPLAY): test/replay/zone_replay.c core/thermal_core.h \
                $(CORE_ARCHIVE)
	@mkdir -p $(REPLAY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

replay: $(CURVE_REPLAY) $(FILTER_REPLAY) $(ZONE_REPLAY)
	@echo "--- Replay: curve_sweep (C) ---"
	@$(CURVE_REPLAY) > $(REPLAY_BIN_DIR)/curve_sweep.csv
	@diff -u $(REPLAY_GOLDEN_DIR)/curve_sweep.csv \
	         $(REPLAY_BIN_DIR)/curve_sweep.csv \
	  || { echo "FAIL: C output differs from golden"; exit 1; }
	@echo "PASS: C == golden"
	@echo "--- Replay: curve_sweep (Python reference) ---"
	@python3 test/reference/curve.py > $(REPLAY_BIN_DIR)/curve_sweep.py.csv
	@diff -u $(REPLAY_BIN_DIR)/curve_sweep.csv \
	         $(REPLAY_BIN_DIR)/curve_sweep.py.csv \
	  || { echo "FAIL: Python reference differs from C output"; exit 1; }
	@echo "PASS: Python ref == C"
	@echo "--- Replay: filter_sweep (C) ---"
	@$(FILTER_REPLAY) > $(REPLAY_BIN_DIR)/filter_sweep.csv
	@diff -u $(REPLAY_GOLDEN_DIR)/filter_sweep.csv \
	         $(REPLAY_BIN_DIR)/filter_sweep.csv \
	  || { echo "FAIL: C output differs from golden"; exit 1; }
	@echo "PASS: C == golden"
	@echo "--- Replay: filter_sweep (Python reference) ---"
	@python3 test/reference/iir.py > $(REPLAY_BIN_DIR)/filter_sweep.py.csv
	@diff -u $(REPLAY_BIN_DIR)/filter_sweep.csv \
	         $(REPLAY_BIN_DIR)/filter_sweep.py.csv \
	  || { echo "FAIL: Python reference differs from C output"; exit 1; }
	@echo "PASS: Python ref == C"
	@echo "--- Replay: zone_sweep (C) ---"
	@$(ZONE_REPLAY) > $(REPLAY_BIN_DIR)/zone_sweep.csv
	@diff -u $(REPLAY_GOLDEN_DIR)/zone_sweep.csv \
	         $(REPLAY_BIN_DIR)/zone_sweep.csv \
	  || { echo "FAIL: C output differs from golden"; exit 1; }
	@echo "PASS: C == golden"
	@echo "--- Replay: zone_sweep (Python reference) ---"
	@python3 test/reference/zone.py > $(REPLAY_BIN_DIR)/zone_sweep.py.csv
	@diff -u $(REPLAY_BIN_DIR)/zone_sweep.csv \
	         $(REPLAY_BIN_DIR)/zone_sweep.py.csv \
	  || { echo "FAIL: Python reference differs from C output"; exit 1; }
	@echo "PASS: Python ref == C"

regen-replay-goldens: $(CURVE_REPLAY) $(FILTER_REPLAY) $(ZONE_REPLAY)
	@mkdir -p $(REPLAY_GOLDEN_DIR)
	$(CURVE_REPLAY) > $(REPLAY_GOLDEN_DIR)/curve_sweep.csv
	$(FILTER_REPLAY) > $(REPLAY_GOLDEN_DIR)/filter_sweep.csv
	$(ZONE_REPLAY) > $(REPLAY_GOLDEN_DIR)/zone_sweep.csv
	@echo "Regenerated goldens. Review the diff (git diff $(REPLAY_GOLDEN_DIR)/) before committing."

# --- Property test: validate_config across random configs ---
$(PROPERTY_BIN): test/property/property_config.c core/thermal_core.h \
                 $(CORE_ARCHIVE)
	@mkdir -p $(PROPERTY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

property: $(PROPERTY_BIN)
	@python3 test/property/run_property.py

# --- Build (delegates to platform/linux) ---
build:
	$(MAKE) -C platform/linux CC=$(CC)

clean:
	rm -rf build
	$(MAKE) -C platform/linux clean
