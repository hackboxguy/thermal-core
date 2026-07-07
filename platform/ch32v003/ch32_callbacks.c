/* platform/ch32v003/ch32_callbacks.c
 *
 * Stage 18e -- thermal_core_callbacks_t wiring for the CH32V003.
 * See ch32_callbacks.h.
 *
 * The core's telemetry_emit / log_event callbacks run inside
 * thermal_core_step() and must return promptly (PRD callback
 * contract -- platforms buffer/queue/drop, they do not block on a
 * sink).  So a callback only enqueues a compact record into a ring
 * buffer; ch32_telemetry_drain() -- called from the main loop after
 * the core step -- does the canonical formatting (the shared
 * test/parity/canonical.c serializer) and the blocking USART1
 * writes, keeping both out of the core's bounded work path.
 */
#include "ch32_callbacks.h"

#if THERMALCORE_CH32_TELEMETRY

#include "canonical.h"
#include "bsp_ch32_uart.h"
#include "thermal_config.h"
#include "thermal_signals.h"

/* Ring of pending telemetry records. thermal_core_step() emits every
 * enabled telemetry signal in one in-step burst before returning, and
 * this main-loop ring is the platform's buffer in front of the
 * blocking UART. Sizing rule: the ring must hold the entire per-tick
 * burst -- otherwise the tail signals (specifically the higher-id
 * fan_health_* records under Stage 20's signal selector) get
 * silently dropped before the post-step drain runs. 16 slots (15
 * usable) covers the tiny-profile max of 12 enabled signals plus a
 * small event-burst margin. The static assert below pins the
 * invariant, and the platform's RAM headroom comfortably fits the
 * record array. On overflow the incoming record is still dropped
 * (the PRD permits coalesce/drop); the static assert just keeps the
 * sized-for-the-config case overflow-free. */
#define CH32_TELEM_RING 16
_Static_assert(CH32_TELEM_RING > THERMAL_MAX_TELEMETRY_SIGNALS,
               "CH32_TELEM_RING must hold the full per-tick signal "
               "burst with room for events; raise it if you raise "
               "THERMAL_MAX_TELEMETRY_SIGNALS");

typedef struct {
    uint32_t ts_ms;
    uint16_t id;          /* signal_id (sample) or event code */
    uint8_t  is_event;
    int32_t  arg0;        /* sample value, or event arg a1 */
    uint32_t arg1, arg2, arg3;   /* event args a2..a4 (0 for samples) */
} ch32_telem_rec_t;

static ch32_telem_rec_t s_ring[CH32_TELEM_RING];
static uint8_t s_ring_head;   /* next slot to write */
static uint8_t s_ring_tail;   /* next slot to read */
static uint32_t s_ring_drops;
static uint32_t s_ring_drops_reported;
static uint32_t s_ring_last_drop_ts_ms;

static uint8_t ring_next(uint8_t i)
{
    return (uint8_t)((i + 1u) % CH32_TELEM_RING);
}

static void ring_push(const ch32_telem_rec_t *rec)
{
    uint8_t next = ring_next(s_ring_head);
    if (next == s_ring_tail) {
        if (s_ring_drops < 0xffffffffu) {
            s_ring_drops++;
        }
        s_ring_last_drop_ts_ms = rec->ts_ms;
        return;   /* full: drop (PRD permits) */
    }
    s_ring[s_ring_head] = *rec;
    s_ring_head = next;
}

static int32_t drop_count_value(void)
{
    return (s_ring_drops > 0x7fffffffu)
             ? (int32_t)0x7fffffff
             : (int32_t)s_ring_drops;
}

void ch32_log_event_cb(uint32_t ts_ms, uint16_t code,
                       uint32_t a1, uint32_t a2,
                       uint32_t a3, uint32_t a4)
{
    ch32_telem_rec_t rec;
    rec.ts_ms    = ts_ms;
    rec.id       = code;
    rec.is_event = 1;
    rec.arg0     = (int32_t)a1;
    rec.arg1     = a2;
    rec.arg2     = a3;
    rec.arg3     = a4;
    ring_push(&rec);
}

void ch32_telemetry_emit_cb(uint32_t ts_ms, uint16_t signal_id,
                            int32_t value)
{
    ch32_telem_rec_t rec;
    rec.ts_ms    = ts_ms;
    rec.id       = signal_id;
    rec.is_event = 0;
    rec.arg0     = value;
    rec.arg1     = 0;
    rec.arg2     = 0;
    rec.arg3     = 0;
    ring_push(&rec);
}

uint32_t ch32_telemetry_drop_count(void)
{
    return s_ring_drops;
}

void ch32_telemetry_drain(void)
{
    while (s_ring_tail != s_ring_head) {
        const ch32_telem_rec_t *rec = &s_ring[s_ring_tail];
        char buf[128];
        int  n;
        if (rec->is_event) {
            n = thermalcore_canonical_event(buf, sizeof buf, rec->ts_ms,
                                            rec->id, (uint32_t)rec->arg0,
                                            rec->arg1, rec->arg2, rec->arg3);
        } else {
            n = thermalcore_canonical_sample(buf, sizeof buf, rec->ts_ms,
                                             rec->id, rec->arg0, 0);
        }
        if (n > 0) {
            bsp_ch32_uart_puts(buf);
        }
        s_ring_tail = ring_next(s_ring_tail);
    }
    if (s_ring_drops != s_ring_drops_reported) {
        char buf[128];
        int n = thermalcore_canonical_sample(
                    buf, sizeof buf, s_ring_last_drop_ts_ms,
                    TSIG_PLATFORM_CH32_TELEMETRY_DROPS,
                    drop_count_value(), 0);
        if (n > 0) {
            bsp_ch32_uart_puts(buf);
            s_ring_drops_reported = s_ring_drops;
        }
    }
}

#else  /* telemetry build disabled: headless STANDALONE */

void ch32_log_event_cb(uint32_t ts_ms, uint16_t code,
                       uint32_t a1, uint32_t a2,
                       uint32_t a3, uint32_t a4)
{
    (void)ts_ms; (void)code;
    (void)a1; (void)a2; (void)a3; (void)a4;
}

uint32_t ch32_telemetry_drop_count(void)
{
    return 0;
}

#endif /* THERMALCORE_CH32_TELEMETRY */
