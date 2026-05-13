/* platform/linux/bsp_socketcan.c
 *
 * Implementation of the Linux SocketCAN BSP.  See bsp_socketcan.h
 * for the contract.
 */
/* POSIX feature test for read / write / close + struct ifreq via
 * <net/if.h>.  Same shape as thermalcored.c.
 * NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
#define _POSIX_C_SOURCE 200809L

#include "bsp_socketcan.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>

#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "obd2.h"

/* === Cached state ========================================== */

void bsp_socketcan_state_init(bsp_socketcan_state_t *state,
                              uint16_t timeout_ms,
                              uint8_t  expected_pid)
{
    if (!state) return;
    state->last_value_ms = 0;
    state->last_value    = 0;
    state->timeout_ms    = timeout_ms;
    state->expected_pid  = expected_pid;
    state->ever_received = 0;
}

void bsp_socketcan_handle_frame(bsp_socketcan_state_t *state,
                                uint32_t can_id,
                                const uint8_t *data, size_t len,
                                uint32_t now_ms)
{
    if (!state || !data) return;
    if (can_id != OBD2_RESPONSE_ID_ECU) return;

    uint8_t value = 0;
    int rc = obd2_decode_response_byte(data, len,
                                        state->expected_pid, &value);
    if (rc != OBD2_OK) {
        /* Decoder failure: leave the cache untouched.  The
         * staleness window will eventually expire and the sample
         * will report valid=0 (PRD §6.3 fail-safe). */
        return;
    }
    state->last_value    = (int32_t)value;
    state->last_value_ms = now_ms;
    state->ever_received = 1;
}

int bsp_socketcan_read_into_sample(const bsp_socketcan_state_t *state,
                                   uint16_t sample_id,
                                   uint32_t now_ms,
                                   thermal_sample_t *out)
{
    if (!state || !out) return -1;
    out->id           = sample_id;
    out->kind         = THERMAL_SAMPLE_CONTEXT_I32;
    out->sample_ts_ms = now_ms;
    out->quality      = 0;

    /* Cold start: no successful decode yet.  Report stale so the
     * acoustic_mask modifier falls back to assume_stationary
     * rather than treating a zeroed cache as "vehicle at rest". */
    if (!state->ever_received) {
        out->valid = 0;
        out->value = 0;
        return 0;
    }

    /* Staleness check.  Use signed math to avoid wraparound
     * surprises if now_ms < last_value_ms (clock skew during
     * --clock=scenario replays). */
    uint32_t age = now_ms - state->last_value_ms;
    if (state->timeout_ms != 0 && age > state->timeout_ms) {
        out->valid = 0;
        out->value = 0;
    } else {
        out->valid = 1;
        out->value = state->last_value;
    }
    return 0;
}

/* === SocketCAN plumbing ==================================== */

int bsp_socketcan_open(const char *iface)
{
    if (!iface || iface[0] == '\0') {
        fprintf(stderr, "bsp_socketcan: empty interface name\n");
        return -1;
    }

    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        fprintf(stderr, "bsp_socketcan: socket(PF_CAN) failed: %s\n",
                strerror(errno));
        return -1;
    }

    /* POSIX if_nametoindex (rather than the BSD-extension SIOCGIFINDEX
     * ioctl + struct ifreq) so the .c stays under -D_POSIX_C_SOURCE. */
    unsigned int ifindex = if_nametoindex(iface);
    if (ifindex == 0u) {
        fprintf(stderr,
                "bsp_socketcan: if_nametoindex('%s') failed: %s\n",
                iface, strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = (int)ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr,
                "bsp_socketcan: bind('%s') failed: %s\n",
                iface, strerror(errno));
        close(fd);
        return -1;
    }

    /* Accept only the ECU response ID, so unrelated bus traffic
     * (other ECUs, request echoes) never reaches our recvfrom. */
    struct can_filter filt;
    filt.can_id   = OBD2_RESPONSE_ID_ECU;
    filt.can_mask = CAN_SFF_MASK;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER,
                   &filt, sizeof(filt)) != 0) {
        fprintf(stderr,
                "bsp_socketcan: setsockopt(CAN_RAW_FILTER) failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }

    fprintf(stderr, "bsp_socketcan: listening on '%s'\n", iface);
    return fd;
}

/* Build + send one OBD-II request to OBD2_REQUEST_ID_FUNCTIONAL.
 * Returns 0 on success, -1 on a fatal socket error (caller should
 * close + reopen). */
static int send_request(int fd, uint8_t pid)
{
    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id  = OBD2_REQUEST_ID_FUNCTIONAL;
    frame.can_dlc = OBD2_FRAME_DLC;
    obd2_encode_request_byte(pid, frame.data);

    ssize_t w = write(fd, &frame, sizeof(frame));
    if (w == (ssize_t)sizeof(frame)) return 0;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* Tx queue full; try again next tick.  Not fatal. */
        return 0;
    }
    fprintf(stderr, "bsp_socketcan: write failed: %s\n", strerror(errno));
    return -1;
}

int bsp_socketcan_poll(bsp_socketcan_state_t *state, int fd,
                       uint32_t now_ms,
                       uint32_t *next_request_at_ms)
{
    if (!state || fd < 0 || !next_request_at_ms) return -1;

    /* 1 Hz request cadence (PRD §6.2). */
    if (now_ms >= *next_request_at_ms) {
        if (send_request(fd, state->expected_pid) != 0) {
            return -1;
        }
        *next_request_at_ms = now_ms + 1000u;
    }

    /* Drain pending responses non-blockingly.  recvfrom returns
     * EAGAIN once the queue empties; that's our exit condition. */
    for (;;) {
        struct can_frame frame;
        ssize_t r = recv(fd, &frame, sizeof(frame), MSG_DONTWAIT);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            fprintf(stderr,
                    "bsp_socketcan: recv failed: %s\n", strerror(errno));
            return -1;
        }
        if (r < (ssize_t)sizeof(struct can_frame)) {
            /* CAN-FD or partial read; skip.  CAN classic recv
             * always returns a full frame. */
            continue;
        }
        bsp_socketcan_handle_frame(state, frame.can_id,
                                    frame.data, frame.can_dlc, now_ms);
    }
    return 0;
}

void bsp_socketcan_close(int fd)
{
    if (fd >= 0) (void)close(fd);
}
