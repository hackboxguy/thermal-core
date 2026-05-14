/* platform/linux/bsp_hil_serial.c
 *
 * See bsp_hil_serial.h for the contract.
 */
/* Need glibc's BSD-extension `cfmakeraw` (not POSIX-only) so the
 * termios open path stays a 1-call setup; same feature-test macro
 * pattern as bsp_socketcan.c's _POSIX_C_SOURCE.
 * NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
#define _DEFAULT_SOURCE
#include "bsp_hil_serial.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "thermal_signals.h"
#include "thermal_wire.h"
#include "thermal_wire_opcodes.h"

/* Platform-private command ID per PRD §8.3 (0x8000..0xFFFF).
 * Mirrors HIL_CMD_SET_PWM_DUTY in platform/esp32_idf/main/main.c
 * -- this constant stays platform-private (not in
 * core/thermal_commands.h). */
#define HIL_CMD_SET_PWM_DUTY  0x8001u

/* Daemon transports run with CRC always validated on serial
 * (PRD §7.2 line 921). */
#define HIL_CRC_ENABLED  1

/* === Sample cache ========================================== */

typedef struct {
    int32_t  value;
    uint32_t last_ts_ms;
    uint8_t  ever_seen;
} hil_sample_cell_t;

static hil_sample_cell_t s_sensor_cache[THERMAL_MAX_SENSORS];
static hil_sample_cell_t s_tach_cache[THERMAL_MAX_ACTUATORS];

/* === Rx frame accumulator ================================== */

#define HIL_RX_BUF_SIZE (THERMAL_WIRE_MAX_LINUX \
                         + THERMAL_WIRE_HEADER_LEN \
                         + THERMAL_WIRE_CRC_LEN)

static uint8_t s_rx_buf[HIL_RX_BUF_SIZE];
static size_t  s_rx_pos = 0;

/* === Outbound sequence counter ============================= */

static uint16_t s_tx_seq = 0;

/* === Counters (for debug visibility) ======================= */

static uint64_t s_ack_count  = 0;
static uint64_t s_nack_count = 0;

/* =========================================================== */
/* URI parsing + termios setup                                  */
/* =========================================================== */

static speed_t baud_to_speed(long b)
{
    switch (b) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B0;     /* signal error */
    }
}

int bsp_hil_serial_open(const char *uri)
{
    if (!uri || strncmp(uri, "serial:", 7) != 0) {
        fprintf(stderr, "bsp_hil_serial: URI must start with "
                        "'serial:'; got %s\n",
                uri ? uri : "(null)");
        return -1;
    }
    const char *rest = uri + 7;

    /* Split optional :baud off the tail.  The path itself never
     * has a colon (it's a /dev/tty* node), so rightmost-colon is
     * unambiguous if the segment after parses as a positive int. */
    char path[128];
    long baud = 115200;
    const char *colon = strrchr(rest, ':');
    if (colon && colon != rest) {
        char *endp = NULL;
        long b = strtol(colon + 1, &endp, 10);
        if (endp && *endp == '\0' && b > 0) {
            size_t plen = (size_t)(colon - rest);
            if (plen >= sizeof(path)) {
                fprintf(stderr, "bsp_hil_serial: path too long: %s\n", uri);
                return -1;
            }
            memcpy(path, rest, plen);
            path[plen] = '\0';
            baud = b;
        } else {
            (void)snprintf(path, sizeof(path), "%s", rest);
        }
    } else {
        (void)snprintf(path, sizeof(path), "%s", rest);
    }

    speed_t spd = baud_to_speed(baud);
    if (spd == B0) {
        fprintf(stderr, "bsp_hil_serial: unsupported baud %ld\n", baud);
        return -1;
    }

    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "bsp_hil_serial: open %s failed: %s\n",
                path, strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        fprintf(stderr, "bsp_hil_serial: tcgetattr %s failed: %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    (void)cfsetispeed(&tio, spd);
    (void)cfsetospeed(&tio, spd);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN]  = 0;          /* non-blocking */
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        fprintf(stderr, "bsp_hil_serial: tcsetattr %s failed: %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }

    /* Reset module state on each fresh open. */
    memset(s_sensor_cache, 0, sizeof(s_sensor_cache));
    memset(s_tach_cache,   0, sizeof(s_tach_cache));
    s_rx_pos     = 0;
    s_tx_seq     = 0;
    s_ack_count  = 0;
    s_nack_count = 0;

    fprintf(stderr, "bsp_hil_serial: opened %s @ %ld baud (HIL_PERIPHERAL)\n",
            path, baud);
    return fd;
}

void bsp_hil_serial_close(int fd)
{
    if (fd >= 0) close(fd);
    fprintf(stderr,
            "bsp_hil_serial: closed (ACKs=%llu NACKs=%llu)\n",
            (unsigned long long)s_ack_count,
            (unsigned long long)s_nack_count);
}

/* =========================================================== */
/* Inbound dispatch                                             */
/* =========================================================== */

static void dispatch_telem_sample(uint16_t signal_id, int32_t value,
                                   uint32_t ts_ms)
{
    if (signal_id >= TSIG_HIL_BASE
     && signal_id <  TSIG_HIL_BASE + TSIG_RANGE_SIZE) {
        uint16_t off  = signal_id - TSIG_HIL_BASE;
        uint8_t  sub  = (uint8_t)(off & 0xF0u);
        uint8_t  slot = (uint8_t)(off & 0x0Fu);
        if (sub == TSIG_HIL_SUB_SENSOR_TEMP && slot < THERMAL_MAX_SENSORS) {
            s_sensor_cache[slot].value      = value;
            s_sensor_cache[slot].last_ts_ms = ts_ms;
            s_sensor_cache[slot].ever_seen  = 1;
            return;
        }
        if (sub == TSIG_HIL_SUB_TACH_RPM && slot < THERMAL_MAX_ACTUATORS) {
            s_tach_cache[slot].value      = value;
            s_tach_cache[slot].last_ts_ms = ts_ms;
            s_tach_cache[slot].ever_seen  = 1;
            return;
        }
    }
    /* Unknown signal: drop silently (no counter in v1). */
}

static void dispatch_frame(const thermal_wire_frame_t *fr, uint32_t now_ms)
{
    if (fr->opcode == THERMAL_WIRE_OP_TELEM_SAMPLE) {
        uint16_t signal_id;
        uint16_t flags;
        int32_t  value;
        if (thermal_wire_decode_telem_sample(fr, &signal_id,
                                              &flags, &value) == THERMAL_WIRE_OK) {
            (void)flags;
            dispatch_telem_sample(signal_id, value, fr->ts_ms);
        }
        return;
    }
    if (fr->opcode == THERMAL_WIRE_OP_CMD_ACK
     || fr->opcode == THERMAL_WIRE_OP_CMD_NACK) {
        uint16_t request_seq, status;
        uint32_t detail;
        if (thermal_wire_decode_cmd_ack_or_nack(fr, &request_seq,
                                                 &status, &detail) == THERMAL_WIRE_OK) {
            if (fr->opcode == THERMAL_WIRE_OP_CMD_ACK) {
                s_ack_count++;
            } else {
                s_nack_count++;
                fprintf(stderr,
                        "bsp_hil_serial: NACK at %u ms req_seq=%u "
                        "status=0x%04x detail=0x%08x\n",
                        (unsigned)now_ms, (unsigned)request_seq,
                        (unsigned)status, (unsigned)detail);
            }
        }
        return;
    }
    /* TELEM_EVENT and other opcodes: drop. */
}

void bsp_hil_serial_drain(int fd, uint32_t now_ms)
{
    if (fd < 0) return;

    /* 1. Pull whatever the kernel has into the tail of the buffer. */
    size_t free_cap = sizeof(s_rx_buf) - s_rx_pos;
    if (free_cap > 0) {
        ssize_t n = read(fd, s_rx_buf + s_rx_pos, free_cap);
        if (n > 0) s_rx_pos += (size_t)n;
    } else {
        /* Pathological: buffer full with no decodable frame at
         * the head.  Drop the oldest byte and retry next tick. */
        memmove(s_rx_buf, s_rx_buf + 1, s_rx_pos - 1);
        s_rx_pos--;
    }

    /* 2. Decode at offset 0 until short or sync-lost. */
    while (s_rx_pos >= THERMAL_WIRE_HEADER_LEN) {
        thermal_wire_frame_t fr;
        int dec = thermal_wire_decode_frame(s_rx_buf, s_rx_pos,
                                            THERMAL_WIRE_MAX_LINUX,
                                            HIL_CRC_ENABLED, &fr);
        if (dec == THERMAL_WIRE_OK) {
            dispatch_frame(&fr, now_ms);
            size_t consumed = (size_t)THERMAL_WIRE_HEADER_LEN
                            + fr.payload_len
                            + (size_t)THERMAL_WIRE_CRC_LEN;
            memmove(s_rx_buf, s_rx_buf + consumed,
                    s_rx_pos - consumed);
            s_rx_pos -= consumed;
        } else if (dec == THERMAL_WIRE_ERR_TRUNCATED) {
            return;
        } else {
            /* BAD_MAGIC / BAD_CRC / ... -- drop 1 byte for
             * self-healing resync.  Mirrors the firmware-side
             * accumulator in platform/esp32_idf/main/main.c. */
            memmove(s_rx_buf, s_rx_buf + 1, s_rx_pos - 1);
            s_rx_pos--;
        }
    }
}

/* =========================================================== */
/* Snapshot-build accessors                                     */
/* =========================================================== */

int bsp_hil_serial_read_sensor(const thermal_config_t *cfg,
                                uint8_t slot, uint32_t now_ms,
                                thermal_sample_t *out)
{
    (void)now_ms;
    if (!cfg || !out) return 0;
    memset(out, 0, sizeof(*out));
    if (slot >= cfg->sensor_count || slot >= THERMAL_MAX_SENSORS) {
        return 0;   /* valid=0 already from memset */
    }
    out->id           = cfg->sensors[slot].id;
    out->kind         = THERMAL_SAMPLE_TEMP_MC;
    out->sample_ts_ms = s_sensor_cache[slot].last_ts_ms;
    out->value        = s_sensor_cache[slot].value;
    out->valid        = s_sensor_cache[slot].ever_seen;
    out->quality      = 0;
    return 0;
}

int bsp_hil_serial_read_tach(const thermal_config_t *cfg,
                              uint8_t slot, uint32_t now_ms,
                              thermal_sample_t *out)
{
    (void)now_ms;
    if (!cfg || !out) return 0;
    memset(out, 0, sizeof(*out));
    if (slot >= cfg->actuator_count || slot >= THERMAL_MAX_ACTUATORS) {
        return 0;
    }
    out->id           = cfg->actuators[slot].id;
    out->kind         = THERMAL_SAMPLE_TACH_RPM;
    out->sample_ts_ms = s_tach_cache[slot].last_ts_ms;
    out->value        = s_tach_cache[slot].value;
    out->valid        = s_tach_cache[slot].ever_seen;
    out->quality      = 0;
    return 0;
}

/* =========================================================== */
/* Outbound CMD_REQUEST(HIL_CMD_SET_PWM_DUTY, duty)             */
/* =========================================================== */

static inline void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

int bsp_hil_serial_write_actuator(int fd, uint32_t ts_ms,
                                   uint8_t slot, uint8_t duty_0_255)
{
    (void)slot;   /* v1: duty applies to actuator slot 0 on the firmware side */
    if (fd < 0) return -1;

    /* HEADER(12) + payload(3 = u16 cmd_id LE + u8 duty) + CRC(2) = 17 */
    uint8_t frame[THERMAL_WIRE_HEADER_LEN + 3u + THERMAL_WIRE_CRC_LEN];

    /* Header per PRD §7.2: TC magic + version + opcode + seq +
     * payload_len + ts_ms. */
    frame[0] = 'T';
    frame[1] = 'C';
    frame[2] = 1;                                  /* version */
    frame[3] = THERMAL_WIRE_OP_CMD_REQUEST;
    put_u16_le(frame + 4,  ++s_tx_seq);
    put_u16_le(frame + 6,  3u);                    /* payload_len */
    put_u32_le(frame + 8,  ts_ms);

    /* Payload: u16 command_id LE + u8 duty. */
    put_u16_le(frame + 12, HIL_CMD_SET_PWM_DUTY);
    frame[14] = duty_0_255;

    /* Trailing CRC over HEADER+payload (PRD §7.2 lines 920-921). */
    uint16_t crc = thermal_wire_crc16(frame, 15);
    put_u16_le(frame + 15, crc);

    ssize_t n = write(fd, frame, sizeof(frame));
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr,
                    "bsp_hil_serial: write failed: %s\n",
                    strerror(errno));
        }
        return -1;
    }
    return (int)n;
}
