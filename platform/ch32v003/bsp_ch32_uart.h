/* platform/ch32v003/bsp_ch32_uart.h
 *
 * Stage 18e -- CH32V003 USART1 BSP for the telemetry tap. A
 * direct-register driver in the same style as bsp_ch32_{pwm,tach,
 * sensor}.c, deliberately independent of ch32fun's printf
 * transport (FUNCONF_USE_UARTPRINTF) so the SWIO debug channel
 * stays free for the THERMALCORE_CH32_STATUS status line.
 *
 * USART1 on PD5 (TX) / PD6 (RX), 8N1. The telemetry tap is
 * transmit-only; Stage 19 added bsp_ch32_uart_getc() so the
 * host-command channel can read the RX line.
 */
#ifndef BSP_CH32_UART_H
#define BSP_CH32_UART_H

#include <stdint.h>

/* Configure USART1 (PD5 TX / PD6 RX) at `baud`, 8N1. Returns 0. */
int bsp_ch32_uart_init(uint32_t baud);

/* Transmit the NUL-terminated string `s` verbatim (no added
 * newline), blocking until each byte has been shifted out. */
void bsp_ch32_uart_puts(const char *s);

/* Non-blocking single-byte read for the Stage 19 host-command
 * channel. In the command build it pops the interrupt-fed RX ring;
 * otherwise it is a direct polled read, unreferenced by -- and so
 * --gc-sections-stripped from -- the default firmware. Returns the
 * byte (0..255), or -1 when none is waiting. */
int bsp_ch32_uart_getc(void);

#endif /* BSP_CH32_UART_H */
