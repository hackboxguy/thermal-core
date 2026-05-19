/* platform/ch32v003/ch32_command.h
 *
 * Stage 19 -- line-buffered ASCII command parser for the CH32V003
 * host-command channel (the bench-build characterisation path).
 *
 * Pure C99, no hardware dependency: the byte accumulator and the
 * line parser compile and run on the host, where test_ch32_command.c
 * exercises them directly. main.c feeds USART1 RX bytes in and turns
 * each parsed command into a BSP effect plus a response line.
 *
 * Grammar (one command per line, terminated by '\n' or '\r'):
 *   loop on | loop off | pwmset <0-255> | pwmget | rpmget | ping
 * Leading / trailing / inter-token blanks are tolerated; any other
 * input -- unknown verb, bad or missing argument, trailing token,
 * over-long line -- parses to CH32_CMD_ERROR.
 */
#ifndef CH32_COMMAND_H
#define CH32_COMMAND_H

#include <stdint.h>

/* Longest command line accepted, terminator excluded. "pwmset 255"
 * is 10 chars; 31 usable leaves generous slack. A longer line is
 * reported as CH32_CMD_ERROR. */
#define CH32_CMD_LINE_MAX 32

typedef enum {
    CH32_CMD_NONE = 0,   /* blank line -- nothing to do */
    CH32_CMD_LOOP_ON,    /* resume the regulating control loop */
    CH32_CMD_LOOP_OFF,   /* bypass the control loop (manual PWM) */
    CH32_CMD_PWMSET,     /* arg = requested duty, 0..255 */
    CH32_CMD_PWMGET,
    CH32_CMD_RPMGET,
    CH32_CMD_PING,
    CH32_CMD_ERROR       /* unrecognised verb / bad argument / overflow */
} ch32_command_type_t;

typedef struct {
    ch32_command_type_t type;
    uint16_t            arg;   /* PWMSET duty; 0 for every other type */
} ch32_command_t;

/* Byte accumulator. Zero-initialise (e.g. `= {0}` or memset) before
 * the first ch32_command_feed() call. */
typedef struct {
    char    buf[CH32_CMD_LINE_MAX];
    uint8_t len;
    uint8_t overflowed;   /* current line already exceeded the buffer */
} ch32_command_rx_t;

/* Feed one received byte. On a line terminator ('\n' or '\r') the
 * accumulated line is parsed into *out, the accumulator is reset, and
 * 1 is returned. Otherwise the byte is buffered and 0 is returned. A
 * blank line yields CH32_CMD_NONE; an over-long line CH32_CMD_ERROR. */
int ch32_command_feed(ch32_command_rx_t *rx, char c, ch32_command_t *out);

/* Parse one already-delimited line (no terminator). Pure -- shared by
 * ch32_command_feed() and exercised directly by the host unit test. */
void ch32_command_parse(const char *line, ch32_command_t *out);

#endif /* CH32_COMMAND_H */
