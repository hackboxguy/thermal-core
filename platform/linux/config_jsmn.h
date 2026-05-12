/* platform/linux/config_jsmn.h
 *
 * Stage 9 9a — JSON-to-thermal_config_t loader (Linux daemon side).
 *
 * Parses the PRD §5.1 JSON config schema using the vendored JSMN
 * tokenizer (platform/linux/jsmn.{c,h}) and produces a fully
 * populated thermal_config_t. The loader runs the syntactic /
 * reference checks itself and then forwards to
 * thermal_core_validate_config() for the semantic rules (PRD §5.3).
 *
 * Loader scope (9a):
 *  - Parses every CORE-owned field in the schema.
 *  - Resolves zone -> sensor / actuator and modifier -> context name
 *    references to slot indexes during a second pass.
 *  - Expands telemetry wildcards (zone_temp_*, actuator_pwm_*, ...)
 *    into per-slot signal IDs from thermal_signals.h.
 *  - Silently skips fields that belong to the Linux BSP layer
 *    (sensor source, hwmon paths, telemetry.transport, control.*).
 *    Stage 9b will absorb those into a sibling runtime-cfg struct.
 *
 * The loader does not allocate from the heap; all token / scratch
 * storage is on the stack of thermal_config_jsmn_parse().
 */
#ifndef THERMAL_CONFIG_JSMN_H
#define THERMAL_CONFIG_JSMN_H

#include <stddef.h>
#include "thermal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a JSON text buffer into a thermal_config_t and validate.
 *
 *   json_text / json_len  — caller-owned buffer. Not required to be
 *                           NUL-terminated; json_len is authoritative.
 *   cfg                   — output; the function zeroes it before use
 *                           and writes only the CORE-owned fields.
 *   err_msg / err_msg_size — optional caller-provided buffer to receive
 *                            a single-line human-readable error message
 *                            on failure. May be NULL / 0 to disable.
 *
 * Error message format on failure:  "<location>: <reason>"
 *   <location>  dotted/indexed path through the JSON document
 *               (e.g. "sensors[2].iir_alpha_q16", "zones[0].trips[1]").
 *   <reason>    short prose ("value out of range", "unknown enum 'x'",
 *               "required field missing", "reference 'soc' not found").
 *
 * Returns:
 *   THERMAL_OK                 — parse + validate succeeded.
 *   THERMAL_ERR_INVALID_ARG    — malformed JSON, schema violation, or
 *                                an unresolvable name reference.
 *   THERMAL_ERR_INVALID_CONFIG — semantic validation rejected the
 *                                resulting config (validate_config).
 *   THERMAL_ERR_NO_SPACE       — token budget exceeded or an array
 *                                length exceeds its compile-time max.
 *   THERMAL_ERR_BOUNDS         — numeric value out of representable
 *                                range for its destination field.
 */
thermal_status_t thermal_config_jsmn_parse(const char *json_text,
                                           size_t json_len,
                                           thermal_config_t *cfg,
                                           char *err_msg,
                                           size_t err_msg_size);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_CONFIG_JSMN_H */
