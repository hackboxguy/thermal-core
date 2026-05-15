/* platform/esp32_idf/main/main.c
 *
 * Stage 14e (refactor) -- JSON-driven pin map.
 *
 * The hardcoded `#define PIN_*` constants are gone.  Pin
 * assignments come from `G_ESP32_PINMAP`, emitted by
 * tools/json2static.py from the JSON config's `esp32_pinmap`
 * section.  Loops over `G_THERMAL_CFG.sensor_count` /
 * `actuator_count` replace the slot-0 hardcoding.  Adding a
 * second fan or DS18B20 is now a JSON-only change: edit the
 * config, rebuild, flash.
 *
 * Three build modes selected at compile time:
 *
 *   STANDALONE        (default; same as 13b):
 *     Real DS18B20 + tach + LEDC fan.  Sensor at 1 Hz, core at
 *     10 Hz, status line at 1 Hz.  Drives the bench rig.
 *
 *   REPLAY_STANDALONE (-DTHERMALCORE_REPLAY_STANDALONE=ON):
 *     No BSP init.  Walks `G_REPLAY_TICKS[]`, feeds each row
 *     through thermal_core_step(), emits CSV via callbacks.
 *
 *   HIL_PERIPHERAL    (-DTHERMALCORE_HIL_PERIPHERAL=ON):
 *     Real BSPs, no thermal_core_step (daemon owns the core).
 *     Read sensor + tach per slot, emit TELEM_SAMPLE frames over
 *     USB-Serial-JTAG, drain inbound CMD_REQUEST frames, apply
 *     per-slot PWM duty.  4-byte CMD_REQUEST payload:
 *     u16 cmd_id + u8 slot + u8 duty.
 *
 * See PRD section 4.2 + 8.3 and impl-plan section 5
 * Stages 13 + 14.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp_esp32_pwm.h"
#include "bsp_esp32_tach.h"
#include "bsp_esp32_sensor.h"
#include "esp32_callbacks.h"
#include "esp32_pinmap.h"

#include "thermal_core.h"
#include "thermal_types.h"
#include "thermal_signals.h"
#include "thermal_config.h"

#if defined(THERMALCORE_MODE_REPLAY_STANDALONE)
#include "replay_fixture.h"
#endif

#if defined(THERMALCORE_MODE_HIL_PERIPHERAL)
#include "esp32_usb_cdc.h"
#include "thermal_wire.h"
#include "thermal_wire_opcodes.h"
#endif

extern const thermal_config_t G_THERMAL_CFG;
extern const esp32_pinmap_t   G_ESP32_PINMAP;

static const char *TAG = "thermal";

/* === Shared BSP init (real-hardware modes: STANDALONE + HIL) ====
 * Loop over G_ESP32_PINMAP for the actuator pins, do one
 * sensor-bus init for the shared 1-Wire bus (all DS18B20s
 * share GPIO 6 in v1).  Returns 0 on success, -1 on any BSP
 * init failure. */
#if defined(THERMALCORE_MODE_HIL_PERIPHERAL) \
    || (!defined(THERMALCORE_MODE_REPLAY_STANDALONE))

static int init_bsps(uint8_t *sensor_ok)
{
    for (uint8_t a = 0; a < G_ESP32_PINMAP.actuator_count; a++) {
        if (bsp_esp32_pwm_init(a,
                G_ESP32_PINMAP.actuators[a].pwm_gpio,
                G_ESP32_PINMAP.actuators[a].pwm_freq_hz) != 0) return -1;
        if (bsp_esp32_tach_init(a,
                G_ESP32_PINMAP.actuators[a].tach_gpio) != 0) return -1;
    }
    /* Single shared 1-Wire bus: all sensors live on slot 0's GPIO. */
    int rc = bsp_esp32_sensor_init(
                G_ESP32_PINMAP.sensors[0].onewire_gpio,
                G_THERMAL_CFG.sensor_count);
    *sensor_ok = (rc == 0);
    return 0;
}

#endif

#if defined(THERMALCORE_MODE_HIL_PERIPHERAL)

/* Provisional HIL ingress signal IDs (canonical since 14c).  Slot
 * decoding via TSIG_HIL_SENSOR_TEMP(slot) / TSIG_HIL_TACH_RPM(slot)
 * from core/thermal_signals.h. */

/* Platform-private command IDs per PRD section 8.3 (0x8000+).
 * 14e: payload now 4 bytes -- u16 cmd_id LE + u8 slot + u8 duty. */
#define HIL_CMD_SET_PWM_DUTY   0x8001u

#define HIL_RX_BUF_SIZE (THERMAL_WIRE_MAX_MCU \
                         + THERMAL_WIRE_HEADER_LEN \
                         + THERMAL_WIRE_CRC_LEN)

/* === HIL inbound dispatch ================================ */

static void hil_dispatch_frame(const thermal_wire_frame_t *fr,
                                uint32_t now_ms, uint16_t *out_seq)
{
    uint8_t reply[THERMAL_WIRE_HEADER_LEN + 8u + THERMAL_WIRE_CRC_LEN];

    if (fr->opcode != THERMAL_WIRE_OP_CMD_REQUEST) {
        int n = thermal_wire_encode_cmd_nack(
            reply, sizeof(reply), (*out_seq)++, now_ms,
            fr->seq, THERMAL_WIRE_STATUS_BAD_OPCODE,
            /*detail=*/0, /*crc_enabled=*/1);
        if (n > 0) esp32_usb_cdc_write(reply, (size_t)n);
        return;
    }

    uint16_t command_id = 0;
    if (fr->payload_len >= 2u) {
        command_id = (uint16_t)fr->payload[0]
                   | ((uint16_t)fr->payload[1] << 8);
    }

    /* HIL_CMD_SET_PWM_DUTY: 4-byte payload = u16 cmd_id + u8 slot + u8 duty. */
    if (command_id == HIL_CMD_SET_PWM_DUTY && fr->payload_len == 4u) {
        uint8_t slot = fr->payload[2];
        uint8_t duty = fr->payload[3];
        if (slot >= G_THERMAL_CFG.actuator_count) {
            int n = thermal_wire_encode_cmd_nack(
                reply, sizeof(reply), (*out_seq)++, now_ms,
                fr->seq, THERMAL_WIRE_STATUS_BAD_PAYLOAD,
                /*detail=*/(uint32_t)slot, /*crc_enabled=*/1);
            if (n > 0) esp32_usb_cdc_write(reply, (size_t)n);
            return;
        }
        bsp_esp32_pwm_set_duty(slot, duty);
        int n = thermal_wire_encode_cmd_ack(
            reply, sizeof(reply), (*out_seq)++, now_ms,
            fr->seq, /*status=*/0u,
            /*detail=*/(uint32_t)duty, /*crc_enabled=*/1);
        if (n > 0) esp32_usb_cdc_write(reply, (size_t)n);
        return;
    }

    /* Anything else: NACK with BAD_PAYLOAD; detail carries the
     * offending command_id so the host can correlate. */
    {
        int n = thermal_wire_encode_cmd_nack(
            reply, sizeof(reply), (*out_seq)++, now_ms,
            fr->seq, THERMAL_WIRE_STATUS_BAD_PAYLOAD,
            /*detail=*/(uint32_t)command_id, /*crc_enabled=*/1);
        if (n > 0) esp32_usb_cdc_write(reply, (size_t)n);
    }
}

static uint8_t s_hil_rx_buf[HIL_RX_BUF_SIZE];
static size_t  s_hil_rx_pos = 0;

static void hil_drain_commands(uint32_t now_ms, uint16_t *out_seq)
{
    size_t free_cap = sizeof(s_hil_rx_buf) - s_hil_rx_pos;
    if (free_cap > 0) {
        size_t got = esp32_usb_cdc_read(s_hil_rx_buf + s_hil_rx_pos,
                                         free_cap);
        s_hil_rx_pos += got;
    } else {
        memmove(s_hil_rx_buf, s_hil_rx_buf + 1, s_hil_rx_pos - 1);
        s_hil_rx_pos--;
    }

    while (s_hil_rx_pos >= THERMAL_WIRE_HEADER_LEN) {
        thermal_wire_frame_t fr;
        int dec = thermal_wire_decode_frame(s_hil_rx_buf, s_hil_rx_pos,
                                            THERMAL_WIRE_MAX_MCU,
                                            /*crc_enabled=*/1, &fr);
        if (dec == THERMAL_WIRE_OK) {
            hil_dispatch_frame(&fr, now_ms, out_seq);
            size_t consumed = (size_t)THERMAL_WIRE_HEADER_LEN
                            + fr.payload_len
                            + (size_t)THERMAL_WIRE_CRC_LEN;
            memmove(s_hil_rx_buf, s_hil_rx_buf + consumed,
                    s_hil_rx_pos - consumed);
            s_hil_rx_pos -= consumed;
        } else if (dec == THERMAL_WIRE_ERR_TRUNCATED) {
            return;
        } else {
            memmove(s_hil_rx_buf, s_hil_rx_buf + 1, s_hil_rx_pos - 1);
            s_hil_rx_pos--;
        }
    }
}

/* === HIL_PERIPHERAL app_main ============================== */

static void app_main_hil_peripheral(void)
{
    esp32_usb_cdc_init();

    uint8_t sensor_ok = 0;
    if (init_bsps(&sensor_ok) != 0) return;
    /* Park all fans at 0 duty until daemon's first CMD_REQUEST. */
    for (uint8_t a = 0; a < G_ESP32_PINMAP.actuator_count; a++) {
        bsp_esp32_pwm_set_duty(a, 0);
    }

    int32_t  cached_temp_mc[THERMAL_MAX_SENSORS];
    uint8_t  cached_valid[THERMAL_MAX_SENSORS];
    for (uint8_t i = 0; i < THERMAL_MAX_SENSORS; i++) {
        cached_temp_mc[i] = INT32_MIN;
        cached_valid[i]   = 0;
    }
    uint32_t now_ms           = 0;
    int      ticks_since_read = 10;     /* force a read on tick 0 */
    uint16_t seq              = 0;
    const uint16_t dt_ms      = 100;

    uint8_t frame[THERMAL_WIRE_MAX_MCU];

    for (;;) {
        int64_t t0_us = esp_timer_get_time();

        /* 1 Hz sensor refresh: read every configured sensor. */
        if (sensor_ok && ticks_since_read >= 10) {
            for (uint8_t i = 0; i < G_THERMAL_CFG.sensor_count; i++) {
                int32_t temp_mc;
                if (bsp_esp32_sensor_read_mc(i, &temp_mc) == 0) {
                    cached_temp_mc[i] = temp_mc;
                    cached_valid[i]   = 1;
                } else {
                    cached_valid[i] = 0;
                }
            }
            ticks_since_read = 0;
        }
        ticks_since_read++;

        /* Emit per-sensor temp samples (valid only). */
        for (uint8_t i = 0; i < G_THERMAL_CFG.sensor_count; i++) {
            if (!cached_valid[i]) continue;
            int n = thermal_wire_encode_telem_sample(
                frame, sizeof(frame), seq++, now_ms,
                TSIG_HIL_SENSOR_TEMP(i), /*flags=*/0,
                cached_temp_mc[i], /*crc_enabled=*/1);
            if (n > 0) esp32_usb_cdc_write(frame, (size_t)n);
        }

        /* Emit per-actuator tach samples. */
        for (uint8_t i = 0; i < G_THERMAL_CFG.actuator_count; i++) {
            uint32_t ticks = bsp_esp32_tach_read_ticks_delta(i);
            int n = thermal_wire_encode_telem_sample(
                frame, sizeof(frame), seq++, now_ms,
                TSIG_HIL_TACH_RPM(i), /*flags=*/0,
                (int32_t)(ticks * 30u), /*crc_enabled=*/1);
            if (n > 0) esp32_usb_cdc_write(frame, (size_t)n);
        }

        /* Drain inbound CMD_REQUEST frames before next outbound tick. */
        hil_drain_commands(now_ms, &seq);

        now_ms += dt_ms;
        int64_t elapsed_us = esp_timer_get_time() - t0_us;
        int64_t remaining_us = (int64_t)dt_ms * 1000 - elapsed_us;
        if (remaining_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(remaining_us / 1000));
        }
    }
}

#elif defined(THERMALCORE_MODE_REPLAY_STANDALONE)

/* === REPLAY_STANDALONE app_main =========================== */

static void app_main_replay(void)
{
    ESP_LOGI(TAG, "thermal-core ESP32-C3 REPLAY_STANDALONE (Stage 13 13c)");

    thermal_core_t            core;
    thermal_core_callbacks_t  cb = {
        .log_event      = esp32_log_event_cb,
        .telemetry_emit = esp32_telemetry_emit_cb,
    };
    thermal_status_t init_s = thermal_core_init(&core, &G_THERMAL_CFG, &cb);
    if (init_s != THERMAL_OK) {
        ESP_LOGE(TAG, "thermal_core_init failed: %d", (int)init_s);
        return;
    }

    const uint16_t dt_ms = G_THERMAL_CFG.control_period_ms;

    for (uint16_t tick = 0; tick < G_REPLAY_TICK_COUNT; tick++) {
        uint32_t now_ms = (uint32_t)tick * (uint32_t)dt_ms;

        thermal_sample_t samples[THERMAL_MAX_SAMPLES_PER_SNAPSHOT];
        uint8_t n = 0;

        for (uint8_t i = 0; i < G_THERMAL_CFG.sensor_count; i++) {
            samples[n].id           = G_THERMAL_CFG.sensors[i].id;
            samples[n].kind         = THERMAL_SAMPLE_TEMP_MC;
            samples[n].sample_ts_ms = now_ms;
            samples[n].value        = G_REPLAY_TICKS[tick].temp_mc;
            samples[n].valid        = 1;
            samples[n].quality      = 0;
            n++;
        }
        for (uint8_t i = 0; i < G_THERMAL_CFG.actuator_count; i++) {
            samples[n].id           = G_THERMAL_CFG.actuators[i].id;
            samples[n].kind         = THERMAL_SAMPLE_TACH_RPM;
            samples[n].sample_ts_ms = now_ms;
            samples[n].value        = (int32_t)G_REPLAY_TICKS[tick].tach_rpm;
            samples[n].valid        = 1;
            samples[n].quality      = 0;
            n++;
        }

        thermal_input_snapshot_t snap = {
            .now_ms       = now_ms,
            .samples      = samples,
            .sample_count = n,
        };

        thermal_output_frame_t out;
        memset(&out, 0, sizeof(out));
        (void)thermal_core_step(&core, &snap, &out);
    }

    printf("END\n");
    fflush(stdout);

    vTaskDelay(portMAX_DELAY);
}

#else

/* === STANDALONE app_main ================================== */

static void app_main_standalone(void)
{
    ESP_LOGI(TAG, "thermal-core ESP32-C3 STANDALONE (Stage 14e)");

    uint8_t sensor_ok = 0;
    if (init_bsps(&sensor_ok) != 0) return;

    thermal_core_t            core;
    thermal_core_callbacks_t  cb = {
        .log_event      = esp32_log_event_cb,
        .telemetry_emit = esp32_telemetry_emit_cb,
    };
    thermal_status_t init_s = thermal_core_init(&core, &G_THERMAL_CFG, &cb);
    if (init_s != THERMAL_OK) {
        ESP_LOGE(TAG, "thermal_core_init failed: %d", (int)init_s);
        return;
    }

    /* Per-slot sensor caches (refreshed at 1 Hz; reused per tick). */
    int32_t cached_temp_mc[THERMAL_MAX_SENSORS];
    uint8_t cached_valid[THERMAL_MAX_SENSORS];
    for (uint8_t i = 0; i < THERMAL_MAX_SENSORS; i++) {
        cached_temp_mc[i] = INT32_MIN;
        cached_valid[i]   = 0;
    }

    uint32_t now_ms        = 0;
    int      ticks_since_read = 10;     /* force a read on tick 0 */
    const uint16_t dt_ms   = G_THERMAL_CFG.control_period_ms;

    for (;;) {
        int64_t t0_us = esp_timer_get_time();

        /* Step 1: refresh sensors at 1 Hz */
        if (sensor_ok && ticks_since_read >= 10) {
            for (uint8_t i = 0; i < G_THERMAL_CFG.sensor_count; i++) {
                int32_t temp_mc;
                if (bsp_esp32_sensor_read_mc(i, &temp_mc) == 0) {
                    cached_temp_mc[i] = temp_mc;
                    cached_valid[i]   = 1;
                } else {
                    cached_valid[i] = 0;
                }
            }
            ticks_since_read = 0;
        }
        ticks_since_read++;

        /* Step 2: build input snapshot from caches + per-tick tach delta. */
        thermal_sample_t samples[THERMAL_MAX_SAMPLES_PER_SNAPSHOT];
        uint8_t n = 0;

        for (uint8_t i = 0; i < G_THERMAL_CFG.sensor_count; i++) {
            samples[n].id           = G_THERMAL_CFG.sensors[i].id;
            samples[n].kind         = THERMAL_SAMPLE_TEMP_MC;
            samples[n].sample_ts_ms = now_ms;
            samples[n].value        = cached_temp_mc[i];
            samples[n].valid        = cached_valid[i];
            samples[n].quality      = 0;
            n++;
        }

        uint32_t tach_ticks[THERMAL_MAX_ACTUATORS] = { 0 };
        for (uint8_t i = 0; i < G_THERMAL_CFG.actuator_count; i++) {
            tach_ticks[i] = bsp_esp32_tach_read_ticks_delta(i);
            samples[n].id           = G_THERMAL_CFG.actuators[i].id;
            samples[n].kind         = THERMAL_SAMPLE_TACH_RPM;
            samples[n].sample_ts_ms = now_ms;
            samples[n].value        = (int32_t)(tach_ticks[i] * 30u);
            samples[n].valid        = 1;
            samples[n].quality      = 0;
            n++;
        }

        thermal_input_snapshot_t snap = {
            .now_ms       = now_ms,
            .samples      = samples,
            .sample_count = n,
        };

        /* Step 3: tick the core */
        thermal_output_frame_t out;
        memset(&out, 0, sizeof(out));
        (void)thermal_core_step(&core, &snap, &out);

        /* Step 4: apply actuator commands (resolve actuator_id -> slot). */
        for (uint8_t i = 0; i < out.actuator_cmd_count; i++) {
            for (uint8_t s = 0; s < G_THERMAL_CFG.actuator_count; s++) {
                if (out.actuator_cmds[i].actuator_id ==
                    G_THERMAL_CFG.actuators[s].id) {
                    bsp_esp32_pwm_set_duty(s,
                        out.actuator_cmds[i].duty_0_255);
                    break;
                }
            }
        }

        /* Step 5: once per second print the status line for slot 0
         * (the multi-slot UDP telemetry covers the full set; the
         * console status line stays compact). */
        if (ticks_since_read == 1) {
            int32_t temp_mc = esp32_callbacks_last_zone_temp_mc(0);
            int32_t pwm_val = esp32_callbacks_last_actuator_pwm(0);
            if (cached_valid[0]) {
                printf("T=%6.2f C  duty=%3" PRId32 "/255 (%3" PRId32
                       "%%)  tach=%4" PRIu32 " ticks/s (~%4" PRIu32 " RPM)\n",
                       (double)temp_mc / 1000.0,
                       pwm_val, (pwm_val * 100) / 255,
                       tach_ticks[0], tach_ticks[0] * 30u);
            } else {
                printf("T=  ERR    duty=%3" PRId32 "/255 (%3" PRId32
                       "%%)  tach=%4" PRIu32 " ticks/s (~%4" PRIu32 " RPM)\n",
                       pwm_val, (pwm_val * 100) / 255,
                       tach_ticks[0], tach_ticks[0] * 30u);
            }
        }

        /* Pace the next tick. */
        now_ms += dt_ms;
        int64_t elapsed_us = esp_timer_get_time() - t0_us;
        int64_t remaining_us = (int64_t)dt_ms * 1000 - elapsed_us;
        if (remaining_us > 0) {
            vTaskDelay(pdMS_TO_TICKS(remaining_us / 1000));
        }
    }
}

#endif /* mode dispatch */

void app_main(void)
{
#if defined(THERMALCORE_MODE_HIL_PERIPHERAL)
    app_main_hil_peripheral();
#elif defined(THERMALCORE_MODE_REPLAY_STANDALONE)
    app_main_replay();
#else
    app_main_standalone();
#endif
}
