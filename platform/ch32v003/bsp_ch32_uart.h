/* platform/ch32v003/bsp_ch32_uart.h
 *
 * Stage 18e -- CH32V003 USART1 BSP for the telemetry tap. A
 * direct-register driver in the same style as bsp_ch32_{pwm,tach,
 * sensor}.c, deliberately independent of ch32fun's printf
 * transport (FUNCONF_USE_UARTPRINTF) so the SWIO debug channel
 * stays free for the THERMALCORE_CH32_STATUS status line.
 *
 * USART1 on PD5 (TX) / PD6 (RX), 8N1. The telemetry path is
 * transmit-only; RX is configured and reserved for a future
 * host->MCU channel.
 */
#ifndef BSP_CH32_UART_H
#define BSP_CH32_UART_H

#include <stdint.h>

/* Configure USART1 (PD5 TX / PD6 RX) at `baud`, 8N1. Returns 0. */
int bsp_ch32_uart_init(uint32_t baud);

/* Transmit the NUL-terminated string `s` verbatim (no added
 * newline), blocking until each byte has been shifted out. */
void bsp_ch32_uart_puts(const char *s);

#endif /* BSP_CH32_UART_H */
