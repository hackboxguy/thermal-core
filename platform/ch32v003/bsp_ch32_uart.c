/* platform/ch32v003/bsp_ch32_uart.c
 *
 * Stage 18e -- CH32V003 USART1 BSP for the telemetry tap. Register
 * sequence per ch32fun's SetupUART (CH32V003 branch), extended to
 * also configure the RX pin. Stage 19 added bsp_ch32_uart_getc()
 * for the host-command channel.
 */
#include "bsp_ch32_uart.h"

#include "ch32fun.h"

int bsp_ch32_uart_init(uint32_t baud)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD | RCC_APB2Periph_USART1;

    /* PD5 = USART1_TX: alt-function push-pull. */
    funPinMode(PD5, GPIO_CFGLR_OUT_10Mhz_AF_PP);
    /* PD6 = USART1_RX: input with pull-up (idle-high). Reserved --
     * the telemetry path is transmit-only. */
    funPinMode(PD6, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PD6, 1);

    USART1->CTLR1 = USART_WordLength_8b | USART_Parity_No |
                    USART_Mode_Tx | USART_Mode_Rx;
    USART1->CTLR2 = USART_StopBits_1;
    USART1->CTLR3 = USART_HardwareFlowControl_None;
    USART1->BRR   = (uint16_t)((FUNCONF_SYSTEM_CORE_CLOCK + baud / 2u)
                               / baud);
    USART1->CTLR1 |= CTLR1_UE_Set;
    return 0;
}

void bsp_ch32_uart_puts(const char *s)
{
    if (s == 0) {
        return;
    }
    for (; *s != '\0'; s++) {
        while (!(USART1->STATR & USART_FLAG_TC)) {
            /* wait for the shift register to drain */
        }
        USART1->DATAR = (uint16_t)(uint8_t)*s;
    }
}

int bsp_ch32_uart_getc(void)
{
    if (USART1->STATR & USART_FLAG_RXNE) {
        /* Reading DATAR clears RXNE. */
        return (int)(uint8_t)USART1->DATAR;
    }
    return -1;
}
