/* platform/linux/telem_wire.c
 *
 * Pure byte-packing implementation of the v1 TELEM_SAMPLE /
 * TELEM_EVENT frames.  See telem_wire.h for layout.
 *
 * Independent of <endian.h> / htole32 — the host byte order is
 * deliberately ignored; bytes are emitted little-endian by shift +
 * mask so the wire format is host-independent.
 */
#include "telem_wire.h"

static void put_u8(uint8_t *p, uint8_t v)
{
    p[0] = v;
}

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

size_t telem_wire_pack_sample(uint8_t *buf,
                              uint32_t ts_ms,
                              uint16_t signal_id,
                              int32_t  value)
{
    put_u8     (buf + 0, TELEM_WIRE_TYPE_SAMPLE);
    put_u32_le (buf + 1, ts_ms);
    put_u16_le (buf + 5, signal_id);
    put_u32_le (buf + 7, (uint32_t)value);
    return TELEM_WIRE_SAMPLE_LEN;
}

size_t telem_wire_pack_event(uint8_t *buf,
                             uint32_t ts_ms,
                             uint16_t code,
                             uint32_t a1,
                             uint32_t a2,
                             uint32_t a3,
                             uint32_t a4)
{
    put_u8     (buf +  0, TELEM_WIRE_TYPE_EVENT);
    put_u32_le (buf +  1, ts_ms);
    put_u16_le (buf +  5, code);
    put_u32_le (buf +  7, a1);
    put_u32_le (buf + 11, a2);
    put_u32_le (buf + 15, a3);
    put_u32_le (buf + 19, a4);
    return TELEM_WIRE_EVENT_LEN;
}
