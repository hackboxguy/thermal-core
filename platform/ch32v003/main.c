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

/* Optional host-command channel over USART1 RX (Stage 19, Makefile:
 * CH32_COMMAND=1 -> -DTHERMALCORE_CH32_COMMAND). A line-based ASCII
 * command parser plus a control-loop bypass mode, driven by the host
 * tools/thermal-telemetry-tool -- the bench path that captures a
 * PWM->RPM sweep. Default off; with it off this file is byte-for-byte
 * the standard STANDALONE regulator. */
#ifndef THERMALCORE_CH32_COMMAND
#define THERMALCORE_CH32_COMMAND 0
#endif

/* Optional per-tick execution-time probe (Makefile: CH32_STEP_TIMING=1
 * -> -DTHERMALCORE_CH32_STEP_TIMING). Brackets thermal_core_step() with
 * a SysTick read and reports its microsecond cost as a `#`-comment
 * line on the telemetry UART -- so it lands in the same host capture
 * as the canonical CSV, with no SWIO console needed. It
 * therefore requires the telemetry tap (CH32_TELEMETRY=1). Default
 * off -- a characterisation diagnostic; the basic firmware is
 * byte-for-byte unaffected. */
#if THERMALCORE_CH32_STEP_TIMING && !THERMALCORE_CH32_TELEMETRY
#error "CH32_STEP_TIMING needs CH32_TELEMETRY=1 (it reports over the telemetry UART)"
#endif
#if THERMALCORE_CH32_STATUS || THERMALCORE_CH32_STEP_TIMING || \
    THERMALCORE_CH32_COMMAND
#include <stdio.h>
#endif

/* Optional canonical-CSV telemetry tap over USART1 (Stage 18e),
 * compile-gated by THERMALCORE_CH32_TELEMETRY (ch32_callbacks.h).
 * Independent of the SWIO status line above -- different transport. */
#if THERMALCORE_CH32_TELEMETRY
#include "bsp_ch32_uart.h"
#include "canonical.h"
/* Telemetry UART baud. 9600 is a conservative default for the
 * crystal-less CH32V003; override at build time with the Makefile's
 * CH32_TELEMETRY_BAUD= argument (-> -DTHERMALCORE_CH32_TELEMETRY_BAUD).
 * 115200 is fine with solid wiring -- notably a common ground to
 * the USB-serial adapter. */
#ifndef THERMALCORE_CH32_TELEMETRY_BAUD
#define THERMALCORE_CH32_TELEMETRY_BAUD 9600
#endif
#endif

/* The command channel writes its responses over the telemetry UART,
 * so it requires the telemetry tap; the Makefile's CH32_COMMAND=1
 * forces CH32_TELEMETRY on, and this guards a hand-passed -D. */
#if THERMALCORE_CH32_COMMAND
#if !THERMALCORE_CH32_TELEMETRY
#error "CH32_COMMAND needs CH32_TELEMETRY=1 (it shares the telemetry UART)"
#endif
#include "ch32_command.h"
#endif

/* Emitted by json2static.py from configs/ch32v003-standalone.json. */
extern const thermal_config_t G_THERMAL_CFG;
extern const ch32_pinmap_t    G_CH32_PINMAP;

#if THERMALCORE_CH32_COMMAND
/* Drain the USART1 RX register and execute any completed command
 * lines, writing each command's response. `cur_duty` / `cur_rpm` are
 * the most recent applied duty and measured RPM -- what pwmget /
 * rpmget report. `loop_bypass` and `host_duty` are updated in place;
 * the caller acts on `loop_bypass` to enter / leave the bypass loop. */
static void ch32_command_poll(ch32_command_rx_t *rx,
                              uint8_t  *loop_bypass,
                              uint8_t  *host_duty,
                              uint8_t   cur_duty,
                              uint32_t  cur_rpm)
{
    int b;
    while ((b = bsp_ch32_uart_getc()) >= 0) {
        ch32_command_t cmd;
        char           resp[20];
        if (!ch32_command_feed(rx, (char)b, &cmd)) {
            continue;
        }
        switch (cmd.type) {
        case CH32_CMD_LOOP_ON:
            *loop_bypass = 0;
            bsp_ch32_uart_puts("+loop on\n");
            break;
        case CH32_CMD_LOOP_OFF:
            *loop_bypass = 1;
            bsp_ch32_uart_puts("+loop off\n");
            break;
        case CH32_CMD_PWMSET:
            /* Manual duty only makes sense with the loop bypassed --
             * otherwise the governor overwrites it next tick. */
            if (*loop_bypass) {
                *host_duty = (uint8_t)cmd.arg;
                (void)snprintf(resp, sizeof resp, "+pwm %u\n",
                               (unsigned)cmd.arg);
                bsp_ch32_uart_puts(resp);
            } else {
                bsp_ch32_uart_puts("-err\n");
            }
            break;
        case CH32_CMD_PWMGET:
            (void)snprintf(resp, sizeof resp, "+pwm %u\n",
                           (unsigned)cur_duty);
            bsp_ch32_uart_puts(resp);
            break;
        case CH32_CMD_RPMGET:
            (void)snprintf(resp, sizeof resp, "+rpm %lu\n",
                           (unsigned long)cur_rpm);
            bsp_ch32_uart_puts(resp);
            break;
        case CH32_CMD_PING:
            bsp_ch32_uart_puts("+pong\n");
            break;
        case CH32_CMD_NONE:
            break;                       /* blank line -- ignore */
        case CH32_CMD_ERROR:
        default:
            bsp_ch32_uart_puts("-err\n");
            break;
        }
    }
}
#endif /* THERMALCORE_CH32_COMMAND */

int main(void)
{
    SystemInit();
    Delay_Ms(100);

    /* Bring up the BSPs from the JSON-driven pin map.  A non-zero
     * return is a slot or pin the BSP cannot honour -- halt rather
     * than run a regulator that cannot drive its hardware. */
    int bsp_status = 0;
    for (uint8_t i = 0; i < G_CH32_PINMAP.actuator_count; i++) {
        bsp_status |= bsp_ch32_pwm_init(i,
                          G_CH32_PINMAP.actuators[i].pwm_gpio,
                          G_CH32_PINMAP.actuators[i].pwm_freq_hz);
        bsp_status |= bsp_ch32_tach_init(i,
                          G_CH32_PINMAP.actuators[i].tach_gpio);
    }
    if (G_CH32_PINMAP.sensor_count > 0) {
        bsp_status |= bsp_ch32_sensor_init(
                          G_CH32_PINMAP.sensors[0].onewire_gpio,
                          G_CH32_PINMAP.sensor_count);
    }
    if (bsp_status != 0) {
        for (;;) { }                 /* BSP init failed: halt */
    }

#if THERMALCORE_CH32_TELEMETRY
    /* Bring up the telemetry UART and emit the canonical CSV header
     * once, before any data rows. */
    bsp_ch32_uart_init(THERMALCORE_CH32_TELEMETRY_BAUD);
    bsp_ch32_uart_puts(THERMALCORE_CANONICAL_HEADER);
#endif

#if THERMALCORE_CH32_COMMAND
    /* Host-command channel state: the RX-line accumulator, the bypass
     * flag, the host-set manual duty, and the last published duty /
     * RPM that pwmget / rpmget answer with. */
    ch32_command_rx_t cmd_rx;
    memset(&cmd_rx, 0, sizeof(cmd_rx));
    uint8_t  loop_bypass = 0;
    uint8_t  host_duty   = 0;
    uint8_t  last_duty   = 0;
    uint32_t last_rpm    = 0;
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

#if THERMALCORE_CH32_COMMAND
        /* Service the host-command channel. A `loop off` received
         * here drops into the bypass loop below: manual PWM control
         * with no sensor read and no thermal_core_step() -- the
         * characterisation path the PWM->RPM sweep needs. Bypass also
         * suspends thermal protection, so it is a bench mode, run
         * with an operator present. */
        ch32_command_poll(&cmd_rx, &loop_bypass, &host_duty,
                          last_duty, last_rpm);
        while (loop_bypass) {
            uint32_t bypass_t0 = SysTick->CNT;
            bsp_ch32_pwm_set_duty(0, host_duty);
            last_duty = host_duty;
            /* tach delta over the 1 s window * 30 = RPM (NF-A8: 2
             * pulses/rev) -- the conversion the regulating tick uses.
             * The duty is applied once at window start so each tach
             * reading spans a single steady duty. */
            last_rpm = bsp_ch32_tach_read_ticks_delta(0) * 30u;
            while ((uint32_t)(SysTick->CNT - bypass_t0) <
                   Ticks_from_Ms(dt_ms)) {
                ch32_command_poll(&cmd_rx, &loop_bypass, &host_duty,
                                  last_duty, last_rpm);
            }
        }
#endif

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
#if THERMALCORE_CH32_STEP_TIMING
        uint32_t step_t0 = SysTick->CNT;
#endif
        (void)thermal_core_step(&core, &snap, &out);
#if THERMALCORE_CH32_STEP_TIMING
        {
            /* step_ticks is SysTick counts -- SysTick runs at HCLK/8
             * on the CH32V003, NOT the core clock. Ticks_from_Ms(1) is
             * SysTick counts per ms, so step_ticks*1000/that is the
             * wall-clock cost in microseconds. Emitted as a `#` comment
             * so canonical-CSV parsers skip it while the raw host
             * capture still carries the number. */
            uint32_t step_ticks = (uint32_t)(SysTick->CNT - step_t0);
            uint32_t step_us = step_ticks * 1000u / Ticks_from_Ms(1);
            char tbuf[64];
            (void)snprintf(tbuf, sizeof(tbuf),
                           "# thermal_core_step: %lu us "
                           "(%lu SysTick ticks)\n",
                           (unsigned long)step_us,
                           (unsigned long)step_ticks);
            bsp_ch32_uart_puts(tbuf);
        }
#endif

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

#if THERMALCORE_CH32_TELEMETRY
        /* Drain the queued telemetry records to USART1 here --
         * outside thermal_core_step(), so the blocking UART writes
         * never inflate the core's bounded per-tick work. */
        ch32_telemetry_drain();
#endif

#if THERMALCORE_CH32_STATUS
        if (temp0_valid) {
            printf("T=%d mC  duty=%u/255  fan=%u RPM\n",
                   (int)temp0_mc, (unsigned)duty0, (unsigned)rpm0);
        } else {
            printf("T=ERR     duty=%u/255  fan=%u RPM\n",
                   (unsigned)duty0, (unsigned)rpm0);
        }
#endif

#if THERMALCORE_CH32_COMMAND
        /* Publish this regulating tick's outputs so pwmget / rpmget
         * answer with live governor values even outside bypass. */
        last_duty = duty0;
        last_rpm  = rpm0;
#endif

        /* Hold the window open to the full control period. */
        while ((uint32_t)(SysTick->CNT - win_t0) <
               Ticks_from_Ms(dt_ms)) {
#if THERMALCORE_CH32_COMMAND
            ch32_command_poll(&cmd_rx, &loop_bypass, &host_duty,
                              last_duty, last_rpm);
#endif
        }
        now_ms += dt_ms;
    }
}
