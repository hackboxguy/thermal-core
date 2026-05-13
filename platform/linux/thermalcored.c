/* platform/linux/thermalcored.c
 *
 * Stage 9 9c — Linux daemon entry point.
 *
 * Glues:
 *   - JSON loader (9a)           → thermal_config_t + thermalcored_runtime_cfg_t
 *   - bsp_mock_tmpfs (9b)        → file-based sensor reads + actuator writes
 *   - core (Stages 2-7)          → thermal_core_step()
 *   - protocol/thermal_wire (10a)→ canonical TC binary frame codec
 *
 * Loop order per tick (impl-plan §5 Stage 9):
 *   1. drain queued commands       (no-op until Stage 10)
 *   2. build input snapshot         (bsp_mock_tmpfs_build_snapshot)
 *   3. thermal_core_step()          (callbacks emit telemetry / events)
 *   4. write actuator frame         (bsp_mock_tmpfs_write_frame)
 *
 * Clock modes:
 *   --clock=wall      monotonic + absolute clock_nanosleep
 *   --clock=scenario  AF_UNIX SOCK_STREAM, reads "TICK <ms>\n" lines
 *                     from the scenario runner (default URI
 *                     unix:/tmp/thermalcored-clock.sock).
 */
/* POSIX feature test: sigaction, clock_nanosleep, getaddrinfo,
 * sockaddr_un + struct linger, openlog/syslog.  The leading
 * underscore is required by POSIX itself; clang-tidy's
 * reserved-identifier check is intentionally suppressed here. */
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include "thermal_core.h"
#include "thermal_types.h"
#include "thermal_config.h"
#include "thermal_commands.h"
#include "config_jsmn.h"
#include "runtime_cfg.h"
#include "bsp_mock_tmpfs.h"
#include "bsp_socketcan.h"
#include "obd2.h"
#include "thermal_wire.h"
#include "thermal_wire_opcodes.h"

/* =============================================================== */
/* File-scope statics                                               */
/* =============================================================== */
/*
 * Limited to this translation unit; the core never sees them.
 * The callbacks (log_event / telemetry_emit) read them to decide
 * where to route the data.
 */

static volatile sig_atomic_t g_running       = 1;
static int                   g_log_stderr    = 0;
static int                   g_telemetry_fd  = -1;
static int                   g_control_fd    = -1;
static uint16_t              g_out_seq       = 0;   /* wraps mod 65536 */
static char                  g_scenario_path[THERMAL_PATH_MAX] = {0};

/* SocketCAN BSP state (Stage 11 11b).  Inert (fd<0, slot<0) when
 * the config has no `canbus:<iface>` context source.  When active,
 * `g_canbus_context_slot` is the index into cfg.contexts[] / runtime.
 * contexts[] that this BSP fulfils.  `g_canbus_next_request_ms`
 * tracks the 1 Hz request cadence (PRD §6.2). */
static int                     g_canbus_fd               = -1;
static int                     g_canbus_context_slot     = -1;
static uint32_t                g_canbus_next_request_ms  = 0;
static bsp_socketcan_state_t   g_canbus_state;

/* CRC on UDP loopback is on by default per PRD §7.2 line 921 (the
 * codec is exercised end-to-end and any single-bit corruption is
 * still caught even on a "trusted" transport). */
#define DAEMON_CRC_ENABLED  1

/* =============================================================== */
/* Signal handling                                                  */
/* =============================================================== */

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    if (sigaction(SIGINT,  &sa, NULL) != 0) return -1;
    if (sigaction(SIGHUP,  &sa, NULL) != 0) return -1;

    /* Ignore SIGPIPE so a closed telemetry socket doesn't kill us. */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    return 0;
}

/* =============================================================== */
/* Telemetry UDP                                                    */
/* =============================================================== */

/* Parse "udp:host:port" into host + port strings. Returns 0 on
 * success, -1 if the URI doesn't match. host/port are NUL-terminated
 * on success. */
static int parse_udp_uri(const char *uri,
                         char *host, size_t host_sz,
                         char *port, size_t port_sz)
{
    if (!uri || strncmp(uri, "udp:", 4) != 0) return -1;
    const char *p   = uri + 4;
    const char *col = strrchr(p, ':');
    if (!col || col == p) return -1;
    size_t hlen = (size_t)(col - p);
    if (hlen + 1 > host_sz) return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    size_t plen = strlen(col + 1);
    if (plen == 0 || plen + 1 > port_sz) return -1;
    memcpy(port, col + 1, plen + 1);
    return 0;
}

/* Returns a connected UDP socket fd on success, or -1 on any
 * error (with a stderr warning).  Caller closes the fd. */
static int open_telemetry_socket(const char *transport_uri)
{
    if (!transport_uri || transport_uri[0] == '\0') {
        fprintf(stderr,
                "thermalcored: no telemetry transport configured\n");
        return -1;
    }

    char host[64], port[16];
    if (parse_udp_uri(transport_uri, host, sizeof(host),
                      port, sizeof(port)) != 0) {
        fprintf(stderr,
                "thermalcored: invalid telemetry transport URI '%s'\n",
                transport_uri);
        return -1;
    }

    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(host, port, &hints, &ai);
    if (rc != 0 || !ai) {
        fprintf(stderr,
                "thermalcored: getaddrinfo('%s:%s') failed: %s\n",
                host, port, gai_strerror(rc));
        return -1;
    }

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        fprintf(stderr,
                "thermalcored: socket failed: %s\n", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        fprintf(stderr,
                "thermalcored: connect failed: %s\n", strerror(errno));
        close(fd);
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    return fd;
}

/* =============================================================== */
/* Control listener (UDP, loopback-only per PRD §7.5 line 962)      */
/* =============================================================== */

/* Open the UDP control listener.  URI must be of the form
 * "udp:127.0.0.1:<port>" — any non-loopback host is rejected
 * (PRD §7.5 line 962: "must not bind to a non-loopback address
 * unless a deliberate unsafe-development flag is set").
 *
 * Returns:
 *   >= 0  socket fd on success.
 *   -1    `uri` is NULL / empty (listener intentionally disabled).
 *   -2    `uri` is non-empty but invalid (bad URI / non-loopback /
 *         bad port / bind failure).  The caller can promote this to
 *         a startup error when control.enable=true. */
static int open_control_socket(const char *uri)
{
    if (!uri || uri[0] == '\0') {
        return -1;  /* listener intentionally disabled */
    }
    char host[64], port[16];
    if (parse_udp_uri(uri, host, sizeof(host), port, sizeof(port)) != 0) {
        fprintf(stderr,
                "thermalcored: invalid control listen URI '%s'\n", uri);
        return -2;
    }
    if (strcmp(host, "127.0.0.1") != 0) {
        fprintf(stderr,
                "thermalcored: control.listen must be loopback (host=127.0.0.1); got '%s'\n",
                host);
        return -2;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr,
                "thermalcored: control socket failed: %s\n", strerror(errno));
        return -2;
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* 127.0.0.1 */
    long port_n = strtol(port, NULL, 10);
    if (port_n <= 0 || port_n > 65535) {
        fprintf(stderr, "thermalcored: bad control port '%s'\n", port);
        close(fd);
        return -2;
    }
    addr.sin_port = htons((uint16_t)port_n);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr,
                "thermalcored: control bind('%s:%ld') failed: %s\n",
                host, port_n, strerror(errno));
        close(fd);
        return -2;
    }

    fprintf(stderr,
            "thermalcored: control listener on udp:%s:%ld\n", host, port_n);
    return fd;
}

/* Encode + sendto an ACK (status==OK) or NACK (status!=OK).
 * `dest`/`dest_len` come from the recvfrom that delivered the
 * matching CMD_REQUEST. */
static void send_acknack(thermal_wire_opcode_t opcode,
                         const struct sockaddr_in *dest, socklen_t dest_len,
                         uint16_t request_seq, uint32_t now_ms,
                         uint16_t status, uint32_t detail_code)
{
    uint8_t buf[THERMAL_WIRE_HEADER_LEN + 32];
    int n = (opcode == THERMAL_WIRE_OP_CMD_ACK)
        ? thermal_wire_encode_cmd_ack (buf, sizeof(buf), g_out_seq++,
                                        now_ms, request_seq,
                                        status, detail_code,
                                        DAEMON_CRC_ENABLED)
        : thermal_wire_encode_cmd_nack(buf, sizeof(buf), g_out_seq++,
                                        now_ms, request_seq,
                                        status, detail_code,
                                        DAEMON_CRC_ENABLED);
    if (n <= 0 || g_control_fd < 0) return;
    (void)sendto(g_control_fd, buf, (size_t)n, MSG_DONTWAIT,
                 (const struct sockaddr *)dest, dest_len);
}

/* Read a little-endian u16 from `p`; used to recover `seq` from a
 * frame that failed deeper decode but whose header (magic + version)
 * already parsed cleanly.  Mirrors the helper in protocol/thermal_wire.c. */
static uint16_t read_u16_le_at(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Drain pending CMD_REQUEST frames; apply each to the core and
 * send an ACK or NACK back to the sender.  Called as Step 1 of
 * every tick (PRD §5 Stage 9 control-loop order).
 *
 * Transport-level reject taxonomy (PRD §7.2 line 937):
 *
 *   - BAD_OPCODE, OVER_CAP: outer header parsed; `seq` was extracted
 *     before the failure, so we can NACK with the matching
 *     THERMAL_WIRE_STATUS_* (≥ 0x8000) value.
 *   - BAD_PAYLOAD (per-opcode payload decode): same — `fr.seq` is
 *     already populated by thermal_wire_decode_frame().
 *   - Unexpected opcode at the listener (TELEM_*, CMD_ACK, ...): the
 *     decode succeeded but the frame isn't a CMD_REQUEST.  NACK with
 *     BAD_OPCODE; seq is trustworthy.
 *   - BAD_MAGIC, BAD_VERSION, BAD_CRC, TRUNCATED: the byte stream
 *     either isn't ours or has been corrupted; `seq` cannot be
 *     trusted.  Silent drop. */
static void drain_commands(thermal_core_t *core, uint32_t now_ms)
{
    if (g_control_fd < 0) return;
    uint8_t buf[THERMAL_WIRE_MAX_LINUX + THERMAL_WIRE_HEADER_LEN + 16];
    for (;;) {
        struct sockaddr_in sender;
        socklen_t          slen = sizeof(sender);
        ssize_t n = recvfrom(g_control_fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&sender, &slen);
        if (n <= 0) break;  /* EAGAIN / EWOULDBLOCK */

        thermal_wire_frame_t fr;
        int dec = thermal_wire_decode_frame(buf, (size_t)n,
                                            THERMAL_WIRE_MAX_LINUX,
                                            DAEMON_CRC_ENABLED, &fr);
        if (dec != THERMAL_WIRE_OK) {
            /* For BAD_OPCODE / OVER_CAP the magic + version were good
             * and the header parsed enough to extract `seq` at offset
             * 4.  We can NACK those.  Other failures (BAD_MAGIC,
             * BAD_VERSION, BAD_CRC, TRUNCATED) leave `seq` untrusted;
             * silently drop. */
            if ((dec == THERMAL_WIRE_ERR_BAD_OPCODE ||
                 dec == THERMAL_WIRE_ERR_OVER_CAP) &&
                (size_t)n >= THERMAL_WIRE_HEADER_LEN) {
                uint16_t seq = read_u16_le_at(buf + 4);
                uint16_t st  = (dec == THERMAL_WIRE_ERR_BAD_OPCODE)
                                 ? (uint16_t)THERMAL_WIRE_STATUS_BAD_OPCODE
                                 : (uint16_t)THERMAL_WIRE_STATUS_OVER_CAP;
                send_acknack(THERMAL_WIRE_OP_CMD_NACK, &sender, slen,
                             seq, now_ms, st, 0u);
            }
            continue;
        }
        if (fr.opcode != THERMAL_WIRE_OP_CMD_REQUEST) {
            /* A device-→-host opcode arriving on the listener is a
             * misuse, but seq is trustworthy. */
            send_acknack(THERMAL_WIRE_OP_CMD_NACK, &sender, slen,
                         fr.seq, now_ms,
                         (uint16_t)THERMAL_WIRE_STATUS_BAD_OPCODE, 0u);
            continue;
        }
        thermal_command_t cmd;
        if (thermal_wire_decode_cmd_request(&fr, &cmd) != THERMAL_WIRE_OK) {
            send_acknack(THERMAL_WIRE_OP_CMD_NACK, &sender, slen,
                         fr.seq, now_ms,
                         (uint16_t)THERMAL_WIRE_STATUS_BAD_PAYLOAD, 0u);
            continue;
        }
        thermal_command_result_t result;
        memset(&result, 0, sizeof(result));
        thermal_status_t s = thermal_core_apply_command(core, now_ms,
                                                        &cmd, &result);
        if (s == THERMAL_OK && result.status == THERMAL_OK) {
            send_acknack(THERMAL_WIRE_OP_CMD_ACK, &sender, slen,
                         fr.seq, now_ms, 0, result.detail_code);
        } else {
            uint16_t st = (s != THERMAL_OK) ? (uint16_t)s
                                            : (uint16_t)result.status;
            send_acknack(THERMAL_WIRE_OP_CMD_NACK, &sender, slen,
                         fr.seq, now_ms, st, result.detail_code);
        }
    }
}

/* =============================================================== */
/* Core callbacks                                                    */
/* =============================================================== */

static void telemetry_emit_cb(uint32_t ts_ms, uint16_t signal_id,
                              int32_t value)
{
    if (g_telemetry_fd < 0) return;
    uint8_t buf[THERMAL_WIRE_HEADER_LEN + 16];   /* sample = 8 byte payload */
    int n = thermal_wire_encode_telem_sample(buf, sizeof(buf),
                                              g_out_seq++, ts_ms,
                                              signal_id, /*flags*/ 0,
                                              value, DAEMON_CRC_ENABLED);
    if (n > 0) {
        (void)send(g_telemetry_fd, buf, (size_t)n, MSG_DONTWAIT);
    }
}

static void log_event_cb(uint32_t ts_ms, uint16_t code,
                         uint32_t a1, uint32_t a2,
                         uint32_t a3, uint32_t a4)
{
    syslog(LOG_INFO,
           "[%u ms] event 0x%04x a=(%u,%u,%u,%u)",
           ts_ms, code, a1, a2, a3, a4);
    if (g_log_stderr) {
        fprintf(stderr,
                "[%u ms] event 0x%04x a=(%u,%u,%u,%u)\n",
                ts_ms, code, a1, a2, a3, a4);
    }
    if (g_telemetry_fd >= 0) {
        uint8_t buf[THERMAL_WIRE_HEADER_LEN + 32]; /* event = 18 byte payload */
        int n = thermal_wire_encode_telem_event(buf, sizeof(buf),
                                                 g_out_seq++, ts_ms,
                                                 code, a1, a2, a3, a4,
                                                 DAEMON_CRC_ENABLED);
        if (n > 0) {
            (void)send(g_telemetry_fd, buf, (size_t)n, MSG_DONTWAIT);
        }
    }
}

/* =============================================================== */
/* Clock                                                             */
/* =============================================================== */

/* Wall-clock state */
static struct timespec g_t0;
static uint64_t        g_period_ns;
static uint64_t        g_next_deadline_ns;

static uint64_t timespec_to_ns(const struct timespec *t)
{
    return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
}

static void ns_to_timespec(uint64_t ns, struct timespec *t)
{
    t->tv_sec  = (time_t)(ns / 1000000000ull);
    t->tv_nsec = (long)(ns % 1000000000ull);
}

static int wall_clock_init(uint16_t period_ms)
{
    if (clock_gettime(CLOCK_MONOTONIC, &g_t0) != 0) return -1;
    g_period_ns        = (uint64_t)period_ms * 1000000ull;
    g_next_deadline_ns = timespec_to_ns(&g_t0) + g_period_ns;
    return 0;
}

/* Sleep until the next wall-clock tick, return now_ms (relative to t0). */
static int wall_clock_next(uint32_t *now_ms_out)
{
    struct timespec deadline;
    ns_to_timespec(g_next_deadline_ns, &deadline);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                           &deadline, NULL) == EINTR) {
        if (!g_running) return -1;
    }
    if (!g_running) return -1;

    /* now_ms = (next_deadline - t0) / 1ms */
    uint64_t rel_ns = g_next_deadline_ns - timespec_to_ns(&g_t0);
    *now_ms_out = (uint32_t)(rel_ns / 1000000ull);
    g_next_deadline_ns += g_period_ns;
    return 0;
}

/* Scenario-clock state */
static int g_scenario_listen_fd = -1;
static int g_scenario_conn_fd   = -1;
static char g_scenario_line[256];
static size_t g_scenario_pos = 0;

static int scenario_clock_init(const char *uri)
{
    /* Expect "unix:<path>". */
    if (strncmp(uri, "unix:", 5) != 0) {
        fprintf(stderr,
                "thermalcored: scenario clock URI must start with 'unix:'; got '%s'\n",
                uri);
        return -1;
    }
    const char *path = uri + 5;
    size_t path_len = strlen(path);
    if (path_len + 1 > sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr,
                "thermalcored: scenario clock path too long: '%s'\n", path);
        return -1;
    }
    if (path_len + 1 > sizeof(g_scenario_path)) {
        fprintf(stderr,
                "thermalcored: scenario clock path too long: '%s'\n", path);
        return -1;
    }
    memcpy(g_scenario_path, path, path_len + 1);

    /* Remove any stale socket file from a prior run. */
    (void)unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr,
                "thermalcored: AF_UNIX socket failed: %s\n", strerror(errno));
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr,
                "thermalcored: bind('%s') failed: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        fprintf(stderr,
                "thermalcored: listen failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    g_scenario_listen_fd = fd;

    fprintf(stderr,
            "thermalcored: waiting for scenario runner on %s ...\n", path);
    int conn = accept(fd, NULL, NULL);
    if (conn < 0) {
        if (errno != EINTR) {
            fprintf(stderr,
                    "thermalcored: accept failed: %s\n", strerror(errno));
        }
        return -1;
    }
    g_scenario_conn_fd = conn;
    return 0;
}

/* Read a newline-terminated line from the scenario connection.
 * Returns 0 on success (line in g_scenario_line, NUL-terminated,
 * newline stripped), -1 on EOF/error/EINTR-with-running-cleared. */
static int scenario_read_line(void)
{
    g_scenario_pos = 0;
    g_scenario_line[0] = '\0';
    while (g_scenario_pos + 1 < sizeof(g_scenario_line)) {
        char c;
        ssize_t r = read(g_scenario_conn_fd, &c, 1);
        if (r == 1) {
            if (c == '\n') {
                g_scenario_line[g_scenario_pos] = '\0';
                return 0;
            }
            g_scenario_line[g_scenario_pos++] = c;
            continue;
        }
        if (r == 0) return -1;            /* EOF */
        if (errno == EINTR) {
            if (!g_running) return -1;
            continue;
        }
        fprintf(stderr,
                "thermalcored: scenario read failed: %s\n", strerror(errno));
        return -1;
    }
    fprintf(stderr,
            "thermalcored: scenario line too long, dropping connection\n");
    return -1;
}

static int scenario_clock_next(uint32_t *now_ms_out)
{
    if (scenario_read_line() != 0) return -1;
    unsigned int ms = 0;
    if (sscanf(g_scenario_line, "TICK %u", &ms) != 1) {
        fprintf(stderr,
                "thermalcored: bad scenario line '%s' (want 'TICK <ms>')\n",
                g_scenario_line);
        return -1;
    }
    *now_ms_out = (uint32_t)ms;
    return 0;
}

static void scenario_clock_close(void)
{
    if (g_scenario_conn_fd >= 0) {
        close(g_scenario_conn_fd);
        g_scenario_conn_fd = -1;
    }
    if (g_scenario_listen_fd >= 0) {
        close(g_scenario_listen_fd);
        g_scenario_listen_fd = -1;
    }
    if (g_scenario_path[0] != '\0') {
        (void)unlink(g_scenario_path);
        g_scenario_path[0] = '\0';
    }
}

/* =============================================================== */
/* Config loader                                                    */
/* =============================================================== */

static char *read_whole_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr,
                "thermalcored: cannot open config '%s': %s\n",
                path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0)                    { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0){ fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz);
    if (!buf)                      { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz)           { free(buf); return NULL; }
    *out_len = n;
    return buf;
}

/* =============================================================== */
/* CLI                                                              */
/* =============================================================== */

static void print_usage(FILE *out)
{
    fprintf(out,
        "thermalcored - thermal-core Linux daemon\n"
        "\n"
        "Usage:\n"
        "  thermalcored --config=<path> [--clock=wall|scenario]\n"
        "               [--scenario-clock-uri=unix:<path>] [--log-stderr]\n"
        "\n"
        "Options:\n"
        "  --config=PATH            Required. JSON config file.\n"
        "  --clock=MODE             wall (default) or scenario.\n"
        "  --scenario-clock-uri=URI default unix:/tmp/thermalcored-clock.sock\n"
        "  --log-stderr             Tee log_event lines to stderr.\n"
        "  --help                   Print this message and exit.\n");
}

typedef enum { CLOCK_MODE_WALL, CLOCK_MODE_SCENARIO } clock_mode_t;

typedef struct {
    const char  *config_path;
    clock_mode_t clock_mode;
    const char  *scenario_uri;
    int          log_stderr;
} cli_opts_t;

static int parse_cli(int argc, char **argv, cli_opts_t *opts)
{
    opts->config_path  = NULL;
    opts->clock_mode   = CLOCK_MODE_WALL;
    opts->scenario_uri = "unix:/tmp/thermalcored-clock.sock";
    opts->log_stderr   = 0;

    static struct option longopts[] = {
        {"config",              required_argument, 0, 'c'},
        {"clock",               required_argument, 0, 'k'},
        {"scenario-clock-uri",  required_argument, 0, 'u'},
        {"log-stderr",          no_argument,       0, 's'},
        {"help",                no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (c) {
        case 'c': opts->config_path = optarg; break;
        case 'k':
            if (strcmp(optarg, "wall") == 0) {
                opts->clock_mode = CLOCK_MODE_WALL;
            } else if (strcmp(optarg, "scenario") == 0) {
                opts->clock_mode = CLOCK_MODE_SCENARIO;
            } else {
                fprintf(stderr,
                        "thermalcored: --clock must be 'wall' or 'scenario'\n");
                return -1;
            }
            break;
        case 'u': opts->scenario_uri = optarg; break;
        case 's': opts->log_stderr   = 1;      break;
        case 'h': print_usage(stdout); return 1;
        default:  return -1;
        }
    }
    if (!opts->config_path) {
        fprintf(stderr,
                "thermalcored: --config=<path> is required\n");
        return -1;
    }
    return 0;
}

/* =============================================================== */
/* Main                                                              */
/* =============================================================== */

int main(int argc, char **argv)
{
    cli_opts_t opts;
    int prc = parse_cli(argc, argv, &opts);
    if (prc != 0) {
        if (prc > 0) return 0;        /* --help */
        print_usage(stderr);
        return 2;
    }
    g_log_stderr = opts.log_stderr;

    /* === Config load ============================================= */
    size_t json_len;
    char *json = read_whole_file(opts.config_path, &json_len);
    if (!json) return 1;

    thermal_config_t           cfg;
    thermalcored_runtime_cfg_t runtime;
    char                       err[256];
    thermal_status_t cs = thermal_config_jsmn_parse(json, json_len,
                                                    &cfg, &runtime,
                                                    err, sizeof(err));
    free(json);
    if (cs != THERMAL_OK) {
        fprintf(stderr,
                "thermalcored: config rejected: %s (status=%d)\n",
                err, (int)cs);
        return 1;
    }

    /* === Signals ================================================= */
    if (install_signal_handlers() != 0) {
        fprintf(stderr,
                "thermalcored: sigaction failed: %s\n", strerror(errno));
        return 1;
    }

    /* === Syslog (always open) + UDP telemetry (best-effort) ===== */
    openlog("thermalcored", LOG_PID, LOG_DAEMON);
    g_telemetry_fd = open_telemetry_socket(runtime.global.telemetry_transport);

    /* Control plane: gated on PRD §7.3 line 945 `control.enable`.
     * A misconfigured listener (enable=true with empty / invalid URI)
     * is a startup error so the bench rig fails loudly rather than
     * running telemetry-only without anyone noticing the missing
     * command channel. */
    g_control_fd = -1;
    if (runtime.global.control_enable) {
        int fd = open_control_socket(runtime.global.control_listen);
        if (fd == -1) {
            fprintf(stderr,
                    "thermalcored: control.enable=true but control.listen "
                    "is empty; aborting\n");
            return 1;
        }
        if (fd == -2) {
            fprintf(stderr,
                    "thermalcored: control.enable=true but control.listen "
                    "is invalid; aborting\n");
            return 1;
        }
        g_control_fd = fd;
    } else {
        fprintf(stderr,
                "thermalcored: control plane disabled (control.enable=false)\n");
    }

    /* SocketCAN BSP (Stage 11 11b): activate iff exactly one
     * `runtime.contexts[i].source` begins with "canbus:".  The
     * suffix is the kernel interface name (vcan0 / can0).  Multiple
     * canbus: sources are rejected loudly; PRD §6 v1 covers a
     * single OBD-II context. */
    g_canbus_fd               = -1;
    g_canbus_context_slot     = -1;
    g_canbus_next_request_ms  = 0;
    for (uint8_t i = 0; i < runtime.context_count; i++) {
        const char *src = runtime.contexts[i].source;
        if (strncmp(src, "canbus:", 7) != 0) continue;
        if (g_canbus_context_slot >= 0) {
            fprintf(stderr,
                    "thermalcored: multiple canbus: context sources not supported in v1\n");
            return 1;
        }
        const char *iface = src + 7;
        int fd = bsp_socketcan_open(iface);
        if (fd < 0) {
            fprintf(stderr,
                    "thermalcored: canbus source on '%s' failed to open\n", iface);
            return 1;
        }
        g_canbus_fd           = fd;
        g_canbus_context_slot = (int)i;
        bsp_socketcan_state_init(&g_canbus_state,
                                  cfg.contexts[i].timeout_ms,
                                  OBD2_PID_VEHICLE_SPEED);
    }

    /* === Core init =============================================== */
    thermal_core_t              core;
    thermal_core_callbacks_t    callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.log_event      = log_event_cb;
    callbacks.telemetry_emit = telemetry_emit_cb;
    thermal_status_t is = thermal_core_init(&core, &cfg, &callbacks);
    if (is != THERMAL_OK) {
        fprintf(stderr,
                "thermalcored: thermal_core_init failed: status=%d\n", (int)is);
        return 1;
    }

    /* === Clock setup ============================================ */
    if (opts.clock_mode == CLOCK_MODE_WALL) {
        if (wall_clock_init(cfg.control_period_ms) != 0) {
            fprintf(stderr,
                    "thermalcored: wall clock init failed: %s\n", strerror(errno));
            return 1;
        }
    } else {
        if (scenario_clock_init(opts.scenario_uri) != 0) {
            return 1;
        }
    }

    /* === Main loop ============================================== */
    while (g_running) {
        uint32_t now_ms;
        int rc = (opts.clock_mode == CLOCK_MODE_WALL)
                   ? wall_clock_next(&now_ms)
                   : scenario_clock_next(&now_ms);
        if (rc != 0) break;

        /* Step 1: drain queued commands (PRD §5 Stage 9). */
        drain_commands(&core, now_ms);

        /* Step 1.5: poll the SocketCAN BSP if a canbus: context is
         * configured.  Sends the OBD-II request at 1 Hz (cadence
         * tracked inside g_canbus_next_request_ms) and drains all
         * pending response frames non-blockingly. */
        if (g_canbus_fd >= 0) {
            (void)bsp_socketcan_poll(&g_canbus_state, g_canbus_fd,
                                      now_ms, &g_canbus_next_request_ms);
        }

        /* Step 2: build snapshot from current BSP state.  Sensor +
         * tach reads always come from bsp_mock_tmpfs; context reads
         * dispatch per slot based on the source prefix
         * (canbus: -> bsp_socketcan, else -> bsp_mock_tmpfs). */
        thermal_sample_t          samples[THERMAL_MAX_SAMPLES_PER_SNAPSHOT];
        thermal_input_snapshot_t  snap;
        uint8_t samples_max = (uint8_t)(sizeof(samples) / sizeof(samples[0]));
        uint8_t need = cfg.sensor_count + cfg.actuator_count + cfg.context_count;
        if (need > samples_max) {
            fprintf(stderr,
                    "thermalcored: build_snapshot failed (samples budget too small?)\n");
            break;
        }
        uint8_t n = 0;
        int build_failed = 0;
        for (uint8_t i = 0; i < cfg.sensor_count; i++) {
            if (bsp_mock_tmpfs_read_sensor(&runtime, &cfg, i, now_ms,
                                            &samples[n]) != 0) {
                build_failed = 1; break;
            }
            n++;
        }
        if (!build_failed) {
            for (uint8_t i = 0; i < cfg.actuator_count; i++) {
                if (bsp_mock_tmpfs_read_tach(&runtime, &cfg, i, now_ms,
                                              &samples[n]) != 0) {
                    build_failed = 1; break;
                }
                n++;
            }
        }
        if (!build_failed) {
            for (uint8_t i = 0; i < cfg.context_count; i++) {
                int rc2;
                if ((int)i == g_canbus_context_slot) {
                    rc2 = bsp_socketcan_read_into_sample(&g_canbus_state,
                                                          cfg.contexts[i].id,
                                                          now_ms,
                                                          &samples[n]);
                } else {
                    rc2 = bsp_mock_tmpfs_read_context(&runtime, &cfg, i,
                                                       now_ms, &samples[n]);
                }
                if (rc2 != 0) { build_failed = 1; break; }
                n++;
            }
        }
        if (build_failed) {
            fprintf(stderr,
                    "thermalcored: build_snapshot failed (samples budget too small?)\n");
            break;
        }
        snap.now_ms       = now_ms;
        snap.samples      = samples;
        snap.sample_count = n;

        /* Step 3: core step (telemetry + log callbacks fire inside). */
        thermal_output_frame_t out;
        memset(&out, 0, sizeof(out));
        thermal_status_t ss = thermal_core_step(&core, &snap, &out);
        if (ss != THERMAL_OK) {
            fprintf(stderr,
                    "thermalcored: thermal_core_step status=%d\n", (int)ss);
        }

        /* Step 4: push actuator commands to sysfs/tmpfs. */
        (void)bsp_mock_tmpfs_write_frame(&runtime, &cfg, &out);
    }

    /* === Cleanup ================================================= */
    if (opts.clock_mode == CLOCK_MODE_SCENARIO) {
        scenario_clock_close();
    }
    if (g_telemetry_fd >= 0) {
        close(g_telemetry_fd);
        g_telemetry_fd = -1;
    }
    if (g_control_fd >= 0) {
        close(g_control_fd);
        g_control_fd = -1;
    }
    if (g_canbus_fd >= 0) {
        bsp_socketcan_close(g_canbus_fd);
        g_canbus_fd = -1;
    }
    closelog();
    return 0;
}
