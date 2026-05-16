/* test/parity/canonical.h
 *
 * The canonical telemetry CSV projection used by Stage 15
 * cross-platform parity.  One row per core callback:
 *
 *     ts_ms,row_type,id,value,flags_or_status,a1,a2,a3,a4
 *
 *   - `S` (sample) rows  -- one per telemetry_emit() callback:
 *       id = signal_id, value = value, flags_or_status = flags
 *       (the telemetry_emit callback carries no flags, so the
 *       REPLAY rigs pass 0), a1..a4 = 0.
 *   - `E` (event) rows -- one per log_event() callback:
 *       id = event_code, value = 0, flags_or_status = 0,
 *       a1..a4 = the four event args.
 *
 * Decimal integers only, no whitespace, '\n' line endings.  The
 * host replay binary and the ESP32 REPLAY firmware both format
 * through this one serializer, so their byte streams -- hence
 * their SHA-256 -- match when the core behaves identically.
 */
#ifndef THERMALCORE_CANONICAL_H
#define THERMALCORE_CANONICAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The canonical CSV header line, including its trailing '\n'. */
extern const char THERMALCORE_CANONICAL_HEADER[];

/* Format one telemetry sample as a canonical `S` row into `buf`.
 * Returns the byte count written (excluding the NUL), or 0 if the
 * row did not fit in `cap`. */
int thermalcore_canonical_sample(char *buf, size_t cap,
                                 uint32_t ts_ms, uint16_t signal_id,
                                 int32_t value, uint16_t flags);

/* Format one event as a canonical `E` row into `buf`.  Returns the
 * byte count written (excluding the NUL), or 0 on overflow. */
int thermalcore_canonical_event(char *buf, size_t cap,
                                uint32_t ts_ms, uint16_t code,
                                uint32_t a1, uint32_t a2,
                                uint32_t a3, uint32_t a4);

#ifdef __cplusplus
}
#endif

#endif /* THERMALCORE_CANONICAL_H */
