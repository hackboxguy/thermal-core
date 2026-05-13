/* platform/esp32_idf/main/esp32_callbacks.c
 *
 * thermal_core_callbacks_t wiring.  See esp32_callbacks.h for
 * the contract.
 */
#include "esp32_callbacks.h"

#include <stdio.h>

#include "thermal_signals.h"
#include "thermal_events.h"

/* === Cached telemetry values (read by app_main) =========== */

#define ZONE_MAX     2   /* small upper bound; v1 ships 1 */
#define ACTUATOR_MAX 2

static int32_t s_zone_temp_mc[ZONE_MAX] = { INT32_MIN, INT32_MIN };
static int32_t s_actuator_pwm[ACTUATOR_MAX] = { INT32_MIN, INT32_MIN };

int32_t esp32_callbacks_last_zone_temp_mc(uint8_t zone)
{
    if (zone >= ZONE_MAX) return INT32_MIN;
    return s_zone_temp_mc[zone];
}

int32_t esp32_callbacks_last_actuator_pwm(uint8_t slot)
{
    if (slot >= ACTUATOR_MAX) return INT32_MIN;
    return s_actuator_pwm[slot];
}

/* === Callbacks ============================================ */

void esp32_telemetry_emit_cb(uint32_t ts_ms, uint16_t signal_id,
                              int32_t value)
{
    (void)ts_ms;
    /* Decode the signal-ID namespace per core/thermal_signals.h.
     * Only zone-temp + actuator-pwm are interesting to cache for
     * the status line; everything else is silently dropped. */
    if (signal_id >= TSIG_ZONE_BASE &&
        signal_id <  TSIG_ZONE_BASE + TSIG_RANGE_SIZE) {
        uint16_t off  = signal_id - TSIG_ZONE_BASE;
        uint8_t  sub  = (uint8_t)(off & 0xF0u);
        uint8_t  slot = (uint8_t)(off & 0x0Fu);
        if (sub == TSIG_ZONE_SUB_TEMP && slot < ZONE_MAX) {
            s_zone_temp_mc[slot] = value;
        }
        return;
    }
    if (signal_id >= TSIG_ACTUATOR_BASE &&
        signal_id <  TSIG_ACTUATOR_BASE + TSIG_RANGE_SIZE) {
        uint16_t off  = signal_id - TSIG_ACTUATOR_BASE;
        uint8_t  sub  = (uint8_t)(off & 0xF0u);
        uint8_t  slot = (uint8_t)(off & 0x0Fu);
        if (sub == TSIG_ACTUATOR_SUB_DUTY && slot < ACTUATOR_MAX) {
            s_actuator_pwm[slot] = value;
        }
        return;
    }
}

void esp32_log_event_cb(uint32_t ts_ms, uint16_t code,
                        uint32_t a1, uint32_t a2,
                        uint32_t a3, uint32_t a4)
{
    /* Events are rare; print immediately so the operator sees
     * fault / command transitions in the live log.  TEVENT_*
     * codes are documented in core/thermal_events.h. */
    printf("[event %u ms] code=0x%04x a=(%u,%u,%u,%u)\n",
            (unsigned)ts_ms, (unsigned)code,
            (unsigned)a1, (unsigned)a2,
            (unsigned)a3, (unsigned)a4);
}
