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

/* Ring of pending telemetry records.  One tick emits 3 signal
 * samples plus the occasional event, and the main loop drains
 * every tick, so 8 slots (7 usable) never overflow in practice;
 * on overflow the incoming record is dropped (the PRD permits
 * coalesce/drop). */
#define CH32_TELEM_RING 8

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

static uint8_t ring_next(uint8_t i)
{
    return (uint8_t)((i + 1u) % CH32_TELEM_RING);
}

static void ring_push(const ch32_telem_rec_t *rec)
{
    uint8_t next = ring_next(s_ring_head);
    if (next == s_ring_tail) {
        return;   /* full: drop (PRD permits) */
    }
    s_ring[s_ring_head] = *rec;
    s_ring_head = next;
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
}

#else  /* telemetry build disabled: headless STANDALONE */

void ch32_log_event_cb(uint32_t ts_ms, uint16_t code,
                       uint32_t a1, uint32_t a2,
                       uint32_t a3, uint32_t a4)
{
    (void)ts_ms; (void)code;
    (void)a1; (void)a2; (void)a3; (void)a4;
}

#endif /* THERMALCORE_CH32_TELEMETRY */
