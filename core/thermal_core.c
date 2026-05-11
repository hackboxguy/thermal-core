/* core/thermal_core.c
 *
 * Stage 1 stubs. All five v1 API functions return THERMAL_ERR_UNAVAILABLE;
 * Stage 2 onward fills in real bodies module by module.
 *
 * The internal struct (thermal_core_internal_t) is the real backing store
 * for thermal_core_t. It's empty in Stage 1; each subsequent stage adds
 * the fields its module needs. The C99 negative-array trick below keeps
 * the internal struct within the reserved buffer at compile time; raising
 * THERMAL_CORE_T_RESERVED_BYTES is a deliberate PR with rationale.
 */
#include "thermal_core.h"

typedef struct {
    /* Stage 2+ adds: zone runtime state, sensor IIR filter state,
     * actuator slew state, PID integrator/derivative history, fault
     * detector state machines, context filter state, modifier state,
     * callbacks copy, const config pointer. */
    char _placeholder;          /* C99 requires at least one member */
} thermal_core_internal_t;

/* Compile-time fit check (C99 idiom). Negative array size triggers a
 * compile error if the internal struct outgrows the reserved buffer. */
typedef char thermal_core_t_fits[
    (sizeof(thermal_core_internal_t) <= sizeof(thermal_core_t)) ? 1 : -1];

thermal_status_t thermal_core_validate_config(const thermal_config_t *cfg) {
    (void)cfg;
    return THERMAL_ERR_UNAVAILABLE;
}

thermal_status_t thermal_core_init(thermal_core_t *ctx,
                                   const thermal_config_t *cfg,
                                   const thermal_core_callbacks_t *cb) {
    (void)ctx; (void)cfg; (void)cb;
    return THERMAL_ERR_UNAVAILABLE;
}

thermal_status_t thermal_core_step(thermal_core_t *ctx,
                                   const thermal_input_snapshot_t *in,
                                   thermal_output_frame_t *out) {
    (void)ctx; (void)in; (void)out;
    return THERMAL_ERR_UNAVAILABLE;
}

thermal_status_t thermal_core_apply_command(thermal_core_t *ctx,
                                            uint32_t now_ms,
                                            const thermal_command_t *cmd,
                                            thermal_command_result_t *result) {
    (void)ctx; (void)now_ms; (void)cmd; (void)result;
    return THERMAL_ERR_UNAVAILABLE;
}

thermal_status_t thermal_core_get_state(const thermal_core_t *ctx,
                                        thermal_state_snapshot_t *state) {
    (void)ctx; (void)state;
    return THERMAL_ERR_UNAVAILABLE;
}
