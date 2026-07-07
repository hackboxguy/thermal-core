/* test/unit/test_ch32_callbacks.c
 *
 * Host-side test for the CH32 telemetry callback ring. The real
 * firmware writes to USART1; this test provides a tiny fake UART sink
 * and drives enough callbacks to overflow the nonblocking ring.
 */
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "ch32_callbacks.h"
#include "thermal_signals.h"

static char   g_uart[4096];
static size_t g_uart_len;

int bsp_ch32_uart_init(uint32_t baud)
{
    (void)baud;
    return 0;
}

void bsp_ch32_uart_puts(const char *s)
{
    size_t n = strlen(s);
    if (g_uart_len + n < sizeof(g_uart)) {
        memcpy(&g_uart[g_uart_len], s, n + 1u);
        g_uart_len += n;
    }
}

int bsp_ch32_uart_getc(void)
{
    return -1;
}

static int capture_has_drop_count(uint32_t drops)
{
    char needle[96];
    (void)snprintf(needle, sizeof needle, ",S,%u,%" PRIu32 ",0,0,0,0,0\n",
                   (unsigned)TSIG_PLATFORM_CH32_TELEMETRY_DROPS, drops);
    return strstr(g_uart, needle) != 0;
}

TEST_CASE(ch32_callbacks)
{
    for (uint32_t i = 0; i < 20u; i++) {
        ch32_telemetry_emit_cb(100u + i, TSIG_ZONE_TEMP(0), (int32_t)i);
    }
    EXPECT_EQ(ch32_telemetry_drop_count() > 0u, 1);
    EXPECT_EQ(ch32_telemetry_drop_count(), 5u);

    ch32_telemetry_drain();
    EXPECT_EQ(capture_has_drop_count(5u), 1);

    size_t len_after_first_drain = g_uart_len;
    ch32_telemetry_drain();
    EXPECT_EQ(g_uart_len, len_after_first_drain);

    for (uint32_t i = 0; i < 16u; i++) {
        ch32_log_event_cb(200u + i, 0x1234u, i, 0u, 0u, 0u);
    }
    EXPECT_EQ(ch32_telemetry_drop_count(), 6u);

    ch32_telemetry_drain();
    EXPECT_EQ(capture_has_drop_count(6u), 1);
}
