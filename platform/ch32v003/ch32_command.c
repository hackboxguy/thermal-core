/* platform/ch32v003/ch32_command.c
 *
 * Stage 19 -- the CH32V003 host-command parser. Pure C99 with no
 * ch32fun / hardware dependency, so it builds and runs unchanged on
 * the host for test_ch32_command.c. See ch32_command.h for the
 * grammar.
 */
#include "ch32_command.h"

/* Skip ASCII spaces and tabs. */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

/* If `lit` matches the token at *p exactly -- i.e. the char after the
 * match is a blank or the NUL terminator, so `lit` is not merely a
 * prefix of a longer word -- return the char after the token; else 0. */
static const char *match_tok(const char *p, const char *lit)
{
    while (*lit != '\0') {
        if (*p != *lit) {
            return 0;
        }
        p++;
        lit++;
    }
    if (*p == '\0' || *p == ' ' || *p == '\t') {
        return p;
    }
    return 0;
}

/* Parse an unsigned decimal in [0,255]. On success stores it into
 * *val and returns the char after the digits; on no digit or on
 * overflow past 255 returns 0. */
static const char *parse_u8(const char *p, uint16_t *val)
{
    const char *start = p;
    uint16_t    v     = 0;

    while (*p >= '0' && *p <= '9') {
        v = (uint16_t)(v * 10u + (uint16_t)(*p - '0'));
        if (v > 255u) {
            return 0;
        }
        p++;
    }
    if (p == start) {
        return 0;                 /* no digits at all */
    }
    *val = v;
    return p;
}

void ch32_command_parse(const char *line, ch32_command_t *out)
{
    const char *p = skip_ws(line);
    const char *q;

    out->arg = 0;

    if (*p == '\0') {
        out->type = CH32_CMD_NONE;
        return;
    }

    if ((q = match_tok(p, "pwmget")) != 0) {
        out->type = (*skip_ws(q) == '\0') ? CH32_CMD_PWMGET
                                          : CH32_CMD_ERROR;
        return;
    }
    if ((q = match_tok(p, "rpmget")) != 0) {
        out->type = (*skip_ws(q) == '\0') ? CH32_CMD_RPMGET
                                          : CH32_CMD_ERROR;
        return;
    }
    if ((q = match_tok(p, "ping")) != 0) {
        out->type = (*skip_ws(q) == '\0') ? CH32_CMD_PING
                                          : CH32_CMD_ERROR;
        return;
    }
    if ((q = match_tok(p, "pwmset")) != 0) {
        uint16_t v = 0;
        q = parse_u8(skip_ws(q), &v);
        if (q != 0 && *skip_ws(q) == '\0') {
            out->type = CH32_CMD_PWMSET;
            out->arg  = v;
        } else {
            out->type = CH32_CMD_ERROR;
        }
        return;
    }
    if ((q = match_tok(p, "loop")) != 0) {
        const char *a = skip_ws(q);
        const char *r;
        if ((r = match_tok(a, "on")) != 0 && *skip_ws(r) == '\0') {
            out->type = CH32_CMD_LOOP_ON;
        } else if ((r = match_tok(a, "off")) != 0 &&
                   *skip_ws(r) == '\0') {
            out->type = CH32_CMD_LOOP_OFF;
        } else {
            out->type = CH32_CMD_ERROR;
        }
        return;
    }

    out->type = CH32_CMD_ERROR;
}

int ch32_command_feed(ch32_command_rx_t *rx, char c, ch32_command_t *out)
{
    if (c == '\n' || c == '\r') {
        if (rx->overflowed) {
            out->type = CH32_CMD_ERROR;
            out->arg  = 0;
        } else {
            rx->buf[rx->len] = '\0';
            ch32_command_parse(rx->buf, out);
        }
        rx->len        = 0;
        rx->overflowed = 0;
        return 1;
    }

    if (rx->len < (uint8_t)(CH32_CMD_LINE_MAX - 1)) {
        rx->buf[rx->len++] = c;
    } else {
        rx->overflowed = 1;       /* line too long: -err on the \n */
    }
    return 0;
}
