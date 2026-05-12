# thermal-core — top-level Makefile
# Delegates to test/ for unit tests and platform/linux/ for the daemon build.
# Stage 0: harness sanity test, core-archive stub, portability guard.

CC ?= gcc
CFLAGS_BASE = -std=c99 -Wall -Wextra -Werror -pedantic -I core -I test/unit $(CFLAGS_EXTRA)

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
CURVE_REPLAY            = $(REPLAY_BIN_DIR)/curve_replay
FILTER_REPLAY           = $(REPLAY_BIN_DIR)/filter_replay
ZONE_REPLAY             = $(REPLAY_BIN_DIR)/zone_replay
PID_REPLAY              = $(REPLAY_BIN_DIR)/pid_replay
STALL_REPLAY            = $(REPLAY_BIN_DIR)/stall_replay
STUCK_SENSOR_REPLAY     = $(REPLAY_BIN_DIR)/stuck_sensor_replay
RUNAWAY_REPLAY          = $(REPLAY_BIN_DIR)/runaway_replay
STALE_CONTEXT_REPLAY    = $(REPLAY_BIN_DIR)/stale_context_replay
FULL_STEP_REPLAY        = $(REPLAY_BIN_DIR)/full_step_replay

# --- Property tests ---
PROPERTY_BIN_DIR   = build/property
PROPERTY_BIN          = $(PROPERTY_BIN_DIR)/property_config
PROPERTY_COMMAND_BIN  = $(PROPERTY_BIN_DIR)/property_command

.PHONY: all test build verify-portability replay regen-replay-goldens property property-command asan clang-tidy cppcheck clean

all: test build verify-portability replay property property-command

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
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE) $(LDFLAGS_EXTRA)

# --- Special: JSON loader test pulls in platform/linux sources ---
# Overrides the wildcard rule above for this specific target because
# it links config_jsmn.c + jsmn.c alongside the test driver.
build/test/test_config_jsmn: \
    test/unit/test_config_jsmn.c test/unit/harness.h \
    platform/linux/config_jsmn.c platform/linux/config_jsmn.h \
    platform/linux/jsmn.c platform/linux/jsmn.h \
    core/thermal_config.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -I platform/linux \
	    -o $@ test/unit/test_config_jsmn.c \
	    platform/linux/config_jsmn.c platform/linux/jsmn.c \
	    $(CORE_ARCHIVE) $(LDFLAGS_EXTRA)

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

$(PID_REPLAY): test/replay/pid_replay.c core/thermal_pid.h \
               $(CORE_ARCHIVE)
	@mkdir -p $(REPLAY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

$(STALL_REPLAY): test/replay/stall_replay.c core/thermal_fault.h \
                 $(CORE_ARCHIVE)
	@mkdir -p $(REPLAY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

$(STUCK_SENSOR_REPLAY): test/replay/stuck_sensor_replay.c core/thermal_fault.h \
                        $(CORE_ARCHIVE)
	@mkdir -p $(REPLAY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

$(RUNAWAY_REPLAY): test/replay/runaway_replay.c core/thermal_fault.h \
                   $(CORE_ARCHIVE)
	@mkdir -p $(REPLAY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

$(STALE_CONTEXT_REPLAY): test/replay/stale_context_replay.c core/thermal_fault.h \
                         $(CORE_ARCHIVE)
	@mkdir -p $(REPLAY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

$(FULL_STEP_REPLAY): test/replay/full_step_replay.c core/thermal_core.h \
                     $(CORE_ARCHIVE)
	@mkdir -p $(REPLAY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

replay: $(CURVE_REPLAY) $(FILTER_REPLAY) $(ZONE_REPLAY) $(PID_REPLAY) \
        $(STALL_REPLAY) $(STUCK_SENSOR_REPLAY) $(RUNAWAY_REPLAY) \
        $(STALE_CONTEXT_REPLAY) $(FULL_STEP_REPLAY)
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
	@echo "--- Replay: pid_sweep (C) ---"
	@$(PID_REPLAY) > $(REPLAY_BIN_DIR)/pid_sweep.csv
	@diff -u $(REPLAY_GOLDEN_DIR)/pid_sweep.csv \
	         $(REPLAY_BIN_DIR)/pid_sweep.csv \
	  || { echo "FAIL: C output differs from golden"; exit 1; }
	@echo "PASS: C == golden"
	@echo "--- Replay: pid_sweep (Python reference) ---"
	@python3 test/reference/pid.py > $(REPLAY_BIN_DIR)/pid_sweep.py.csv
	@diff -u $(REPLAY_BIN_DIR)/pid_sweep.csv \
	         $(REPLAY_BIN_DIR)/pid_sweep.py.csv \
	  || { echo "FAIL: Python reference differs from C output"; exit 1; }
	@echo "PASS: Python ref == C"
	@echo "--- Replay: stall_sweep (C) ---"
	@$(STALL_REPLAY) > $(REPLAY_BIN_DIR)/stall_sweep.csv
	@diff -u $(REPLAY_GOLDEN_DIR)/stall_sweep.csv \
	         $(REPLAY_BIN_DIR)/stall_sweep.csv \
	  || { echo "FAIL: C output differs from golden"; exit 1; }
	@echo "PASS: C == golden"
	@echo "--- Replay: stall_sweep (Python reference) ---"
	@python3 test/reference/stall.py > $(REPLAY_BIN_DIR)/stall_sweep.py.csv
	@diff -u $(REPLAY_BIN_DIR)/stall_sweep.csv \
	         $(REPLAY_BIN_DIR)/stall_sweep.py.csv \
	  || { echo "FAIL: Python reference differs from C output"; exit 1; }
	@echo "PASS: Python ref == C"
	@echo "--- Replay: stuck_sensor_sweep (C) ---"
	@$(STUCK_SENSOR_REPLAY) > $(REPLAY_BIN_DIR)/stuck_sensor_sweep.csv
	@diff -u $(REPLAY_GOLDEN_DIR)/stuck_sensor_sweep.csv \
	         $(REPLAY_BIN_DIR)/stuck_sensor_sweep.csv \
	  || { echo "FAIL: C output differs from golden"; exit 1; }
	@echo "PASS: C == golden"
	@echo "--- Replay: stuck_sensor_sweep (Python reference) ---"
	@python3 test/reference/stuck_sensor.py > $(REPLAY_BIN_DIR)/stuck_sensor_sweep.py.csv
	@diff -u $(REPLAY_BIN_DIR)/stuck_sensor_sweep.csv \
	         $(REPLAY_BIN_DIR)/stuck_sensor_sweep.py.csv \
	  || { echo "FAIL: Python reference differs from C output"; exit 1; }
	@echo "PASS: Python ref == C"
	@echo "--- Replay: runaway_sweep (C) ---"
	@$(RUNAWAY_REPLAY) > $(REPLAY_BIN_DIR)/runaway_sweep.csv
	@diff -u $(REPLAY_GOLDEN_DIR)/runaway_sweep.csv \
	         $(REPLAY_BIN_DIR)/runaway_sweep.csv \
	  || { echo "FAIL: C output differs from golden"; exit 1; }
	@echo "PASS: C == golden"
	@echo "--- Replay: runaway_sweep (Python reference) ---"
	@python3 test/reference/runaway.py > $(REPLAY_BIN_DIR)/runaway_sweep.py.csv
	@diff -u $(REPLAY_BIN_DIR)/runaway_sweep.csv \
	         $(REPLAY_BIN_DIR)/runaway_sweep.py.csv \
	  || { echo "FAIL: Python reference differs from C output"; exit 1; }
	@echo "PASS: Python ref == C"
	@echo "--- Replay: stale_context_sweep (C) ---"
	@$(STALE_CONTEXT_REPLAY) > $(REPLAY_BIN_DIR)/stale_context_sweep.csv
	@diff -u $(REPLAY_GOLDEN_DIR)/stale_context_sweep.csv \
	         $(REPLAY_BIN_DIR)/stale_context_sweep.csv \
	  || { echo "FAIL: C output differs from golden"; exit 1; }
	@echo "PASS: C == golden"
	@echo "--- Replay: stale_context_sweep (Python reference) ---"
	@python3 test/reference/stale_context.py > $(REPLAY_BIN_DIR)/stale_context_sweep.py.csv
	@diff -u $(REPLAY_BIN_DIR)/stale_context_sweep.csv \
	         $(REPLAY_BIN_DIR)/stale_context_sweep.py.csv \
	  || { echo "FAIL: Python reference differs from C output"; exit 1; }
	@echo "PASS: Python ref == C"
	@echo "--- Replay: full_step_sweep (C, no Python ref) ---"
	@$(FULL_STEP_REPLAY) > $(REPLAY_BIN_DIR)/full_step_sweep.csv
	@diff -u $(REPLAY_GOLDEN_DIR)/full_step_sweep.csv \
	         $(REPLAY_BIN_DIR)/full_step_sweep.csv \
	  || { echo "FAIL: C output differs from golden"; exit 1; }
	@echo "PASS: C == golden"

regen-replay-goldens: $(CURVE_REPLAY) $(FILTER_REPLAY) $(ZONE_REPLAY) $(PID_REPLAY) \
                      $(STALL_REPLAY) $(STUCK_SENSOR_REPLAY) $(RUNAWAY_REPLAY) \
                      $(STALE_CONTEXT_REPLAY) $(FULL_STEP_REPLAY)
	@mkdir -p $(REPLAY_GOLDEN_DIR)
	$(CURVE_REPLAY) > $(REPLAY_GOLDEN_DIR)/curve_sweep.csv
	$(FILTER_REPLAY) > $(REPLAY_GOLDEN_DIR)/filter_sweep.csv
	$(ZONE_REPLAY) > $(REPLAY_GOLDEN_DIR)/zone_sweep.csv
	$(PID_REPLAY) > $(REPLAY_GOLDEN_DIR)/pid_sweep.csv
	$(STALL_REPLAY) > $(REPLAY_GOLDEN_DIR)/stall_sweep.csv
	$(STUCK_SENSOR_REPLAY) > $(REPLAY_GOLDEN_DIR)/stuck_sensor_sweep.csv
	$(RUNAWAY_REPLAY) > $(REPLAY_GOLDEN_DIR)/runaway_sweep.csv
	$(STALE_CONTEXT_REPLAY) > $(REPLAY_GOLDEN_DIR)/stale_context_sweep.csv
	$(FULL_STEP_REPLAY) > $(REPLAY_GOLDEN_DIR)/full_step_sweep.csv
	@echo "Regenerated goldens. Review the diff (git diff $(REPLAY_GOLDEN_DIR)/) before committing."

# --- Property test: validate_config across random configs ---
$(PROPERTY_BIN): test/property/property_config.c core/thermal_core.h \
                 $(CORE_ARCHIVE)
	@mkdir -p $(PROPERTY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

property: $(PROPERTY_BIN)
	@python3 test/property/run_property.py

# --- Property test: apply_command across random commands (Stage 8 8b) ---
$(PROPERTY_COMMAND_BIN): test/property/property_command.c core/thermal_core.h \
                         core/thermal_commands.h $(CORE_ARCHIVE)
	@mkdir -p $(PROPERTY_BIN_DIR)
	$(CC) $(CFLAGS_BASE) -o $@ $< $(CORE_ARCHIVE)

property-command: $(PROPERTY_COMMAND_BIN)
	@python3 test/property/run_property_command.py

# --- ASan/UBSan unit-test build ---
# Sanitizer-clean is a merge gate from Stage 6 onward (plan §3).
# Skips verify-portability (-Wl,--wrap=malloc conflicts with ASan's
# malloc interception) and replay/property (unit tests are the primary
# catch surface for the detector and module math).
asan:
	$(MAKE) clean
	$(MAKE) test \
	    CFLAGS_EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
	    LDFLAGS_EXTRA="-fsanitize=address,undefined"
	@echo "--- Cleaning sanitizer artifacts so non-asan targets relink cleanly ---"
	@$(MAKE) clean >/dev/null

# --- Build (delegates to platform/linux) ---
build:
	$(MAKE) -C platform/linux CC=$(CC)

# --- clang-tidy on core/ only (Stage 7 7d). Config in .clang-tidy. ---
clang-tidy:
	@which clang-tidy >/dev/null || { echo "clang-tidy not installed"; exit 1; }
	clang-tidy --quiet core/*.c -- $(CFLAGS_BASE)

# --- cppcheck on core/ only (Stage 8 8b). ---
# Suppressions in .cppcheck-suppressions (cppcheck's format doesn't
# allow inline comments). Current suppressions:
#   missingIncludeSystem -- cppcheck wants every <header> resolvable on
#                           its own include path; that's the compiler's
#                           job, not the analyzer's.
#   shiftNegativeLHS     -- Q16.16 arithmetic shift on signed int is the
#                           documented project convention (matches gcc/
#                           clang behavior and the Python references).
cppcheck:
	@which cppcheck >/dev/null || { echo "cppcheck not installed"; exit 1; }
	cppcheck --quiet --enable=warning,style,performance,portability \
	    --error-exitcode=1 \
	    --std=c99 \
	    --suppressions-list=.cppcheck-suppressions \
	    -I core \
	    core/*.c

clean:
	rm -rf build
	$(MAKE) -C platform/linux clean
