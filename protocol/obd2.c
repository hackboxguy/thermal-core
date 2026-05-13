/* protocol/obd2.c
 *
 * Implementation of the portable OBD-II Service 01 codec.
 * See obd2.h for the contract.
 */
#include "obd2.h"

/* ISO-TP single-frame PCI byte for a request:
 *   high nibble = 0 (single-frame)
 *   low nibble  = payload length in bytes
 * Service 01 single-byte request: 0x01 + <pid> -> 2 useful bytes. */
#define OBD2_REQUEST_PCI       0x02u

/* Same shape for a single-byte response: 0x41 + <pid> + <value> = 3. */
#define OBD2_RESPONSE_PCI      0x03u

/* ISO 15765-2 recommended padding byte. */
#define OBD2_PAD               0x55u

void obd2_encode_request_byte(uint8_t pid, uint8_t out[OBD2_FRAME_DLC])
{
    if (!out) return;
    out[0] = OBD2_REQUEST_PCI;
    out[1] = OBD2_SERVICE_01_CURRENT_DATA;
    out[2] = pid;
    out[3] = OBD2_PAD;
    out[4] = OBD2_PAD;
    out[5] = OBD2_PAD;
    out[6] = OBD2_PAD;
    out[7] = OBD2_PAD;
}

int obd2_decode_response_byte(const uint8_t *in, size_t in_len,
                              uint8_t expected_pid,
                              uint8_t *value_out)
{
    if (!in || !value_out) return OBD2_ERR_BAD_SHAPE;
    if (in_len != OBD2_FRAME_DLC) return OBD2_ERR_BAD_SHAPE;

    /* PCI byte: only ISO-TP single-frame is accepted in v1.  The
     * high nibble of the PCI byte encodes the frame type; non-zero
     * means first-frame (0x1X), consecutive-frame (0x2X), or
     * flow-control (0x3X), all of which require multi-frame
     * reassembly that v1 does not implement. */
    uint8_t pci = in[0];
    if ((pci & 0xF0u) != 0u) {
        return OBD2_ERR_BAD_SHAPE;
    }

    /* Negative-response detection comes BEFORE the wrong-service
     * check so callers can distinguish "ECU explicitly rejected us"
     * (a valid response we should log) from "wire is corrupt"
     * (potentially a transient bus issue). */
    if (in[1] == OBD2_NEGATIVE_RESPONSE) {
        return OBD2_ERR_NEGATIVE_RESPONSE;
    }

    if (in[1] != OBD2_SERVICE_01_RESPONSE) {
        return OBD2_ERR_WRONG_SERVICE;
    }

    /* For a single-byte-PID response the PCI length field must be
     * exactly 3 (service byte + PID echo + value).  Any other
     * length means we are looking at a different shape than we
     * understand — e.g. a multi-byte-PID response we are not
     * configured for — and should reject rather than read past
     * the meaningful bytes. */
    if ((pci & 0x0Fu) != OBD2_RESPONSE_PCI) {
        return OBD2_ERR_WRONG_PCI_LEN;
    }

    if (in[2] != expected_pid) {
        return OBD2_ERR_WRONG_PID;
    }

    *value_out = in[3];
    return OBD2_OK;
}
