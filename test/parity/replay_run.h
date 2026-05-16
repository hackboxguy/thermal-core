/* test/parity/replay_run.h
 *
 * The shared Stage 15 replay loop.  Both the host replay binary
 * (test/parity/replay_host.c) and the ESP32-C3 REPLAY firmware
 * (platform/esp32_idf/main/main.c) call replay_run(), so the
 * snapshot-building and stepping code is defined exactly once --
 * the parity test cannot be defeated by the two rigs drifting
 * apart in glue code.
 */
#ifndef REPLAY_RUN_H
#define REPLAY_RUN_H

#include "thermal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Drive `core` (already thermal_core_init'd with the platform's
 * telemetry/event callbacks) through the whole replay fixture.
 * Emits the canonical CSV header, the per-callback data rows (via
 * the callbacks), and a trailing "END\n" line, all on stdout. */
void replay_run(thermal_core_t *core, const thermal_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_RUN_H */
