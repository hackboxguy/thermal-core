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

# --- Protocol archive (Stage 10 10a) ---
# Portable C99 wire codec; depends only on core/thermal_commands.h.
# Not linked into anything in 10a -- the unit test compiles its own
# copy directly.  10b switches the daemon to use this archive.
PROTOCOL_SRCS    = $(wildcard protocol/*.c)
PROTOCOL_OBJS    = $(patsubst protocol/%.c,build/protocol/%.o,$(PROTOCOL_SRCS))
PROTOCOL_ARCHIVE = build/protocol/libthermal_protocol.a

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

.PHONY: all test build verify-portability replay regen-replay-goldens property property-command asan clang-tidy cppcheck smoke integration integration-can scenario determinism fuzz-json fuzz-wire coverage build-esp32 clean

all: test build verify-portability replay property property-command smoke integration

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
    platform/linux/runtime_cfg.h \
    core/thermal_config.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -I platform/linux \
	    -o $@ test/unit/test_config_jsmn.c \
	    platform/linux/config_jsmn.c platform/linux/jsmn.c \
	    $(CORE_ARCHIVE) $(LDFLAGS_EXTRA)

# --- Special: runtime cfg load test (same shape as test_config_jsmn) ---
build/test/test_runtime_cfg_load: \
    test/unit/test_runtime_cfg_load.c test/unit/harness.h \
    platform/linux/config_jsmn.c platform/linux/config_jsmn.h \
    platform/linux/jsmn.c platform/linux/jsmn.h \
    platform/linux/runtime_cfg.h \
    core/thermal_config.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -I platform/linux \
	    -o $@ test/unit/test_runtime_cfg_load.c \
	    platform/linux/config_jsmn.c platform/linux/jsmn.c \
	    $(CORE_ARCHIVE) $(LDFLAGS_EXTRA)

# --- Special: bsp_mock_tmpfs test ---
build/test/test_bsp_mock_tmpfs: \
    test/unit/test_bsp_mock_tmpfs.c test/unit/harness.h \
    platform/linux/bsp_mock_tmpfs.c platform/linux/bsp_mock_tmpfs.h \
    platform/linux/runtime_cfg.h \
    core/thermal_config.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -I platform/linux \
	    -o $@ test/unit/test_bsp_mock_tmpfs.c \
	    platform/linux/bsp_mock_tmpfs.c \
	    $(CORE_ARCHIVE) $(LDFLAGS_EXTRA)

# --- Special: protocol/thermal_wire round-trip + CRC + cap test ---
build/test/test_thermal_wire: \
    test/unit/test_thermal_wire.c test/unit/harness.h \
    protocol/thermal_wire.c protocol/thermal_wire.h \
    protocol/thermal_wire_opcodes.h \
    core/thermal_commands.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -I protocol \
	    -o $@ test/unit/test_thermal_wire.c \
	    protocol/thermal_wire.c \
	    $(CORE_ARCHIVE) $(LDFLAGS_EXTRA)

# --- Special: protocol/obd2 OBD-II PID 0x0D encode/decode test (Stage 11 11a) ---
build/test/test_obd2: \
    test/unit/test_obd2.c test/unit/harness.h \
    protocol/obd2.c protocol/obd2.h
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -I protocol \
	    -o $@ test/unit/test_obd2.c \
	    protocol/obd2.c $(LDFLAGS_EXTRA)

# --- Special: bsp_socketcan synthetic-frame BSP test (Stage 11 11b) ---
# Links the Linux BSP module + the portable codec.  No socket I/O
# in the unit test itself; bsp_socketcan_handle_frame is driven
# directly with byte arrays.
build/test/test_bsp_socketcan: \
    test/unit/test_bsp_socketcan.c test/unit/harness.h \
    platform/linux/bsp_socketcan.c platform/linux/bsp_socketcan.h \
    protocol/obd2.c protocol/obd2.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -I platform/linux -I protocol \
	    -o $@ test/unit/test_bsp_socketcan.c \
	    platform/linux/bsp_socketcan.c \
	    protocol/obd2.c \
	    $(CORE_ARCHIVE) $(LDFLAGS_EXTRA)

# --- Special: thermal-plant simulator unit test (Stage 12 12a) ---
# Plant lives at tools/thermalcore-scenario/plant.{c,h}; depends on
# core/thermal_curve.c (via thermal_curve_eval_y0) which is already
# in the core archive.  Plant object is built with -fPIC so 12b's
# Python runner can link it into a shared library for ctypes.
build/tools/thermalcore-scenario/plant.o: \
    tools/thermalcore-scenario/plant.c \
    tools/thermalcore-scenario/plant.h \
    core/thermal_curve.h core/thermal_types.h
	@mkdir -p build/tools/thermalcore-scenario
	$(CC) $(CFLAGS_BASE) -I tools/thermalcore-scenario -fPIC \
	    -c -o $@ tools/thermalcore-scenario/plant.c

build/test/test_plant: \
    test/unit/test_plant.c test/unit/harness.h \
    tools/thermalcore-scenario/plant.c \
    tools/thermalcore-scenario/plant.h \
    core/thermal_curve.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -I tools/thermalcore-scenario \
	    -o $@ test/unit/test_plant.c \
	    tools/thermalcore-scenario/plant.c \
	    $(CORE_ARCHIVE) $(LDFLAGS_EXTRA)

# --- libplant.so for ctypes consumption (Stage 12 12b) ----
# Shared library version of the plant; loaded by
# tools/thermalcore-scenario/plant_ffi.py.  Reuses the -fPIC
# plant.o that the test_plant rule already builds, plus the
# core archive for thermal_curve_eval_y0.
build/tools/thermalcore-scenario/libplant.so: \
    build/tools/thermalcore-scenario/plant.o $(CORE_ARCHIVE)
	@mkdir -p build/tools/thermalcore-scenario
	$(CC) -shared -o $@ \
	    build/tools/thermalcore-scenario/plant.o \
	    $(CORE_ARCHIVE)

# --- Scenario runner: drives the daemon under --clock=scenario
# against the deterministic plant (Stage 12 12b).
# Not folded into `make all` -- it spawns subprocesses + sockets.
# 12c adds the CI job that runs it.
SCENARIO_FILES = $(wildcard scenarios/*.scn)

# Stage 12 12c: iterate all canonical scenarios (each carries its
# own `config <path>` directive so no manifest is needed).
scenario: build build/tools/thermalcore-scenario/libplant.so \
          tools/thermalcore-scenario/run.py \
          tools/thermalcore-scenario/plant_ffi.py \
          tools/thermalcore-scenario/scenario.py \
          tools/thermalcore-probe \
          $(SCENARIO_FILES)
	@set -e; for s in $(SCENARIO_FILES); do \
	    echo "=== $$s ==="; \
	    python3 tools/thermalcore-scenario/run.py $$s; \
	done
	@echo "scenario: ALL PASS"

# --- Determinism gate (Stage 12 12d, v1 Linux release-gate) ---
# Same-build run-twice + gcc-vs-clang SHA-256 over the captured
# telemetry CSVs.  Q16.16 plant + --clock=scenario daemon +
# byte-stable CSV from the probe (12b) together promise
# byte-equal output across runs and across compilers.
determinism: build build/tools/thermalcore-scenario/libplant.so \
             tools/thermalcore-scenario/check_determinism.py \
             tools/thermalcore-scenario/run.py \
             $(SCENARIO_FILES)
	@python3 tools/thermalcore-scenario/check_determinism.py --twice
	@python3 tools/thermalcore-scenario/check_determinism.py --cross-compiler

# --- Special: canonical config hash test (sha256 + encoder + padding poison) ---
build/test/test_config_hash: \
    test/unit/test_config_hash.c test/unit/harness.h \
    support/sha256.c support/sha256.h \
    support/thermal_config_hash.c support/thermal_config_hash.h \
    core/thermal_config.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -I support \
	    -o $@ test/unit/test_config_hash.c \
	    support/sha256.c support/thermal_config_hash.c \
	    $(CORE_ARCHIVE) $(LDFLAGS_EXTRA)

# --- Generated static config (json2static.py) ---
build/test/generated/minimal_static.c: \
    configs/minimal-1zone-1fan.json tools/json2static.py
	@mkdir -p build/test/generated
	python3 tools/json2static.py -o $@ configs/minimal-1zone-1fan.json

# --- Special: json2static round-trip test ---
build/test/test_json2static_roundtrip: \
    test/unit/test_json2static_roundtrip.c test/unit/harness.h \
    build/test/generated/minimal_static.c \
    platform/linux/config_jsmn.c platform/linux/config_jsmn.h \
    platform/linux/jsmn.c platform/linux/jsmn.h \
    platform/linux/runtime_cfg.h \
    support/sha256.c support/thermal_config_hash.c support/thermal_config_hash.h \
    core/thermal_config.h $(CORE_ARCHIVE)
	@mkdir -p build/test
	$(CC) $(CFLAGS_BASE) -I platform/linux -I support \
	    -o $@ test/unit/test_json2static_roundtrip.c \
	    build/test/generated/minimal_static.c \
	    platform/linux/config_jsmn.c platform/linux/jsmn.c \
	    support/sha256.c support/thermal_config_hash.c \
	    $(CORE_ARCHIVE) $(LDFLAGS_EXTRA)

# --- Smoke: spawn thermalcored + drive ticks + verify telemetry ---
smoke: build test/smoke/test_thermalcored_smoke.py test/smoke/smoke-config.json
	@python3 test/smoke/test_thermalcored_smoke.py

# --- Integration: spawn thermalcored + drive tools/thermalcore-tune ---
# Stage 10 10c: exercises all five command subcommands end-to-end.
# Codex v7 carryover adds the PID-positive tuning script (set-pid
# soc 0 0 0 -> ACK + integral reset + zero output).  Both scripts run
# back-to-back; the second waits on a free 9012 control port so they
# do not race.
integration: build test/integration/test_thermalcore_tune.py \
             test/integration/test_thermalcore_tune_pid.py \
             test/integration/test_hil_serial.py \
             test/integration/pid-config.json \
             tools/thermalcore-tune tools/thermalcore_wire.py \
             test/smoke/smoke-config.json
	@python3 test/integration/test_thermalcore_tune.py
	@python3 test/integration/test_thermalcore_tune_pid.py
	@python3 test/integration/test_hil_serial.py

# --- Integration-can: vcan0 + car-can-emulator hardware loop -------
# Stage 11 11c: spawns car-can-emulator on vcan0, drives speed via
# TCP, asserts the daemon's TSIG_CONTEXT_VALUE_0 telemetry converges.
# The Python harness skips cleanly when vcan0 is unavailable so
# `make integration-can` is a no-op on dev machines without vcan.
# On the canonical CI (ubuntu-latest with CAP_NET_ADMIN), the
# harness runs for real -- a SKIP there blocks the Stage 11 exit
# gate per impl-plan section 5.

build/car-can-emulator/car-can-emulator: tools/car-can-emulator/CMakeLists.txt
	@mkdir -p build/car-can-emulator
	cmake -S tools/car-can-emulator -B build/car-can-emulator
	cmake --build build/car-can-emulator

integration-can: build build/car-can-emulator/car-can-emulator \
                 test/integration/test_canbus_obd2.py \
                 test/integration/test_canbus_busloss.py \
                 test/integration/canbus-config.json
	@python3 test/integration/test_canbus_obd2.py
	@python3 test/integration/test_canbus_busloss.py

# --- Fuzz: libFuzzer over the JSON loader (needs clang) ---
# Not part of `make all` -- it runs for 60 s.  CI runs it as a
# dedicated job.  Build the harness with clang + fuzzer + ASan.
#
# The growing corpus + crash artifacts go to build/fuzz/corpus and
# build/fuzz/artifacts respectively so test/fuzz/seeds/ stays
# pristine and committable.  libFuzzer reads seeds AND adds new
# discoveries to the first positional dir; the seed dir is read-only
# in this layout.
FUZZ_CC ?= clang
fuzz-json: build/fuzz/fuzz_jsmn
	@mkdir -p build/fuzz/corpus build/fuzz/artifacts
	@build/fuzz/fuzz_jsmn -max_total_time=60 \
	    -artifact_prefix=build/fuzz/artifacts/ \
	    build/fuzz/corpus/ test/fuzz/seeds/

# Per-fuzz sanitized core archive.  fuzz_jsmn would otherwise reach
# thermal_core_validate_config (called by the loader) through the
# non-sanitized $(CORE_ARCHIVE), which silenced OOB/UB detection in
# core/ for fuzz inputs.  The core objects are compiled with
# `-fsanitize=fuzzer-no-link,address,undefined` so SanitizerCoverage
# + ASan/UBSan apply but libFuzzer's `main` isn't pulled in twice
# (the final fuzz_jsmn link supplies it via `-fsanitize=fuzzer`).
FUZZ_CFLAGS_BASE = -std=c99 -Wall -Wextra -Werror -pedantic \
                   -I core -I test/unit \
                   -O1 -g -fno-omit-frame-pointer \
                   -fsanitize=fuzzer-no-link,address,undefined
FUZZ_CORE_OBJS    = $(patsubst core/%.c,build/fuzz/core/%.o,$(CORE_SRCS))
FUZZ_CORE_ARCHIVE = build/fuzz/core/libthermal_core.a

build/fuzz/core/%.o: core/%.c core/thermal_config.h
	@mkdir -p build/fuzz/core
	$(FUZZ_CC) $(FUZZ_CFLAGS_BASE) -c -o $@ $<

$(FUZZ_CORE_ARCHIVE): $(FUZZ_CORE_OBJS)
	@mkdir -p build/fuzz/core
	ar rcs $@ $^

build/fuzz/fuzz_jsmn: \
    test/fuzz/fuzz_jsmn.c \
    platform/linux/config_jsmn.c platform/linux/config_jsmn.h \
    platform/linux/jsmn.c platform/linux/jsmn.h \
    platform/linux/runtime_cfg.h \
    core/thermal_config.h $(FUZZ_CORE_ARCHIVE)
	@mkdir -p build/fuzz
	$(FUZZ_CC) -std=c99 -O1 -g \
	    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	    -I core -I test/unit -I platform/linux \
	    -o $@ test/fuzz/fuzz_jsmn.c \
	    platform/linux/config_jsmn.c platform/linux/jsmn.c \
	    $(FUZZ_CORE_ARCHIVE)

# --- Fuzz-wire: libFuzzer over protocol/thermal_wire.c (Stage 10 10d) ----
# Same shape as fuzz-json: protocol/ is compiled with $(FUZZ_CC)
# -fsanitize=fuzzer-no-link,address,undefined into a per-fuzz archive
# so SanitizerCoverage + ASan / UBSan apply to the decoder.  The final
# fuzz_wire link adds -fsanitize=fuzzer for libFuzzer's main.
FUZZ_PROTO_CFLAGS = -std=c99 -Wall -Wextra -Werror -pedantic \
                    -I core -I protocol \
                    -O1 -g -fno-omit-frame-pointer \
                    -fsanitize=fuzzer-no-link,address,undefined
FUZZ_PROTO_OBJS    = build/fuzz/protocol/thermal_wire.o
FUZZ_PROTO_ARCHIVE = build/fuzz/protocol/libthermal_protocol.a

build/fuzz/protocol/%.o: protocol/%.c $(wildcard protocol/*.h) \
                         core/thermal_commands.h
	@mkdir -p build/fuzz/protocol
	$(FUZZ_CC) $(FUZZ_PROTO_CFLAGS) -c -o $@ $<

$(FUZZ_PROTO_ARCHIVE): $(FUZZ_PROTO_OBJS)
	@mkdir -p build/fuzz/protocol
	ar rcs $@ $^

fuzz-wire: build/fuzz/fuzz_wire
	@mkdir -p build/fuzz/wire-corpus build/fuzz/wire-artifacts
	@build/fuzz/fuzz_wire -max_total_time=60 \
	    -artifact_prefix=build/fuzz/wire-artifacts/ \
	    build/fuzz/wire-corpus/ test/fuzz/wire-seeds/

build/fuzz/fuzz_wire: \
    test/fuzz/fuzz_wire.c \
    protocol/thermal_wire.h protocol/thermal_wire_opcodes.h \
    core/thermal_commands.h $(FUZZ_PROTO_ARCHIVE)
	@mkdir -p build/fuzz
	$(FUZZ_CC) -std=c99 -O1 -g \
	    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	    -I core -I protocol \
	    -o $@ test/fuzz/fuzz_wire.c \
	    $(FUZZ_PROTO_ARCHIVE)

# --- Core archive ---
build/core/%.o: core/%.c core/thermal_config.h
	@mkdir -p build/core
	$(CC) $(CFLAGS_BASE) -c -o $@ $<

$(CORE_ARCHIVE): $(CORE_OBJS)
	@mkdir -p build/core
	ar rcs $@ $^

# --- Protocol archive (Stage 10 10a; protocol/*.h wildcarded in 11a) ---
build/protocol/%.o: protocol/%.c $(wildcard protocol/*.h) \
                    core/thermal_commands.h
	@mkdir -p build/protocol
	$(CC) $(CFLAGS_BASE) -I protocol -c -o $@ $<

$(PROTOCOL_ARCHIVE): $(PROTOCOL_OBJS)
	@mkdir -p build/protocol
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

# --- Stage 15 cross-platform parity: host replay binary ---
# Host side of replay-parity.  Compiles the shared test/parity/
# sources against the json2static-generated G_THERMAL_CFG of
# esp32-c3-standalone.json -- the identical const config the
# ESP32-C3 REPLAY firmware builds.  -I platform/esp32_idf/main is
# for esp32_pinmap.h, which the generated config #includes.
PARITY_DIR        = build/parity
PARITY_HOST       = $(PARITY_DIR)/replay_host
PARITY_CONFIG_C   = $(PARITY_DIR)/esp32_config.c
ESP32_CONFIG_JSON = platform/esp32_idf/configs/esp32-c3-standalone.json
PARITY_SRCS       = test/parity/replay_host.c test/parity/replay_run.c \
                    test/parity/replay_fixture.c test/parity/canonical.c

$(PARITY_CONFIG_C): $(ESP32_CONFIG_JSON) tools/json2static.py
	@mkdir -p $(PARITY_DIR)
	python3 tools/json2static.py -o $@ $(ESP32_CONFIG_JSON) \
	    --symbol G_THERMAL_CFG

$(PARITY_HOST): $(PARITY_SRCS) $(PARITY_CONFIG_C) $(CORE_ARCHIVE) \
                test/parity/canonical.h test/parity/replay_fixture.h \
                test/parity/replay_run.h
	@mkdir -p $(PARITY_DIR)
	$(CC) $(CFLAGS_BASE) -I test/parity -I platform/esp32_idf/main \
	    -o $@ $(PARITY_SRCS) $(PARITY_CONFIG_C) $(CORE_ARCHIVE)

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

# --- Coverage (impl-plan §3 row 9 / §5 Stage 9). ----
# Rebuilds test + replay stack with --coverage, captures via lcov,
# generates HTML at build/coverage/html/.  Visibility only -- the
# CI job runs `continue-on-error: true` so this never gates a PR.
# After capturing, removes the coverage-instrumented .o + .gc*
# build artifacts so a subsequent `make all` relinks against
# fresh uninstrumented objects (same intent as the `asan` target,
# but the build/coverage/ HTML report has to survive).
COVERAGE_DIR = build/coverage
coverage:
	@which lcov >/dev/null || { echo "lcov not installed"; exit 1; }
	@which genhtml >/dev/null || { echo "genhtml not installed"; exit 1; }
	$(MAKE) clean
	$(MAKE) test \
	    CFLAGS_EXTRA="--coverage -O0 -g" \
	    LDFLAGS_EXTRA="--coverage"
	$(MAKE) replay \
	    CFLAGS_EXTRA="--coverage -O0 -g" \
	    LDFLAGS_EXTRA="--coverage"
	@mkdir -p $(COVERAGE_DIR)
	# `--ignore-errors mismatch,inconsistent` works around a known
	# lcov 2.x quirk with gcov 13's main-line accounting on
	# `TEST_CASE` expansions (harmless line-number drift).
	lcov --capture --directory build/ \
	    --base-directory . --no-external \
	    --ignore-errors mismatch,inconsistent \
	    --output-file $(COVERAGE_DIR)/coverage.info
	genhtml --quiet --ignore-errors inconsistent \
	    $(COVERAGE_DIR)/coverage.info \
	    --output-directory $(COVERAGE_DIR)/html
	@echo "Coverage HTML at $(COVERAGE_DIR)/html/index.html"
	@echo "--- Cleaning coverage-instrumented artifacts (preserves $(COVERAGE_DIR)) ---"
	@rm -rf build/core build/test build/replay build/property build/platform-linux build/fuzz
	@$(MAKE) -C platform/linux clean >/dev/null 2>&1 || true

# --- Build (delegates to platform/linux) ---
# Depend on the core archive explicitly so `make clean && make build`
# works without first running `make test` -- the platform makefile
# has a deliberate-failure rule when the archive is missing, but the
# orchestration belongs at this level.
build: $(CORE_ARCHIVE)
	$(MAKE) -C platform/linux CC=$(CC)

# --- clang-tidy on core/ + platform/linux/ (Stage 9 9c scope extension).
# Config in .clang-tidy. ---
# platform/linux/jsmn.c is vendored zserge code; clang-tidy is run
# against it but its findings are not the project's to fix.  If/when
# vendored noise appears, file targeted nolint comments in jsmn.c
# itself with a citation back to the upstream commit.
clang-tidy:
	@which clang-tidy >/dev/null || { echo "clang-tidy not installed"; exit 1; }
	clang-tidy --quiet core/*.c platform/linux/*.c support/*.c protocol/*.c \
	    tools/thermalcore-scenario/*.c -- \
	    $(CFLAGS_BASE) -I platform/linux -I support -I protocol

# --- cppcheck on core/ + platform/linux/ (Stage 9 9c scope extension).
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
	    -I core -I platform/linux -I support -I protocol \
	        -I tools/thermalcore-scenario \
	    core/*.c platform/linux/*.c support/*.c protocol/*.c \
	    tools/thermalcore-scenario/*.c

# --- ESP32 firmware build matrix (Stage 13 13d, local mirror of CI) ---
# Builds the firmware in both modes (STANDALONE + REPLAY_STANDALONE)
# and enforces the PRD section 9.2 size budget on the STANDALONE
# image's core/+protocol/ contribution.  Not wired into `all`: it
# requires a local ESP-IDF install; Linux-only devs skip it.
#
# Override the IDF location with ESP_IDF_PATH if your install lives
# somewhere other than ~/esp/esp-idf.
ESP_IDF_PATH ?= $(HOME)/esp/esp-idf
build-esp32:
	@if [ ! -f "$(ESP_IDF_PATH)/export.sh" ]; then \
	    echo "SKIP: ESP-IDF not found at $(ESP_IDF_PATH); set ESP_IDF_PATH to override"; \
	    exit 0; \
	fi
	@echo "--- build-esp32: STANDALONE ---"
	@bash -c '. "$(ESP_IDF_PATH)/export.sh" && \
	    cd platform/esp32_idf && \
	    idf.py fullclean >/dev/null && \
	    idf.py build && \
	    idf.py size-files --format json \
	        --output-file build/size-files.json'
	python3 tools/check_esp32_size_budget.py \
	    platform/esp32_idf/build/size-files.json
	@echo "--- build-esp32: REPLAY_STANDALONE ---"
	@bash -c '. "$(ESP_IDF_PATH)/export.sh" && \
	    cd platform/esp32_idf && \
	    idf.py fullclean >/dev/null && \
	    idf.py -DTHERMALCORE_REPLAY_STANDALONE=ON build'
	@echo "--- build-esp32: HIL_PERIPHERAL ---"
	@bash -c '. "$(ESP_IDF_PATH)/export.sh" && \
	    cd platform/esp32_idf && \
	    idf.py fullclean >/dev/null && \
	    idf.py -DTHERMALCORE_HIL_PERIPHERAL=ON build && \
	    idf.py size-files --format json \
	        --output-file build/size-files.json'
	python3 tools/check_esp32_size_budget.py \
	    platform/esp32_idf/build/size-files.json

clean:
	rm -rf build
	$(MAKE) -C platform/linux clean
