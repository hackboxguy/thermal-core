/* platform/ch32v003/bsp_ch32_uart.c
 *
 * Stage 18e -- CH32V003 USART1 BSP for the telemetry tap. Register
 * sequence per ch32fun's SetupUART (CH32V003 branch), extended to
 * also configure the RX pin.
 *
 * Stage 19 added the host-command channel. RX is interrupt-driven:
 * the USART1 RXNE interrupt feeds a ring buffer that
 * bsp_ch32_uart_getc() drains from the main loop. Polling alone
 * cannot work here -- the regulating tick blocks ~800 ms in the
 * DS18B20 conversion, during which a polled reader would lose every
 * byte of a host command (the receiver holds only one). The whole
 * RX-interrupt path is compiled only for the command build; the
 * default / telemetry firmware is byte-for-byte unchanged.
 */
#include "bsp_ch32_uart.h"

#include "ch32fun.h"

#if THERMALCORE_CH32_COMMAND
/* RX ring buffer: the USART1 interrupt is the sole producer,
 * bsp_ch32_uart_getc() the sole consumer, so no lock is needed.
 * 64 bytes is many times the longest command line. */
#define UART_RX_RING_SZ 64u
static volatile uint8_t rx_ring[UART_RX_RING_SZ];
static volatile uint8_t rx_head;            /* ISR advances  */
static volatile uint8_t rx_tail;            /* getc advances */

void USART1_IRQHandler(void) __attribute__((interrupt));
void USART1_IRQHandler(void)
{
    /* Drain every byte the receiver holds. Reading STATR then DATAR
     * clears RXNE plus the error flags. A byte that arrived with a
     * framing / noise / parity / overrun error is line noise -- on an
     * idle RX line that is typically crosstalk coupling the adjacent
     * TX pin in -- so it is dropped rather than fed to the command
     * parser as garbage. A byte that would overflow the ring is
     * dropped too. */
    for (;;) {
        uint16_t st = USART1->STATR;
        if (!(st & USART_STATR_RXNE)) {
            break;
        }
        uint8_t b = (uint8_t)USART1->DATAR;
        if (st & (USART_STATR_FE | USART_STATR_NE |
                  USART_STATR_PE | USART_STATR_ORE)) {
            continue;                       /* corrupt byte -- drop */
        }
        uint8_t next = (uint8_t)((rx_head + 1u) % UART_RX_RING_SZ);
        if (next != rx_tail) {
            rx_ring[rx_head] = b;
            rx_head = next;
        }
    }
}
#endif /* THERMALCORE_CH32_COMMAND */

int bsp_ch32_uart_init(uint32_t baud)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD | RCC_APB2Periph_USART1;

    /* PD5 = USART1_TX: alt-function push-pull. */
    funPinMode(PD5, GPIO_CFGLR_OUT_10Mhz_AF_PP);
    /* PD6 = USART1_RX: input with pull-up (idle-high). Carries the
     * Stage 19 host-command channel; unused by the telemetry tap. */
    funPinMode(PD6, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PD6, 1);

    USART1->CTLR1 = USART_WordLength_8b | USART_Parity_No |
                    USART_Mode_Tx | USART_Mode_Rx;
    USART1->CTLR2 = USART_StopBits_1;
    USART1->CTLR3 = USART_HardwareFlowControl_None;
    USART1->BRR   = (uint16_t)((FUNCONF_SYSTEM_CORE_CLOCK + baud / 2u)
                               / baud);
    USART1->CTLR1 |= CTLR1_UE_Set;
#if THERMALCORE_CH32_COMMAND
    /* Enable the RX-not-empty interrupt so command bytes are captured
     * even while the control loop blocks in the DS18B20 conversion. */
    USART1->CTLR1 |= USART_CTLR1_RXNEIE;
    NVIC_EnableIRQ(USART1_IRQn);
#endif
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
#if THERMALCORE_CH32_COMMAND
    /* Pop one byte from the interrupt-fed RX ring. */
    if (rx_tail == rx_head) {
        return -1;
    }
    {
        uint8_t b = rx_ring[rx_tail];
        rx_tail = (uint8_t)((rx_tail + 1u) % UART_RX_RING_SZ);
        return (int)b;
    }
#else
    /* No command channel: a direct polled read. Unreferenced in the
     * default / telemetry firmware, so --gc-sections strips it. */
    if (USART1->STATR & USART_FLAG_RXNE) {
        return (int)(uint8_t)USART1->DATAR;
    }
    return -1;
#endif
}
