/* test/fuzz/fuzz_wire.c
 *
 * libFuzzer harness over the canonical TC wire decoder
 * (`protocol/thermal_wire.c`).  Built only by the `fuzz-wire`
 * Makefile target (requires clang).  Seed corpus lives in
 * test/fuzz/wire-seeds/; growing corpus + crash artifacts go
 * under build/fuzz/wire-{corpus,artifacts}/.
 *
 * Contract: the decoder must never crash regardless of input.
 * Both CRC and no-CRC modes are exercised per input; if the
 * outer frame decode succeeds, the matching per-opcode payload
 * decoder is also called so its length / opcode-check paths are
 * exercised.
 */
#include <stddef.h>
#include <stdint.h>

#include "thermal_commands.h"
#include "thermal_wire.h"
#include "thermal_wire_opcodes.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    thermal_wire_frame_t fr;
    for (int crc = 0; crc <= 1; crc++) {
        int rc = thermal_wire_decode_frame(data, size,
                                            THERMAL_WIRE_MAX_LINUX,
                                            crc, &fr);
        if (rc != THERMAL_WIRE_OK) continue;
        switch (fr.opcode) {
        case THERMAL_WIRE_OP_TELEM_SAMPLE: {
            uint16_t sig, flags; int32_t val;
            (void)thermal_wire_decode_telem_sample(&fr, &sig, &flags, &val);
            break;
        }
        case THERMAL_WIRE_OP_TELEM_EVENT: {
            uint16_t code; uint32_t a1, a2, a3, a4;
            (void)thermal_wire_decode_telem_event(&fr, &code, &a1, &a2, &a3, &a4);
            break;
        }
        case THERMAL_WIRE_OP_CMD_REQUEST: {
            thermal_command_t cmd;
            (void)thermal_wire_decode_cmd_request(&fr, &cmd);
            break;
        }
        case THERMAL_WIRE_OP_CMD_ACK:
        case THERMAL_WIRE_OP_CMD_NACK: {
            uint16_t req, st; uint32_t det;
            (void)thermal_wire_decode_cmd_ack_or_nack(&fr, &req, &st, &det);
            break;
        }
        default: break;
        }
    }
    return 0;
}
