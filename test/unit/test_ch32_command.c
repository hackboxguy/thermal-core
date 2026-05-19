/* test/unit/test_ch32_command.c
 *
 * Stage 19 19a -- host-side unit test for the CH32V003 host-command
 * parser (platform/ch32v003/ch32_command.c). The parser is pure C99
 * with no ch32fun dependency, so it builds and runs on the host; the
 * test drives ch32_command_parse() and ch32_command_feed() directly.
 */
#include <string.h>

#include "harness.h"
#include "ch32_command.h"

/* Parse one delimited line; optionally return its argument. */
static ch32_command_type_t parse(const char *line, uint16_t *arg)
{
    ch32_command_t c;
    ch32_command_parse(line, &c);
    if (arg != 0) {
        *arg = c.arg;
    }
    return c.type;
}

/* Feed an entire byte string through ch32_command_feed(); return the
 * type of the LAST completed line and, via out-params, that line's
 * argument and the count of completed lines. */
static ch32_command_type_t feed_all(const char *s, uint16_t *arg,
                                    int *completed)
{
    ch32_command_rx_t   rx;
    ch32_command_t      c;
    ch32_command_type_t last = CH32_CMD_NONE;
    int                 n    = 0;

    memset(&rx, 0, sizeof(rx));
    c.arg = 0;
    for (; *s != '\0'; s++) {
        if (ch32_command_feed(&rx, *s, &c)) {
            last = c.type;
            n++;
        }
    }
    if (arg != 0) {
        *arg = c.arg;
    }
    if (completed != 0) {
        *completed = n;
    }
    return last;
}

TEST_CASE(ch32_command)
{
    uint16_t arg;
    int      n;

    /* --- ch32_command_parse: every verb --- */
    EXPECT_EQ(parse("loop on", 0),  CH32_CMD_LOOP_ON);
    EXPECT_EQ(parse("loop off", 0), CH32_CMD_LOOP_OFF);
    EXPECT_EQ(parse("pwmget", 0),   CH32_CMD_PWMGET);
    EXPECT_EQ(parse("rpmget", 0),   CH32_CMD_RPMGET);
    EXPECT_EQ(parse("ping", 0),     CH32_CMD_PING);

    /* --- pwmset argument bounds --- */
    EXPECT_EQ(parse("pwmset 0", &arg), CH32_CMD_PWMSET);
    EXPECT_EQ(arg, 0);
    EXPECT_EQ(parse("pwmset 128", &arg), CH32_CMD_PWMSET);
    EXPECT_EQ(arg, 128);
    EXPECT_EQ(parse("pwmset 255", &arg), CH32_CMD_PWMSET);
    EXPECT_EQ(arg, 255);

    /* --- whitespace tolerance --- */
    EXPECT_EQ(parse("  ping  ", 0), CH32_CMD_PING);
    EXPECT_EQ(parse("\tpwmset\t42", &arg), CH32_CMD_PWMSET);
    EXPECT_EQ(arg, 42);

    /* --- blank lines --- */
    EXPECT_EQ(parse("", 0),    CH32_CMD_NONE);
    EXPECT_EQ(parse("   ", 0), CH32_CMD_NONE);

    /* --- errors: bad verb, bad/missing/oversize arg, trailing junk --- */
    EXPECT_EQ(parse("nope", 0),          CH32_CMD_ERROR);
    EXPECT_EQ(parse("pwm", 0),           CH32_CMD_ERROR);
    EXPECT_EQ(parse("pinger", 0),        CH32_CMD_ERROR);  /* prefix */
    EXPECT_EQ(parse("pwmset", 0),        CH32_CMD_ERROR);  /* no arg */
    EXPECT_EQ(parse("pwmset x", 0),      CH32_CMD_ERROR);  /* non-numeric */
    EXPECT_EQ(parse("pwmset 256", 0),    CH32_CMD_ERROR);  /* > 255 */
    EXPECT_EQ(parse("pwmset 9999", 0),   CH32_CMD_ERROR);
    EXPECT_EQ(parse("pwmset 1 2", 0),    CH32_CMD_ERROR);  /* extra token */
    EXPECT_EQ(parse("pwmget 5", 0),      CH32_CMD_ERROR);  /* arg on no-arg */
    EXPECT_EQ(parse("loop", 0),          CH32_CMD_ERROR);  /* no arg */
    EXPECT_EQ(parse("loop sideways", 0), CH32_CMD_ERROR);

    /* --- ch32_command_feed: the terminator drives the parse --- */
    EXPECT_EQ(feed_all("ping\n", &arg, &n), CH32_CMD_PING);
    EXPECT_EQ(n, 1);

    EXPECT_EQ(feed_all("pwmset 200\n", &arg, &n), CH32_CMD_PWMSET);
    EXPECT_EQ(arg, 200);
    EXPECT_EQ(n, 1);

    /* a byte stream with no terminator completes nothing */
    EXPECT_EQ(feed_all("ping", &arg, &n), CH32_CMD_NONE);
    EXPECT_EQ(n, 0);

    /* CRLF: '\r' ends the line, '\n' then completes an empty one */
    EXPECT_EQ(feed_all("rpmget\r\n", &arg, &n), CH32_CMD_NONE);
    EXPECT_EQ(n, 2);

    /* two commands back to back */
    EXPECT_EQ(feed_all("loop off\npwmset 10\n", &arg, &n), CH32_CMD_PWMSET);
    EXPECT_EQ(arg, 10);
    EXPECT_EQ(n, 2);

    /* an over-long line is reported as ERROR on its terminator */
    EXPECT_EQ(feed_all(
        "pwmsetpwmsetpwmsetpwmsetpwmsetpwmsetpwmset\n", &arg, &n),
        CH32_CMD_ERROR);
    EXPECT_EQ(n, 1);
}
