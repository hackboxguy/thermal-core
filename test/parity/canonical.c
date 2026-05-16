/* test/parity/canonical.c -- see canonical.h for the contract. */
#include "canonical.h"

#include <inttypes.h>
#include <stdio.h>

const char THERMALCORE_CANONICAL_HEADER[] =
    "ts_ms,row_type,id,value,flags_or_status,a1,a2,a3,a4\n";

int thermalcore_canonical_sample(char *buf, size_t cap,
                                 uint32_t ts_ms, uint16_t signal_id,
                                 int32_t value, uint16_t flags)
{
    int n = snprintf(buf, cap,
                     "%" PRIu32 ",S,%u,%" PRId32 ",%u,0,0,0,0\n",
                     ts_ms, (unsigned)signal_id, value,
                     (unsigned)flags);
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

int thermalcore_canonical_event(char *buf, size_t cap,
                                uint32_t ts_ms, uint16_t code,
                                uint32_t a1, uint32_t a2,
                                uint32_t a3, uint32_t a4)
{
    int n = snprintf(buf, cap,
                     "%" PRIu32 ",E,%u,0,0,%" PRIu32 ",%" PRIu32
                     ",%" PRIu32 ",%" PRIu32 "\n",
                     ts_ms, (unsigned)code, a1, a2, a3, a4);
    return (n > 0 && (size_t)n < cap) ? n : 0;
}
