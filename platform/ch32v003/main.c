/* platform/ch32v003/main.c
 *
 * Stage 18 -- thermal-core STANDALONE on the WCH CH32V003.
 *
 * The portable core/ (built under the tiny profile, PRD Appendix
 * D.3) runs the control law on-chip with no host: a DS18B20 feeds
 * thermal_core_step(), which drives a 4-wire fan over PWM with tach
 * readback. Same shape as the ESP32 STANDALONE app_main, trimmed to
 * the single mode this port supports (PRD Appendix D.5).
 *
 * Pacing: each tick is delimited by a SysTick window of
 * control_period_ms. The DS18B20 conversion wait (~800 ms, blocking
 * inside the sensor BSP) is absorbed within the window, so the
 * effective control period stays control_period_ms as long as that
 * exceeds the conversion time -- the config sets it to 1000 ms.
 */
#include "ch32fun.h"

#include <stdint.h>
#include <string.h>

#include "thermal_core.h"
#include "thermal_config.h"
#include "ch32_pinmap.h"
#include "ch32_callbacks.h"
#include "bsp_ch32_pwm.h"
#include "bsp_ch32_tach.h"
#include "bsp_ch32_sensor.h"

/* Optional once-per-tick status line over the SWIO debug channel
 * (PRD Appendix D.2 -- the only readout on this headless board).
 * Build with -DTHERMALCORE_CH32_STATUS=0 to drop it (and ch32fun's
 * printf, ~1.2 KB) on a flash-critical build. */
#ifndef THERMALCORE_CH32_STATUS
#define THERMALCORE_CH32_STATUS 1
#endif
#if THERMALCORE_CH32_STATUS
#include <stdio.h>
#endif

/* Optional canonical-CSV telemetry tap over USART1 (Stage 18e),
 * compile-gated by THERMALCORE_CH32_TELEMETRY (ch32_callbacks.h).
 * Independent of the SWIO status line above -- different transport. */
#if THERMALCORE_CH32_TELEMETRY
#include "bsp_ch32_uart.h"
#include "canonical.h"
#endif

/* Emitted by json2static.py from configs/ch32v003-standalone.json. */
extern const thermal_config_t G_THERMAL_CFG;
extern const ch32_pinmap_t    G_CH32_PINMAP;

int main(void)
{
    SystemInit();
    Delay_Ms(100);

    /* Bring up the BSPs from the JSON-driven pin map. */
    for (uint8_t i = 0; i < G_CH32_PINMAP.actuator_count; i++) {
        bsp_ch32_pwm_init(i, G_CH32_PINMAP.actuators[i].pwm_gpio,
                          G_CH32_PINMAP.actuators[i].pwm_freq_hz);
        bsp_ch32_tach_init(i, G_CH32_PINMAP.actuators[i].tach_gpio);
    }
    if (G_CH32_PINMAP.sensor_count > 0) {
        bsp_ch32_sensor_init(G_CH32_PINMAP.sensors[0].onewire_gpio,
                             G_CH32_PINMAP.sensor_count);
    }

#if THERMALCORE_CH32_TELEMETRY
    /* Bring up the telemetry UART and emit the canonical CSV header
     * once, before any data rows. */
    bsp_ch32_uart_init(115200);
    bsp_ch32_uart_puts(THERMALCORE_CANONICAL_HEADER);
#endif

    /* thermal_core_t is large relative to the 2 KB SRAM; main()
     * never returns, so a static local keeps it out of the stack. */
    static thermal_core_t core;
    thermal_core_callbacks_t cb;
    cb.log_event = ch32_log_event_cb;
#if THERMALCORE_CH32_TELEMETRY
    cb.telemetry_emit = ch32_telemetry_emit_cb;
#else
    cb.telemetry_emit = 0;
#endif
    if (thermal_core_init(&core, &G_THERMAL_CFG, &cb) != THERMAL_OK) {
        for (;;) { }                 /* config rejected: halt */
    }

    uint32_t       now_ms = 0;
    const uint16_t dt_ms  = G_THERMAL_CFG.control_period_ms;

    for (;;) {
        uint32_t win_t0 = SysTick->CNT;

        /* Build the input snapshot: one temperature sample per
         * sensor, one tach-RPM sample per actuator. */
        thermal_sample_t samples[THERMAL_MAX_SAMPLES_PER_SNAPSHOT];
        uint8_t  n           = 0;
        int32_t  temp0_mc    = 0;
        uint8_t  temp0_valid = 0;
        uint32_t rpm0        = 0;

        for (uint8_t i = 0; i < G_THERMAL_CFG.sensor_count; i++) {
            int32_t temp_mc = 0;
            int ok = (bsp_ch32_sensor_read_mc(i, &temp_mc) == 0);
            samples[n].id           = G_THERMAL_CFG.sensors[i].id;
            samples[n].kind         = THERMAL_SAMPLE_TEMP_MC;
            samples[n].sample_ts_ms = now_ms;
            samples[n].value        = temp_mc;
            samples[n].valid        = (uint8_t)ok;
            samples[n].quality      = 0;
            if (i == 0) {
                temp0_mc    = temp_mc;
                temp0_valid = (uint8_t)ok;
            }
            n++;
        }
        for (uint8_t i = 0; i < G_THERMAL_CFG.actuator_count; i++) {
            /* NF-A8: 2 tach pulses per revolution; over a 1 s window
             * the tick delta times 30 is RPM. */
            uint32_t rpm = bsp_ch32_tach_read_ticks_delta(i) * 30u;
            samples[n].id           = G_THERMAL_CFG.actuators[i].id;
            samples[n].kind         = THERMAL_SAMPLE_TACH_RPM;
            samples[n].sample_ts_ms = now_ms;
            samples[n].value        = (int32_t)rpm;
            samples[n].valid        = 1;
            samples[n].quality      = 0;
            if (i == 0) {
                rpm0 = rpm;
            }
            n++;
        }

        thermal_input_snapshot_t snap;
        snap.now_ms       = now_ms;
        snap.samples      = samples;
        snap.sample_count = n;

        /* Tick the core. */
        thermal_output_frame_t out;
        memset(&out, 0, sizeof(out));
        (void)thermal_core_step(&core, &snap, &out);

        /* Apply actuator commands (resolve actuator_id -> slot). */
        uint8_t duty0 = 0;
        for (uint8_t i = 0; i < out.actuator_cmd_count; i++) {
            for (uint8_t s = 0; s < G_THERMAL_CFG.actuator_count; s++) {
                if (out.actuator_cmds[i].actuator_id ==
                    G_THERMAL_CFG.actuators[s].id) {
                    bsp_ch32_pwm_set_duty(s,
                        out.actuator_cmds[i].duty_0_255);
                    if (s == 0) {
                        duty0 = out.actuator_cmds[i].duty_0_255;
                    }
                    break;
                }
            }
        }

#if THERMALCORE_CH32_STATUS
        if (temp0_valid) {
            printf("T=%d mC  duty=%u/255  fan=%u RPM\n",
                   (int)temp0_mc, (unsigned)duty0, (unsigned)rpm0);
        } else {
            printf("T=ERR     duty=%u/255  fan=%u RPM\n",
                   (unsigned)duty0, (unsigned)rpm0);
        }
#endif

        /* Hold the window open to the full control period. */
        while ((uint32_t)(SysTick->CNT - win_t0) <
               Ticks_from_Ms(dt_ms)) {
        }
        now_ms += dt_ms;
    }
}
