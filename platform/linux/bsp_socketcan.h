/* platform/linux/bsp_socketcan.h
 *
 * Stage 11 11b — Linux SocketCAN BSP for the OBD-II vehicle_speed
 * context.  Wraps the portable codec from protocol/obd2.{c,h}
 * around a CAN_RAW socket on a configurable interface
 * (typically `vcan0` for bench tests, `can0` for real hardware
 * per PRD §6.2).
 *
 * Responsibilities split:
 *
 *   - bsp_socketcan_state_t + handle_frame() + read_into_sample()
 *     are pure functions over a passed-in state struct.  Unit
 *     tests instantiate the state on the stack and inject
 *     synthetic CAN frames; no sockets are opened.
 *
 *   - bsp_socketcan_open() / poll() / close() do the actual
 *     AF_CAN / SOCK_RAW plumbing inside bsp_socketcan.c.  Only
 *     thermalcored.c calls them.
 *
 * Header is intentionally free of `<linux/can.h>` — handle_frame
 * takes `(uint32_t can_id, const uint8_t *data, size_t len)`,
 * which is the same shape an ESP32 TWAI driver would invoke when
 * the same codec lands there in Stage 13.
 */
#ifndef THERMAL_BSP_SOCKETCAN_H
#define THERMAL_BSP_SOCKETCAN_H

#include <stddef.h>
#include <stdint.h>

#include "thermal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* === Cached state ========================================== */
/*
 * Updated by `bsp_socketcan_handle_frame` on every OBD-II OK
 * response.  Read by `bsp_socketcan_read_into_sample` once per
 * tick to populate the input snapshot for the core's
 * `acoustic_mask` modifier.
 */

typedef struct {
    uint32_t last_value_ms;   /* now_ms of the most recent OK frame */
    int32_t  last_value;      /* km/h on success, 0 otherwise */
    uint16_t timeout_ms;      /* PRD §6.3 fail-safe staleness window */
    uint8_t  expected_pid;    /* normally OBD2_PID_VEHICLE_SPEED */
    uint8_t  ever_received;   /* 0 until the first OK frame; gates
                                 freshness checks before the first
                                 response so an absent ECU is
                                 reported as stale rather than as
                                 a value-of-0 km/h. */
} bsp_socketcan_state_t;

/* Zero-initialise the cached state.  `timeout_ms` comes from
 * `thermal_context_cfg_t.timeout_ms` (PRD §6.3 default 3000 ms).
 * `expected_pid` is the OBD-II PID the BSP requested; only
 * responses echoing this PID update the cache. */
void bsp_socketcan_state_init(bsp_socketcan_state_t *state,
                              uint16_t timeout_ms,
                              uint8_t  expected_pid);

/* === Frame handler (pure function over state) ============== */
/*
 * Filter CAN frame by ID, decode via `obd2_decode_response_byte`,
 * and update the cache on success.  No-op when:
 *   - can_id != OBD2_RESPONSE_ID_ECU (frame is for someone else)
 *   - the decoder returns non-zero (BAD_SHAPE / NEGATIVE_RESPONSE
 *     / WRONG_SERVICE / WRONG_PID / WRONG_PCI_LEN)
 *
 * The BSP intentionally collapses every decoder failure mode into
 * "leave the cache alone, let the staleness window expire": from
 * the daemon's perspective an NRC is no fresher than no answer at
 * all.  The codec's distinct error codes are still useful for
 * future diagnostics (e.g. logging), they just don't change the
 * sample stream.
 */
void bsp_socketcan_handle_frame(bsp_socketcan_state_t *state,
                                uint32_t can_id,
                                const uint8_t *data, size_t len,
                                uint32_t now_ms);

/* === Sample read ========================================== */
/*
 * Fill `out` with the cached value + freshness bit.
 *
 *   - sample_id = `thermal_context_cfg_t.id` for the context slot
 *     this BSP fulfils.
 *   - valid=1 iff the BSP has received at least one OK frame AND
 *     `now_ms - last_value_ms <= timeout_ms`.
 *   - valid=0 on cold start (no frame yet) and after the timeout
 *     window expires.  Per PRD §6.3 the `acoustic_mask` modifier
 *     applies `fail_safe = assume_stationary` when this happens.
 *
 * Always returns 0; the valid bit carries the actual freshness
 * outcome.
 */
int bsp_socketcan_read_into_sample(const bsp_socketcan_state_t *state,
                                   uint16_t sample_id,
                                   uint32_t now_ms,
                                   thermal_sample_t *out);

/* === SocketCAN plumbing (Linux-only; daemon-side) ========== */
/*
 * `iface` is a kernel CAN interface name ("vcan0", "can0").
 * Returns a non-negative file descriptor on success, or -1 on
 * any failure (socket / SIOCGIFINDEX / bind).  Errors are
 * logged to stderr.  Caller owns the fd until `close`.
 */
int bsp_socketcan_open(const char *iface);

/* Per-tick poll.  Sends an OBD-II Service 01 request for
 * `state->expected_pid` to `OBD2_REQUEST_ID_FUNCTIONAL` (0x7DF)
 * whenever `now_ms >= *next_request_at_ms`, then drains all
 * pending response frames non-blockingly and routes each through
 * `bsp_socketcan_handle_frame`.
 *
 * Cadence: PRD §6.2 specifies 1 Hz polling.  Callers pass a
 * pointer to a uint32_t holding the next-due timestamp; the
 * function advances it by 1000 ms after each request.
 *
 * Returns 0 on success, -1 if the socket dies (caller should
 * close + reopen on next tick).
 */
int bsp_socketcan_poll(bsp_socketcan_state_t *state, int fd,
                       uint32_t now_ms,
                       uint32_t *next_request_at_ms);

/* Close the fd.  No-op for negative values. */
void bsp_socketcan_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_BSP_SOCKETCAN_H */
