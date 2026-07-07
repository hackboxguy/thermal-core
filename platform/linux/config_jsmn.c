/* platform/linux/config_jsmn.c
 *
 * Stage 9 9a — JSON-to-thermal_config_t loader.
 *
 * Two-pass walker over JSMN tokens:
 *
 *   Pass A: populates sensors / context_signals / actuators arrays.
 *           Each entry gets its CORE-owned fields filled in.
 *   Pass B: populates zones / policy_modifiers / faults / telemetry.
 *           Name references in zones (sensors/actuators) and modifiers
 *           (context) are resolved to slot indexes via linear scan of
 *           the pass-A arrays.
 *
 * The loader then calls thermal_core_validate_config() for PRD §5.3
 * semantic rules (ID uniqueness, curve monotonicity, PWM bounds,
 * telemetry signal-id support, etc.).
 *
 * Platform-only fields (sensor source, hwmon paths, telemetry transport,
 * control listener, ...) are recognised by an explicit allowlist and
 * silently skipped; Stage 9b will absorb them into a sibling runtime
 * cfg struct. Unknown CORE fields outside the allowlist produce a
 * "<location>: unknown key '<name>'" error.
 *
 * Error messages have the form "<dotted-path>: <reason>" and are
 * snprintf'd into the caller's err_msg buffer if provided.
 */
#include "config_jsmn.h"
#include "runtime_cfg.h"

/* Match jsmn.c's compile-time configuration so the jsmntok_t struct
 * layout (including the `parent` field) is the same in both TUs. */
#define JSMN_PARENT_LINKS
#define JSMN_STRICT
#include "jsmn.h"

#include "thermal_core.h"
#include "thermal_config.h"
#include "thermal_signals.h"
#include "thermal_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>

/* =============================================================== */
/* Parsing context                                                  */
/* =============================================================== */

#define JSMN_MAX_TOKENS    4096
#define PATH_MAX_LEN        256

typedef struct {
    const char *json;
    jsmntok_t  *toks;
    int         n_toks;
    char       *err;
    size_t      err_size;
    /* Loader scratch: number of state_pwm entries the JSON
     * explicitly listed per actuator slot.  Lets parse_zone reject
     * a trip whose cooling_state falls past the explicit length,
     * which the tail-zero-fill in the C struct would otherwise
     * disguise as a legal "off" (PRD §5.3 line 805). */
    uint8_t     state_pwm_lens[THERMAL_MAX_ACTUATORS];
} parse_ctx_t;

/* =============================================================== */
/* Error helpers                                                    */
/* =============================================================== */

static thermal_status_t set_err(parse_ctx_t *ctx,
                                thermal_status_t code,
                                const char *path,
                                const char *reason)
{
    if (ctx->err && ctx->err_size > 0) {
        snprintf(ctx->err, ctx->err_size, "%s: %s", path, reason);
    }
    return code;
}

/* Same as set_err but with a printf-style reason. */
static thermal_status_t set_errf(parse_ctx_t *ctx,
                                 thermal_status_t code,
                                 const char *path,
                                 const char *fmt,
                                 const char *arg)
{
    if (ctx->err && ctx->err_size > 0) {
        char reason[128];
        snprintf(reason, sizeof(reason), fmt, arg);
        snprintf(ctx->err, ctx->err_size, "%s: %s", path, reason);
    }
    return code;
}

/* =============================================================== */
/* Token primitives                                                 */
/* =============================================================== */

/* Return the index past the value at idx, recursing through any
 * nested OBJECT/ARRAY tokens.  Bounded by n_toks so a malformed
 * JSON producing a token with a corrupted .size value can't walk
 * past the valid range.  All callers that index into ctx->toks
 * with the returned value are themselves guarded by tok_in_range. */
static int skip_token_n(const jsmntok_t *toks, int n_toks, int idx)
{
    if (idx < 0 || idx >= n_toks) return n_toks;
    int j = idx + 1;
    if (toks[idx].type == JSMN_OBJECT) {
        for (int i = 0; i < toks[idx].size && j < n_toks; i++) {
            j = j + 1;             /* skip key (string, no recursion) */
            j = skip_token_n(toks, n_toks, j);
        }
    } else if (toks[idx].type == JSMN_ARRAY) {
        for (int i = 0; i < toks[idx].size && j < n_toks; i++) {
            j = skip_token_n(toks, n_toks, j);
        }
    }
    return j;
}

/* Wrapper: pulls n_toks from the parse_ctx_t.  Almost all call sites
 * have ctx in scope, so this keeps them concise. */
#define skip_token(toks, idx)  skip_token_n((toks), ctx->n_toks, (idx))

/* Return non-zero if token index `t` is within the parsed token
 * array AND its start/end form a usable range.  Used by every
 * primitive accessor below so a malformed JSON that confuses JSMN's
 * accounting (object/key/value mismatches) can never walk into
 * uninitialised tokens past ctx->n_toks. */
static int tok_in_range(const parse_ctx_t *ctx, int t)
{
    if (t < 0 || t >= ctx->n_toks) return 0;
    const jsmntok_t *tok = &ctx->toks[t];
    if (tok->start < 0 || tok->end < tok->start) return 0;
    return 1;
}

/* Compare a JSMN_STRING token to a C string literal.  Returns 0 if
 * `t` is out of range, not a string, or content differs. */
static int tok_str_eq(const parse_ctx_t *ctx, int t, const char *s)
{
    if (!tok_in_range(ctx, t)) return 0;
    const jsmntok_t *tok = &ctx->toks[t];
    if (tok->type != JSMN_STRING) return 0;
    size_t len = (size_t)(tok->end - tok->start);
    size_t sl  = strlen(s);
    return len == sl && memcmp(ctx->json + tok->start, s, len) == 0;
}

/* Copy a JSMN_STRING token's contents into a fixed-size buffer.
 * Returns 0 on success, -1 if `t` is out of range, not a string, or
 * the string doesn't fit. */
static int tok_str_copy(const parse_ctx_t *ctx, int t, char *dst, size_t dst_sz)
{
    if (!tok_in_range(ctx, t)) return -1;
    const jsmntok_t *tok = &ctx->toks[t];
    if (tok->type != JSMN_STRING) return -1;
    size_t len = (size_t)(tok->end - tok->start);
    if (len + 1 > dst_sz) return -1;
    memcpy(dst, ctx->json + tok->start, len);
    dst[len] = '\0';
    return 0;
}

/* Parse a JSMN_PRIMITIVE token as a signed long long. Returns 0 on
 * success, -1 if the token is out of range, isn't a numeric primitive,
 * or doesn't fit in 32 chars. */
static int tok_parse_long(const parse_ctx_t *ctx, int t, long long *out)
{
    if (!tok_in_range(ctx, t)) return -1;
    const jsmntok_t *tok = &ctx->toks[t];
    if (tok->type != JSMN_PRIMITIVE) return -1;
    size_t len = (size_t)(tok->end - tok->start);
    if (len == 0 || len >= 32) return -1;
    char buf[32];
    memcpy(buf, ctx->json + tok->start, len);
    buf[len] = '\0';
    /* Reject true / false / null primitives. */
    char first = buf[0];
    if (first == 't' || first == 'f' || first == 'n') return -1;
    char *endp;
    long long v = strtoll(buf, &endp, 10);
    if (endp == buf || *endp != '\0') return -1;
    *out = v;
    return 0;
}

/* Parse a JSMN_PRIMITIVE token as a boolean (true/false or 0/1).
 * Returns 0 on success, -1 otherwise. */
static int tok_parse_bool(const parse_ctx_t *ctx, int t, int *out)
{
    if (!tok_in_range(ctx, t)) return -1;
    const jsmntok_t *tok = &ctx->toks[t];
    if (tok->type != JSMN_PRIMITIVE) return -1;
    size_t len = (size_t)(tok->end - tok->start);
    const char *s = ctx->json + tok->start;
    if (len == 4 && memcmp(s, "true",  4) == 0) { *out = 1; return 0; }
    if (len == 5 && memcmp(s, "false", 5) == 0) { *out = 0; return 0; }
    long long v;
    if (tok_parse_long(ctx, t, &v) == 0 && (v == 0 || v == 1)) {
        *out = (int)v;
        return 0;
    }
    return -1;
}

/* Bounded-range integer parse. lo/hi are `long long` to portably cover
 * uint32_t-width destination fields without depending on `long` being
 * 64-bit. */
static int tok_parse_int_range(const parse_ctx_t *ctx, int t,
                               long long lo, long long hi, long long *out)
{
    long long v;
    if (tok_parse_long(ctx, t, &v) != 0) return -1;
    if (v < lo || v > hi) return -2;
    *out = v;
    return 0;
}

/* =============================================================== */
/* Enum maps                                                        */
/* =============================================================== */

typedef struct { const char *name; int value; } enum_map_t;

static const enum_map_t AGGREGATION_MAP[] = {
    { "max",      THERMAL_AGG_MAX      },
    { "avg",      THERMAL_AGG_AVG      },
    { "weighted", THERMAL_AGG_WEIGHTED },
    { NULL, 0 }
};

static const enum_map_t GOVERNOR_MAP[] = {
    { "step_wise", THERMAL_GOVERNOR_STEP_WISE },
    { "pid",       THERMAL_GOVERNOR_PID       },
    { NULL, 0 }
};

static const enum_map_t TRIP_SEVERITY_MAP[] = {
    { "warn",     THERMAL_TRIP_WARN     },
    { "critical", THERMAL_TRIP_CRITICAL },
    { "shutdown", THERMAL_TRIP_SHUTDOWN },
    { NULL, 0 }
};

static const enum_map_t CONTEXT_UNIT_MAP[] = {
    { "none",       THERMAL_CONTEXT_UNIT_NONE       },
    { "kmh",        THERMAL_CONTEXT_UNIT_KMH        },
    { "bool",       THERMAL_CONTEXT_UNIT_BOOL       },
    { "rpm",        THERMAL_CONTEXT_UNIT_RPM        },
    { "celsius_mc", THERMAL_CONTEXT_UNIT_CELSIUS_MC },
    { NULL, 0 }
};

static const enum_map_t FAILSAFE_MAP[] = {
    { "assume_stationary", THERMAL_FAILSAFE_ASSUME_STATIONARY },
    { "hold_last",         THERMAL_FAILSAFE_HOLD_LAST         },
    { "assume_value",      THERMAL_FAILSAFE_ASSUME_VALUE      },
    { NULL, 0 }
};

static const enum_map_t MOD_STAGE_MAP[] = {
    { "pre_governor_trip_offset", THERMAL_MOD_STAGE_PRE_GOVERNOR_TRIP_OFFSET },
    { "post_governor_pwm_cap",    THERMAL_MOD_STAGE_POST_GOVERNOR_PWM_CAP    },
    { NULL, 0 }
};

static const enum_map_t FAULT_SEVERITY_MAP[] = {
    { "degraded", THERMAL_FAULT_SEVERITY_DEGRADED },
    { "critical", THERMAL_FAULT_SEVERITY_CRITICAL },
    { NULL, 0 }
};

static const enum_map_t FAULT_ACTION_MAP[] = {
    { "none",                          THERMAL_FAULT_ACTION_NONE                          },
    { "mark_degraded",                 THERMAL_FAULT_ACTION_MARK_DEGRADED                 },
    { "use_zone_fallback",             THERMAL_FAULT_ACTION_USE_ZONE_FALLBACK             },
    { "force_pwm_max_until_recovered", THERMAL_FAULT_ACTION_FORCE_PWM_MAX_UNTIL_RECOVERED },
    { "force_pwm_max_and_latch",       THERMAL_FAULT_ACTION_FORCE_PWM_MAX_AND_LATCH       },
    { "request_shutdown",              THERMAL_FAULT_ACTION_REQUEST_SHUTDOWN              },
    { NULL, 0 }
};

/* Look up a token string against an enum map; returns 0 on hit,
 * -1 on miss. Uses a buffer larger than THERMAL_NAME_MAX so longer
 * fault-action / modifier-stage enum spellings still fit. */
static int enum_lookup(const parse_ctx_t *ctx, int t,
                       const enum_map_t *map, int *out)
{
    char buf[64];
    if (tok_str_copy(ctx, t, buf, sizeof(buf)) != 0) return -1;
    for (int i = 0; map[i].name; i++) {
        if (strcmp(map[i].name, buf) == 0) {
            *out = map[i].value;
            return 0;
        }
    }
    return -1;
}

/* =============================================================== */
/* Platform-only fields                                             */
/* =============================================================== */
/*
 * Platform-only keys (PRD §5.1) are dispatched by name in each
 * per-object parser below: "source" / "pwm" / "tach" /
 * "pwm_freq_hz" / "tach_pulses_per_rev" / "fail_safe_value" inside
 * sensors / actuators / contexts, "transport" inside telemetry,
 * "control" at the top level.  When the caller passes a non-NULL
 * thermalcored_runtime_cfg_t, those fields land in the matching
 * runtime slot; otherwise they are silently skipped, preserving
 * the 9a behaviour for tests that only care about the CORE half.
 */

/* =============================================================== */
/* Pass A — sensors / context_signals / actuators                   */
/* =============================================================== */

static thermal_status_t parse_sensor(parse_ctx_t *ctx,
                                     int obj_idx,
                                     thermal_sensor_cfg_t *out,
                                     runtime_sensor_cfg_t *r_out,
                                     const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }

    unsigned int seen = 0;
    enum { F_ID = 1u << 0, F_NAME = 1u << 1,
           F_ALPHA = 1u << 2, F_STALE = 1u << 3 };

    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        if (tok_str_eq(ctx, k, "id")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 65535]");
            }
            out->id = (uint16_t)val;
            seen |= F_ID;
        } else if (tok_str_eq(ctx, k, "name")) {
            if (tok_str_copy(ctx, v, out->name, sizeof(out->name)) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "string too long or wrong type");
            }
            seen |= F_NAME;
        } else if (tok_str_eq(ctx, k, "iir_alpha_q16")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected non-negative integer");
            }
            out->iir_alpha_q16 = (int32_t)val;
            seen |= F_ALPHA;
        } else if (tok_str_eq(ctx, k, "max_staleness_ms")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 1, UINT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected positive integer");
            }
            out->max_staleness_ms = (uint32_t)val;
            seen |= F_STALE;
        } else if (tok_str_eq(ctx, k, "source")) {
            /* platform-only: filesystem path for the sensor value */
            if (r_out) {
                if (tok_str_copy(ctx, v, r_out->source, sizeof(r_out->source)) != 0) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                                   "path too long or wrong type");
                }
            }
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }

    if ((seen & (F_ID | F_NAME | F_ALPHA | F_STALE)) !=
        (F_ID | F_NAME | F_ALPHA | F_STALE)) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "required field missing");
    }
    return THERMAL_OK;
}

static thermal_status_t parse_context_signal(parse_ctx_t *ctx,
                                             int obj_idx,
                                             thermal_context_cfg_t *out,
                                             runtime_context_cfg_t *r_out,
                                             const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }

    unsigned int seen = 0;
    enum { F_ID = 1u << 0, F_NAME = 1u << 1, F_UNIT = 1u << 2,
           F_ALPHA = 1u << 3, F_TIMEOUT = 1u << 4, F_FS = 1u << 5 };

    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        if (tok_str_eq(ctx, k, "id")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 65535]");
            }
            out->id = (uint16_t)val;
            seen |= F_ID;
        } else if (tok_str_eq(ctx, k, "name")) {
            if (tok_str_copy(ctx, v, out->name, sizeof(out->name)) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "string too long or wrong type");
            }
            seen |= F_NAME;
        } else if (tok_str_eq(ctx, k, "unit")) {
            int val;
            if (enum_lookup(ctx, v, CONTEXT_UNIT_MAP, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "unknown enum");
            }
            out->unit = (uint8_t)val;
            seen |= F_UNIT;
        } else if (tok_str_eq(ctx, k, "iir_alpha_q16")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected non-negative integer");
            }
            out->iir_alpha_q16 = (int32_t)val;
            seen |= F_ALPHA;
        } else if (tok_str_eq(ctx, k, "timeout_ms")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 1, UINT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected positive integer");
            }
            out->timeout_ms = (uint32_t)val;
            seen |= F_TIMEOUT;
        } else if (tok_str_eq(ctx, k, "fail_safe")) {
            int val;
            if (enum_lookup(ctx, v, FAILSAFE_MAP, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "unknown enum");
            }
            out->fail_safe = (uint8_t)val;
            seen |= F_FS;
        } else if (tok_str_eq(ctx, k, "source")) {
            if (r_out) {
                if (tok_str_copy(ctx, v, r_out->source, sizeof(r_out->source)) != 0) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                                   "path too long or wrong type");
                }
            }
        } else if (tok_str_eq(ctx, k, "fail_safe_value")) {
            long long val;
            if (tok_parse_int_range(ctx, v, INT32_MIN, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer");
            }
            if (r_out) r_out->fail_safe_value = (int32_t)val;
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }

    if ((seen & (F_ID | F_NAME | F_UNIT | F_ALPHA | F_TIMEOUT | F_FS)) !=
        (F_ID | F_NAME | F_UNIT | F_ALPHA | F_TIMEOUT | F_FS)) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "required field missing");
    }
    return THERMAL_OK;
}

#if THERMALCORE_ENABLE_FAN_HEALTH
/* =============================================================== */
/* Pass A — per-actuator fan-health block (Stage 17, PRD App C)      */
/* =============================================================== */

static const enum_map_t FAN_BASELINE_SRC_MAP[] = {
    { "field",   THERMAL_FAN_BASELINE_SRC_FIELD   },
    { "factory", THERMAL_FAN_BASELINE_SRC_FACTORY },
    { "model",   THERMAL_FAN_BASELINE_SRC_MODEL   },
    { NULL, 0 }
};

/* Parse the { "aging":, "degraded":, "failing": } severity-threshold
 * object. Values are signed whole percent; the monotonic ordering
 * (0 > aging > degraded > failing) is checked by validate_config. */
static thermal_status_t parse_severity_pct(parse_ctx_t *ctx, int obj_idx,
                                           thermal_fan_health_cfg_t *out,
                                           const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    unsigned int seen = 0;
    enum { F_A = 1u << 0, F_D = 1u << 1, F_F = 1u << 2 };
    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[64];   /* fan-health JSON keys exceed THERMAL_NAME_MAX */
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);
        long long val;
        int8_t *dst = NULL;
        if      (tok_str_eq(ctx, k, "aging"))    { dst = &out->aging_pct;    seen |= F_A; }
        else if (tok_str_eq(ctx, k, "degraded")) { dst = &out->degraded_pct; seen |= F_D; }
        else if (tok_str_eq(ctx, k, "failing"))  { dst = &out->failing_pct;  seen |= F_F; }
        else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        if (tok_parse_int_range(ctx, v, -100, -1, &val) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                           "expected integer in [-100, -1]");
        }
        *dst = (int8_t)val;
        k = skip_token(ctx->toks, v);
    }
    if ((seen & (F_A | F_D | F_F)) != (F_A | F_D | F_F)) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "required field missing");
    }
    return THERMAL_OK;
}

/* Parse a per-actuator `fan_health` block. Structural parsing only:
 * field types, ranges, and required keys. The semantic baseline
 * invariants (monotonicity, threshold ordering, point count) are
 * checked by thermal_core_validate_config. */
static thermal_status_t parse_fan_health(parse_ctx_t *ctx, int obj_idx,
                                         thermal_fan_health_cfg_t *out,
                                         const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    memset(out, 0, sizeof(*out));

    unsigned int seen = 0;
    enum { F_SRC = 1u << 0, F_BASE = 1u << 1, F_PT = 1u << 2, F_PTOL = 1u << 3,
           F_RT = 1u << 4, F_RTOL = 1u << 5, F_MIN = 1u << 6, F_SEV = 1u << 7 };
    int saw_enable = 0, enable = 0;

    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[64];   /* fan-health JSON keys exceed THERMAL_NAME_MAX */
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        if (tok_str_eq(ctx, k, "enable")) {
            if (tok_parse_bool(ctx, v, &enable) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "expected boolean");
            }
            saw_enable = 1;
        } else if (tok_str_eq(ctx, k, "fan_model")) {
            /* Tooling-only compatibility metadata (PRD C.3): validated
             * as a string here, not carried into the core config. */
            char nm[64];
            if (tok_str_copy(ctx, v, nm, sizeof(nm)) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "string too long or wrong type");
            }
        } else if (tok_str_eq(ctx, k, "baseline_source")) {
            int val;
            if (enum_lookup(ctx, v, FAN_BASELINE_SRC_MAP, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "unknown enum");
            }
            out->baseline_source = (uint8_t)val;
            seen |= F_SRC;
        } else if (tok_str_eq(ctx, k, "baseline")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "expected array");
            }
            int cn = ctx->toks[v].size;
            if (cn > THERMAL_MAX_FAN_HEALTH_POINTS) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, sub_path,
                               "too many baseline points");
            }
            int ck = v + 1;
            for (int j = 0; j < cn; j++) {
                char elem_path[PATH_MAX_LEN + 32];
                snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                if (!tok_in_range(ctx, ck) ||
                    ctx->toks[ck].type != JSMN_ARRAY ||
                    ctx->toks[ck].size != 2) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                   "expected [pwm, rpm] pair");
                }
                int p_pwm = ck + 1;
                int p_rpm = skip_token(ctx->toks, p_pwm);
                long long pwm_v, rpm_v;
                if (tok_parse_int_range(ctx, p_pwm, 0, 255, &pwm_v) != 0) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                   "pwm must be an integer in [0, 255]");
                }
                if (tok_parse_int_range(ctx, p_rpm, 1, 65535, &rpm_v) != 0) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                   "rpm must be an integer in [1, 65535]");
                }
                out->baseline[j].x      = (int32_t)pwm_v;
                out->baseline[j].value0 = (int32_t)rpm_v;
                out->baseline[j].value1 = 0;
                ck = skip_token(ctx->toks, ck);
            }
            out->baseline_count = (uint8_t)cn;
            seen |= F_BASE;
        } else if (tok_str_eq(ctx, k, "stable_pwm_ticks")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 1, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "expected integer in [1, 65535]");
            }
            out->stable_pwm_ticks = (uint16_t)val;
            seen |= F_PT;
        } else if (tok_str_eq(ctx, k, "stable_pwm_tolerance")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, 255, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "expected integer in [0, 255]");
            }
            out->stable_pwm_tolerance = (uint8_t)val;
            seen |= F_PTOL;
        } else if (tok_str_eq(ctx, k, "stable_rpm_ticks")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 1, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "expected integer in [1, 65535]");
            }
            out->stable_rpm_ticks = (uint16_t)val;
            seen |= F_RT;
        } else if (tok_str_eq(ctx, k, "stable_rpm_tolerance_pct")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, 100, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "expected integer in [0, 100]");
            }
            out->stable_rpm_tolerance_pct = (uint8_t)val;
            seen |= F_RTOL;
        } else if (tok_str_eq(ctx, k, "min_points_observed")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 1, THERMAL_MAX_FAN_HEALTH_POINTS,
                                    &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "expected integer in [1, THERMAL_MAX_FAN_HEALTH_POINTS]");
            }
            out->min_points_observed = (uint8_t)val;
            seen |= F_MIN;
        } else if (tok_str_eq(ctx, k, "severity_pct")) {
            thermal_status_t s = parse_severity_pct(ctx, v, out, sub_path);
            if (s != THERMAL_OK) return s;
            seen |= F_SEV;
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }

    out->enable = (uint8_t)(saw_enable && enable);
    if (!out->enable) {
        /* Disabled (or enable:false) -> no further fields required. */
        return THERMAL_OK;
    }

    unsigned int required = F_SRC | F_BASE | F_PT | F_PTOL |
                            F_RT | F_RTOL | F_MIN | F_SEV;
    if ((seen & required) != required) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                       "required field missing");
    }
    return THERMAL_OK;
}
#endif /* THERMALCORE_ENABLE_FAN_HEALTH */

static thermal_status_t parse_actuator(parse_ctx_t *ctx,
                                       int obj_idx,
                                       uint8_t slot,
                                       thermal_config_t *cfg,
                                       runtime_actuator_cfg_t *r_out,
                                       const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }

    thermal_actuator_cfg_t *out = &cfg->actuators[slot];

    unsigned int seen = 0;
    enum { F_ID = 1u << 0, F_NAME = 1u << 1, F_PMIN = 1u << 2,
           F_PMAX = 1u << 3, F_STATES = 1u << 4 };
#if THERMALCORE_ENABLE_FAN_HEALTH
    int saw_tach = 0, saw_fan_health = 0;
    memset(&cfg->fan_health[slot], 0, sizeof(cfg->fan_health[slot]));
#endif

    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        if (tok_str_eq(ctx, k, "id")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 65535]");
            }
            out->id = (uint16_t)val;
            seen |= F_ID;
        } else if (tok_str_eq(ctx, k, "name")) {
            if (tok_str_copy(ctx, v, out->name, sizeof(out->name)) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "string too long or wrong type");
            }
            seen |= F_NAME;
        } else if (tok_str_eq(ctx, k, "pwm_min")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, 255, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 255]");
            }
            out->pwm_min = (uint8_t)val;
            seen |= F_PMIN;
        } else if (tok_str_eq(ctx, k, "pwm_max")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, 255, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 255]");
            }
            out->pwm_max = (uint8_t)val;
            seen |= F_PMAX;
        } else if (tok_str_eq(ctx, k, "slew_per_tick")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, 255, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 255]");
            }
            out->slew_per_tick = (uint8_t)val;
        } else if (tok_str_eq(ctx, k, "spinup_pwm")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, 255, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 255]");
            }
            out->spinup_pwm = (uint8_t)val;
        } else if (tok_str_eq(ctx, k, "spinup_ms")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected non-negative integer");
            }
            out->spinup_ms = (uint32_t)val;
        } else if (tok_str_eq(ctx, k, "state_pwm")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected array");
            }
            int sn = ctx->toks[v].size;
            if (sn > THERMAL_MAX_COOLING_STATES) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, sub_path, "too many cooling states");
            }
            int sk = v + 1;
            for (int j = 0; j < sn; j++) {
                long long sval;
                if (tok_parse_int_range(ctx, sk, 0, 255, &sval) != 0) {
                    char elem_path[PATH_MAX_LEN + 32];
                    snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, elem_path, "expected integer in [0, 255]");
                }
                out->state_pwm[j] = (uint8_t)sval;
                sk = skip_token(ctx->toks, sk);
            }
            /* Record JSON-explicit length so parse_zone can later
             * reject trips whose cooling_state runs past it (PRD
             * §5.3 line 805). */
            ctx->state_pwm_lens[slot] = (uint8_t)sn;
            seen |= F_STATES;
        } else if (tok_str_eq(ctx, k, "pwm")) {
            if (r_out) {
                if (tok_str_copy(ctx, v, r_out->pwm, sizeof(r_out->pwm)) != 0) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                                   "path too long or wrong type");
                }
            }
        } else if (tok_str_eq(ctx, k, "tach")) {
#if THERMALCORE_ENABLE_FAN_HEALTH
            saw_tach = 1;
#endif
            if (r_out) {
                if (tok_str_copy(ctx, v, r_out->tach, sizeof(r_out->tach)) != 0) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                                   "path too long or wrong type");
                }
            }
        } else if (tok_str_eq(ctx, k, "pwm_freq_hz")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "expected non-negative integer");
            }
            if (r_out) r_out->pwm_freq_hz = (uint32_t)val;
        } else if (tok_str_eq(ctx, k, "tach_pulses_per_rev")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "expected integer in [0, 65535]");
            }
            if (r_out) r_out->tach_pulses_per_rev = (uint16_t)val;
#if THERMALCORE_ENABLE_FAN_HEALTH
        } else if (tok_str_eq(ctx, k, "fan_health")) {
            thermal_status_t s = parse_fan_health(ctx, v,
                                                  &cfg->fan_health[slot],
                                                  sub_path);
            if (s != THERMAL_OK) return s;
            saw_fan_health = 1;
#endif
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }

    if ((seen & (F_ID | F_NAME | F_PMIN | F_PMAX | F_STATES)) !=
        (F_ID | F_NAME | F_PMIN | F_PMAX | F_STATES)) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "required field missing");
    }
#if THERMALCORE_ENABLE_FAN_HEALTH
    /* PRD C.4: an enabled fan-health block needs a tach source to
     * compare against -- reject it on a tachless actuator. */
    if (saw_fan_health && cfg->fan_health[slot].enable && !saw_tach) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                       "fan_health enabled but actuator has no tach source");
    }
#endif
    return THERMAL_OK;
}

/* =============================================================== */
/* Pass B — trips, PID, zones                                       */
/* =============================================================== */

static thermal_status_t parse_trip(parse_ctx_t *ctx,
                                   int obj_idx,
                                   thermal_trip_cfg_t *out,
                                   const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    unsigned int seen = 0;
    enum { F_T = 1u << 0, F_H = 1u << 1, F_S = 1u << 2, F_C = 1u << 3 };

    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        if (tok_str_eq(ctx, k, "temp_mc")) {
            long long val;
            if (tok_parse_int_range(ctx, v, INT32_MIN, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer");
            }
            out->temp_mc = (int32_t)val;
            seen |= F_T;
        } else if (tok_str_eq(ctx, k, "hyst_mc")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected non-negative integer");
            }
            out->hyst_mc = (int32_t)val;
            seen |= F_H;
        } else if (tok_str_eq(ctx, k, "severity")) {
            int val;
            if (enum_lookup(ctx, v, TRIP_SEVERITY_MAP, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "unknown enum");
            }
            out->severity = (uint8_t)val;
            seen |= F_S;
        } else if (tok_str_eq(ctx, k, "cooling_state")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, THERMAL_MAX_COOLING_STATES - 1, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "cooling_state out of range");
            }
            out->cooling_state = (uint8_t)val;
            seen |= F_C;
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }

    if ((seen & (F_T | F_H | F_S | F_C)) != (F_T | F_H | F_S | F_C)) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "required field missing");
    }
    return THERMAL_OK;
}

/* Parse a PID sub-object. Every field is optional; absent fields stay
 * zero so validate_config can flag them if the governor requires them. */
static thermal_status_t parse_pid(parse_ctx_t *ctx,
                                  int obj_idx,
                                  thermal_pid_cfg_t *out,
                                  const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        long long val;
        int32_t  *dst32 = NULL;
        uint16_t *dst16 = NULL;

        if      (tok_str_eq(ctx, k, "kp_q16"))           dst32 = &out->kp_q16;
        else if (tok_str_eq(ctx, k, "ki_q16"))           dst32 = &out->ki_q16;
        else if (tok_str_eq(ctx, k, "kd_q16"))           dst32 = &out->kd_q16;
        else if (tok_str_eq(ctx, k, "setpoint_mc"))      dst32 = &out->setpoint_mc;
        else if (tok_str_eq(ctx, k, "kp_min_q16"))       dst32 = &out->kp_min_q16;
        else if (tok_str_eq(ctx, k, "kp_max_q16"))       dst32 = &out->kp_max_q16;
        else if (tok_str_eq(ctx, k, "ki_min_q16"))       dst32 = &out->ki_min_q16;
        else if (tok_str_eq(ctx, k, "ki_max_q16"))       dst32 = &out->ki_max_q16;
        else if (tok_str_eq(ctx, k, "kd_min_q16"))       dst32 = &out->kd_min_q16;
        else if (tok_str_eq(ctx, k, "kd_max_q16"))       dst32 = &out->kd_max_q16;
        else if (tok_str_eq(ctx, k, "setpoint_min_mc"))  dst32 = &out->setpoint_min_mc;
        else if (tok_str_eq(ctx, k, "setpoint_max_mc"))  dst32 = &out->setpoint_max_mc;
        else if (tok_str_eq(ctx, k, "dt_min_ms"))        dst16 = &out->dt_min_ms;
        else if (tok_str_eq(ctx, k, "dt_max_ms"))        dst16 = &out->dt_max_ms;
        else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }

        if (dst32) {
            if (tok_parse_int_range(ctx, v, INT32_MIN, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer");
            }
            *dst32 = (int32_t)val;
        } else if (dst16) {
            if (tok_parse_int_range(ctx, v, 0, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 65535]");
            }
            *dst16 = (uint16_t)val;
        }
        k = skip_token(ctx->toks, v);
    }
    return THERMAL_OK;
}

/* Resolve a name string from a pass-A array. The arrays are
 * thermal_sensor_cfg_t / thermal_context_cfg_t / thermal_actuator_cfg_t
 * which all share the layout {uint16_t id; char name[N]; ...}.
 *
 * Returns the slot index (0..count-1) on success, or -1 on miss. */
static int resolve_name_in_sensors(const thermal_config_t *cfg, const char *name)
{
    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        if (strcmp(cfg->sensors[i].name, name) == 0) return i;
    }
    return -1;
}

static int resolve_name_in_contexts(const thermal_config_t *cfg, const char *name)
{
    for (uint8_t i = 0; i < cfg->context_count; i++) {
        if (strcmp(cfg->contexts[i].name, name) == 0) return i;
    }
    return -1;
}

static int resolve_name_in_actuators(const thermal_config_t *cfg, const char *name)
{
    for (uint8_t i = 0; i < cfg->actuator_count; i++) {
        if (strcmp(cfg->actuators[i].name, name) == 0) return i;
    }
    return -1;
}

static int resolve_name_in_zones(const thermal_config_t *cfg, const char *name)
{
    for (uint8_t i = 0; i < cfg->zone_count; i++) {
        if (strcmp(cfg->zones[i].name, name) == 0) return i;
    }
    return -1;
}

static thermal_status_t parse_zone(parse_ctx_t *ctx,
                                   int obj_idx,
                                   const thermal_config_t *cfg_view,
                                   thermal_zone_cfg_t *out,
                                   const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    unsigned int seen = 0;
    enum { F_NAME = 1u << 0, F_SENSORS = 1u << 1, F_AGG = 1u << 2,
           F_GOV = 1u << 3, F_FB = 1u << 4, F_ACTUATORS = 1u << 5,
           F_TRIPS = 1u << 6 };

    /* Track JSON-explicit weights length so we can match it against
     * sensor_count for weighted aggregation (PRD §5.3 line 800). */
    int weights_explicit_len = -1;

    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        if (tok_str_eq(ctx, k, "name")) {
            if (tok_str_copy(ctx, v, out->name, sizeof(out->name)) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "string too long or wrong type");
            }
            seen |= F_NAME;
        } else if (tok_str_eq(ctx, k, "sensors")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected array");
            }
            int sn = ctx->toks[v].size;
            if (sn > THERMAL_MAX_SENSORS_PER_ZONE) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, sub_path, "too many sensors per zone");
            }
            int sk = v + 1;
            for (int j = 0; j < sn; j++) {
                char nm[THERMAL_NAME_MAX];
                if (tok_str_copy(ctx, sk, nm, sizeof(nm)) != 0) {
                    char elem_path[PATH_MAX_LEN + 32];
                    snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                   "expected string sensor name");
                }
                int idx = resolve_name_in_sensors(cfg_view, nm);
                if (idx < 0) {
                    char elem_path[PATH_MAX_LEN + 32];
                    snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                    return set_errf(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                    "unknown sensor '%s'", nm);
                }
                out->sensor_ids[j] = cfg_view->sensors[idx].id;
                sk = skip_token(ctx->toks, sk);
            }
            out->sensor_count = (uint8_t)sn;
            seen |= F_SENSORS;
        } else if (tok_str_eq(ctx, k, "sensor_weights_q16")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected array");
            }
            int sn = ctx->toks[v].size;
            if (sn > THERMAL_MAX_SENSORS_PER_ZONE) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, sub_path,
                               "too many sensor weights per zone");
            }
            int sk = v + 1;
            for (int j = 0; j < sn; j++) {
                long long val;
                if (tok_parse_int_range(ctx, sk, INT32_MIN, INT32_MAX, &val) != 0) {
                    char elem_path[PATH_MAX_LEN + 32];
                    snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                   "expected integer");
                }
                out->sensor_weights_q16[j] = (int32_t)val;
                sk = skip_token(ctx->toks, sk);
            }
            weights_explicit_len = sn;
        } else if (tok_str_eq(ctx, k, "aggregation")) {
            int val;
            if (enum_lookup(ctx, v, AGGREGATION_MAP, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "unknown enum");
            }
            out->aggregation = (uint8_t)val;
            seen |= F_AGG;
        } else if (tok_str_eq(ctx, k, "fallback_temp_mc")) {
            long long val;
            if (tok_parse_int_range(ctx, v, INT32_MIN, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer");
            }
            out->fallback_temp_mc = (int32_t)val;
            seen |= F_FB;
        } else if (tok_str_eq(ctx, k, "governor")) {
            int val;
            if (enum_lookup(ctx, v, GOVERNOR_MAP, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "unknown enum");
            }
            out->governor = (uint8_t)val;
            seen |= F_GOV;
        } else if (tok_str_eq(ctx, k, "pid")) {
            thermal_status_t s = parse_pid(ctx, v, &out->pid, sub_path);
            if (s != THERMAL_OK) return s;
        } else if (tok_str_eq(ctx, k, "actuators")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected array");
            }
            int an = ctx->toks[v].size;
            if (an > THERMAL_MAX_ACTUATORS_PER_ZONE) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, sub_path,
                               "too many actuators per zone");
            }
            int ak = v + 1;
            for (int j = 0; j < an; j++) {
                char nm[THERMAL_NAME_MAX];
                if (tok_str_copy(ctx, ak, nm, sizeof(nm)) != 0) {
                    char elem_path[PATH_MAX_LEN + 32];
                    snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                   "expected string actuator name");
                }
                int idx = resolve_name_in_actuators(cfg_view, nm);
                if (idx < 0) {
                    char elem_path[PATH_MAX_LEN + 32];
                    snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                    return set_errf(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                    "unknown actuator '%s'", nm);
                }
                out->actuator_ids[j] = cfg_view->actuators[idx].id;
                ak = skip_token(ctx->toks, ak);
            }
            out->actuator_count = (uint8_t)an;
            seen |= F_ACTUATORS;
        } else if (tok_str_eq(ctx, k, "trips")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected array");
            }
            int tn = ctx->toks[v].size;
            if (tn > THERMAL_MAX_TRIPS_PER_ZONE) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, sub_path,
                               "too many trips per zone");
            }
            int tk = v + 1;
            for (int j = 0; j < tn; j++) {
                char elem_path[PATH_MAX_LEN + 32];
                snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                thermal_status_t s = parse_trip(ctx, tk, &out->trips[j], elem_path);
                if (s != THERMAL_OK) return s;
                tk = skip_token(ctx->toks, tk);
            }
            out->trip_count = (uint8_t)tn;
            seen |= F_TRIPS;
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }

    unsigned int required = F_NAME | F_SENSORS | F_AGG | F_GOV | F_FB |
                            F_ACTUATORS | F_TRIPS;
    if ((seen & required) != required) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "required field missing");
    }

    /* PRD §5.3 line 800: weighted aggregation requires a weights
     * array whose length matches sensor_count.  Tail-zero-fill
     * would silently give a missing sensor weight 0. */
    if (out->aggregation == THERMAL_AGG_WEIGHTED) {
        if (weights_explicit_len < 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "weighted aggregation requires sensor_weights_q16");
        }
        if ((uint8_t)weights_explicit_len != out->sensor_count) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "sensor_weights_q16 length must match sensor_count");
        }
    }

    /* PRD §5.3 line 805: each trip's cooling_state must fall inside
     * the JSON-explicit state_pwm length of every referenced
     * actuator.  Validates against ctx->state_pwm_lens[] recorded
     * during pass A. */
    for (uint8_t t = 0; t < out->trip_count; t++) {
        uint8_t cs = out->trips[t].cooling_state;
        for (uint8_t a = 0; a < out->actuator_count; a++) {
            uint16_t act_id = out->actuator_ids[a];
            int slot = -1;
            for (uint8_t i = 0; i < cfg_view->actuator_count; i++) {
                if (cfg_view->actuators[i].id == act_id) { slot = (int)i; break; }
            }
            if (slot < 0) continue;  /* should not happen — name resolved earlier */
            if (cs >= ctx->state_pwm_lens[slot]) {
                char elem_path[PATH_MAX_LEN + 32];
                snprintf(elem_path, sizeof(elem_path),
                         "%s.trips[%u]", path, (unsigned)t);
                return set_errf(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                "cooling_state past actuator state_pwm length '%s'",
                                cfg_view->actuators[slot].name);
            }
        }
    }
    return THERMAL_OK;
}

/* =============================================================== */
/* Pass B — modifiers (curve + name ref)                            */
/* =============================================================== */

static thermal_status_t parse_curve_point(parse_ctx_t *ctx,
                                          int obj_idx,
                                          thermal_curve_point_t *out,
                                          const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    unsigned int seen = 0;
    enum { F_X = 1u << 0, F_V0 = 1u << 1, F_V1 = 1u << 2 };

    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        long long val;
        int32_t *dst = NULL;
        if      (tok_str_eq(ctx, k, "x"))      { dst = &out->x;      seen |= F_X;  }
        else if (tok_str_eq(ctx, k, "value0")) { dst = &out->value0; seen |= F_V0; }
        else if (tok_str_eq(ctx, k, "value1")) { dst = &out->value1; seen |= F_V1; }
        else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        if (tok_parse_int_range(ctx, v, INT32_MIN, INT32_MAX, &val) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer");
        }
        *dst = (int32_t)val;
        k = skip_token(ctx->toks, v);
    }
    if ((seen & (F_X | F_V0 | F_V1)) != (F_X | F_V0 | F_V1)) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "required field missing");
    }
    return THERMAL_OK;
}

static thermal_status_t parse_modifier(parse_ctx_t *ctx,
                                       int obj_idx,
                                       const thermal_config_t *cfg_view,
                                       thermal_modifier_cfg_t *out,
                                       const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    unsigned int seen = 0;
    enum { F_NAME = 1u << 0, F_CTX = 1u << 1, F_STAGES = 1u << 2,
           F_CURVE = 1u << 3, F_FS = 1u << 4 };

    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        if (tok_str_eq(ctx, k, "name")) {
            if (tok_str_copy(ctx, v, out->name, sizeof(out->name)) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "string too long or wrong type");
            }
            seen |= F_NAME;
        } else if (tok_str_eq(ctx, k, "context")) {
            char nm[THERMAL_NAME_MAX];
            if (tok_str_copy(ctx, v, nm, sizeof(nm)) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected string");
            }
            int idx = resolve_name_in_contexts(cfg_view, nm);
            if (idx < 0) {
                return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                                "unknown context '%s'", nm);
            }
            out->context_id = cfg_view->contexts[idx].id;
            seen |= F_CTX;
        } else if (tok_str_eq(ctx, k, "stages")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected array");
            }
            int sn = ctx->toks[v].size;
            int sk = v + 1;
            uint8_t mask = 0;
            for (int j = 0; j < sn; j++) {
                int val;
                if (enum_lookup(ctx, sk, MOD_STAGE_MAP, &val) != 0) {
                    char elem_path[PATH_MAX_LEN + 32];
                    snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                   "unknown stage");
                }
                mask = (uint8_t)(mask | (uint8_t)val);
                sk = skip_token(ctx->toks, sk);
            }
            out->stages = mask;
            seen |= F_STAGES;
        } else if (tok_str_eq(ctx, k, "curve")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected array");
            }
            int cn = ctx->toks[v].size;
            if (cn > THERMAL_MAX_CURVE_POINTS) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, sub_path, "too many curve points");
            }
            int ck = v + 1;
            for (int j = 0; j < cn; j++) {
                char elem_path[PATH_MAX_LEN + 32];
                snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                thermal_status_t s = parse_curve_point(ctx, ck,
                                                       &out->curve[j], elem_path);
                if (s != THERMAL_OK) return s;
                ck = skip_token(ctx->toks, ck);
            }
            out->curve_count = (uint8_t)cn;
            seen |= F_CURVE;
        } else if (tok_str_eq(ctx, k, "fail_safe")) {
            int val;
            if (enum_lookup(ctx, v, FAILSAFE_MAP, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "unknown enum");
            }
            out->fail_safe = (uint8_t)val;
            seen |= F_FS;
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }

    unsigned int required = F_NAME | F_CTX | F_STAGES | F_CURVE | F_FS;
    if ((seen & required) != required) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "required field missing");
    }
    return THERMAL_OK;
}

/* =============================================================== */
/* Pass B — fault detector defaults                                 */
/* =============================================================== */

/* PRD §4.7 line 617: only stuck_sensor uses correlated_context_id;
 * for the other three detectors it is set to 0xFFFF by convention.
 * Per-detector threshold key naming comes from PRD §5 line 611:
 *
 *   detector       | threshold0           | threshold1
 *   ---            | ---                  | ---
 *   stall          | stall_rpm            | stall_pwm_threshold
 *   stuck_sensor   | delta_mc             | window_ticks
 *   runaway        | rise_mc_threshold    | cooling_pwm_threshold
 *   stale_context  | (unused)             | (unused)
 */
typedef enum {
    DET_STALL = 0,
    DET_STUCK_SENSOR,
    DET_RUNAWAY,
    DET_STALE_CONTEXT
} detector_kind_t;

/* Check JSMN PRIMITIVE token for the literal `null`. */
static int tok_is_null(const parse_ctx_t *ctx, int t)
{
    if (!tok_in_range(ctx, t)) return 0;
    const jsmntok_t *tok = &ctx->toks[t];
    if (tok->type != JSMN_PRIMITIVE) return 0;
    size_t len = (size_t)(tok->end - tok->start);
    return len == 4 && memcmp(ctx->json + tok->start, "null", 4) == 0;
}

static thermal_status_t parse_fault_detector(parse_ctx_t *ctx,
                                             int obj_idx,
                                             detector_kind_t kind,
                                             const thermal_config_t *cfg_view,
                                             thermal_fault_detector_cfg_t *out,
                                             const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }

    /* PRD §4.7 line 617: 0xFFFF by convention for non-stuck_sensor
     * detectors, and the advisory default for stuck_sensor when
     * `correlated_context` is absent or null. */
    out->correlated_context_id = 0xFFFFu;

    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        /* Shared keys ------------------------------------------------- */
        if (tok_str_eq(ctx, k, "enabled")) {
            int val;
            if (tok_parse_bool(ctx, v, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected boolean");
            }
            out->enabled = (uint8_t)val;
        } else if (tok_str_eq(ctx, k, "severity")) {
            int val;
            if (enum_lookup(ctx, v, FAULT_SEVERITY_MAP, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "unknown enum");
            }
            out->severity = (uint8_t)val;
        } else if (tok_str_eq(ctx, k, "action")) {
            int val;
            if (enum_lookup(ctx, v, FAULT_ACTION_MAP, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "unknown enum");
            }
            out->action = (uint8_t)val;
        } else if (tok_str_eq(ctx, k, "persist_ticks")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 65535]");
            }
            out->persist_ticks = (uint16_t)val;
        } else if (tok_str_eq(ctx, k, "recovery_ticks")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 65535]");
            }
            out->recovery_ticks = (uint16_t)val;

        /* Per-detector descriptive thresholds ------------------------- */
        } else if (tok_str_eq(ctx, k, "stall_rpm")) {
            if (kind != DET_STALL) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "stall_rpm only valid for stall detector");
            }
            long long val;
            if (tok_parse_int_range(ctx, v, INT32_MIN, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer");
            }
            out->threshold0 = (int32_t)val;
        } else if (tok_str_eq(ctx, k, "stall_pwm_threshold")) {
            if (kind != DET_STALL) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "stall_pwm_threshold only valid for stall detector");
            }
            long long val;
            if (tok_parse_int_range(ctx, v, INT32_MIN, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer");
            }
            out->threshold1 = (int32_t)val;
        } else if (tok_str_eq(ctx, k, "delta_mc")) {
            if (kind != DET_STUCK_SENSOR) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "delta_mc only valid for stuck_sensor detector");
            }
            long long val;
            if (tok_parse_int_range(ctx, v, INT32_MIN, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer");
            }
            out->threshold0 = (int32_t)val;
        } else if (tok_str_eq(ctx, k, "window_ticks")) {
            if (kind != DET_STUCK_SENSOR) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "window_ticks only valid for stuck_sensor detector");
            }
            long long val;
            if (tok_parse_int_range(ctx, v, 0, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "expected non-negative integer");
            }
            out->threshold1 = (int32_t)val;
        } else if (tok_str_eq(ctx, k, "rise_mc_threshold")) {
            if (kind != DET_RUNAWAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "rise_mc_threshold only valid for runaway detector");
            }
            long long val;
            if (tok_parse_int_range(ctx, v, INT32_MIN, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer");
            }
            out->threshold0 = (int32_t)val;
        } else if (tok_str_eq(ctx, k, "cooling_pwm_threshold")) {
            if (kind != DET_RUNAWAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "cooling_pwm_threshold only valid for runaway detector");
            }
            long long val;
            if (tok_parse_int_range(ctx, v, INT32_MIN, INT32_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer");
            }
            out->threshold1 = (int32_t)val;

        /* Per-stuck-sensor correlated context ------------------------- */
        } else if (tok_str_eq(ctx, k, "correlated_context")) {
            if (kind != DET_STUCK_SENSOR) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "correlated_context only valid for stuck_sensor detector");
            }
            if (tok_is_null(ctx, v)) {
                /* PRD §4.7 line 632: null → advisory mode. */
                out->correlated_context_id = 0xFFFFu;
            } else {
                char nm[THERMAL_NAME_MAX];
                if (tok_str_copy(ctx, v, nm, sizeof(nm)) != 0) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                                   "expected string or null");
                }
                int idx = resolve_name_in_contexts(cfg_view, nm);
                if (idx < 0) {
                    return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                                    "unknown context '%s'", nm);
                }
                out->correlated_context_id = cfg_view->contexts[idx].id;
            }
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }
    return THERMAL_OK;
}

static thermal_status_t parse_fault_detection_object(parse_ctx_t *ctx,
                                                     int obj_idx,
                                                     const thermal_config_t *cfg_view,
                                                     thermal_fault_detection_cfg_t *out,
                                                     const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        thermal_fault_detector_cfg_t *dst = NULL;
        detector_kind_t kind = DET_STALL;
        if      (tok_str_eq(ctx, k, "stall"))         { dst = &out->stall_defaults;         kind = DET_STALL; }
        else if (tok_str_eq(ctx, k, "stuck_sensor"))  { dst = &out->stuck_sensor_defaults;  kind = DET_STUCK_SENSOR; }
        else if (tok_str_eq(ctx, k, "runaway"))       { dst = &out->runaway_defaults;       kind = DET_RUNAWAY; }
        else if (tok_str_eq(ctx, k, "stale_context")) { dst = &out->stale_context_defaults; kind = DET_STALE_CONTEXT; }
        else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        thermal_status_t s = parse_fault_detector(ctx, v, kind, cfg_view, dst, sub_path);
        if (s != THERMAL_OK) return s;
        k = skip_token(ctx->toks, v);
    }
    return THERMAL_OK;
}

/* =============================================================== */
/* Pass B — telemetry signal expansion                              */
/* =============================================================== */

/* Append a single signal id, bounds-checking against the array max. */
static thermal_status_t telemetry_append(parse_ctx_t *ctx,
                                         thermal_telemetry_cfg_t *t,
                                         uint16_t sig,
                                         const char *path)
{
    if (t->enabled_signal_count >= THERMAL_MAX_TELEMETRY_SIGNALS) {
        return set_err(ctx, THERMAL_ERR_NO_SPACE, path,
                       "too many enabled telemetry signals");
    }
    t->enabled_signal_ids[t->enabled_signal_count++] = sig;
    return THERMAL_OK;
}

/* Selector matching helpers ------------------------------------- */

static int str_has_prefix(const char *s, const char *p, const char **rest)
{
    size_t pl = strlen(p);
    if (strncmp(s, p, pl) != 0) return 0;
    *rest = s + pl;
    return 1;
}

/* Expand a single telemetry selector string into one or more signal
 * ids appended to cfg->telemetry. cfg is needed for slot counts and
 * named-slot lookups. */
static thermal_status_t telemetry_expand_one(parse_ctx_t *ctx,
                                             const thermal_config_t *cfg,
                                             thermal_telemetry_cfg_t *t,
                                             const char *sel,
                                             const char *path)
{
    const char *rest;

    /* Wildcard families ------------------------------------------ */
    if (strcmp(sel, "zone_temp_*") == 0) {
        if (cfg->zone_count == 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "wildcard expanded to nothing");
        }
        for (uint8_t s = 0; s < cfg->zone_count; s++) {
            thermal_status_t r = telemetry_append(ctx, t, TSIG_ZONE_TEMP(s), path);
            if (r != THERMAL_OK) return r;
        }
        return THERMAL_OK;
    }
    if (strcmp(sel, "zone_cooling_state_*") == 0) {
        if (cfg->zone_count == 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "wildcard expanded to nothing");
        }
        for (uint8_t s = 0; s < cfg->zone_count; s++) {
            thermal_status_t r = telemetry_append(ctx, t,
                                                  TSIG_ZONE_COOLING_STATE(s), path);
            if (r != THERMAL_OK) return r;
        }
        return THERMAL_OK;
    }
    if (strcmp(sel, "zone_aggregation_valid_*") == 0) {
        if (cfg->zone_count == 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "wildcard expanded to nothing");
        }
        for (uint8_t s = 0; s < cfg->zone_count; s++) {
            thermal_status_t r = telemetry_append(ctx, t,
                                                  TSIG_ZONE_AGGREGATION_VALID(s), path);
            if (r != THERMAL_OK) return r;
        }
        return THERMAL_OK;
    }
    if (strcmp(sel, "actuator_pwm_*") == 0) {
        if (cfg->actuator_count == 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "wildcard expanded to nothing");
        }
        for (uint8_t s = 0; s < cfg->actuator_count; s++) {
            thermal_status_t r = telemetry_append(ctx, t,
                                                  TSIG_ACTUATOR_DUTY(s), path);
            if (r != THERMAL_OK) return r;
        }
        return THERMAL_OK;
    }
    if (strcmp(sel, "actuator_rpm_*") == 0) {
        if (cfg->actuator_count == 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "wildcard expanded to nothing");
        }
        for (uint8_t s = 0; s < cfg->actuator_count; s++) {
            thermal_status_t r = telemetry_append(ctx, t,
                                                  TSIG_ACTUATOR_RPM(s), path);
            if (r != THERMAL_OK) return r;
        }
        return THERMAL_OK;
    }
    if (strcmp(sel, "pid_terms_*") == 0) {
        if (cfg->zone_count == 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "wildcard expanded to nothing");
        }
        for (uint8_t z = 0; z < cfg->zone_count; z++) {
            for (uint8_t term = 0; term < 4; term++) {
                thermal_status_t r = telemetry_append(ctx, t,
                                                      TSIG_PID(z, term), path);
                if (r != THERMAL_OK) return r;
            }
        }
        return THERMAL_OK;
    }
    if (strcmp(sel, "modifier_pwm_cap_*") == 0) {
        if (cfg->modifier_count == 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "wildcard expanded to nothing");
        }
        for (uint8_t s = 0; s < cfg->modifier_count; s++) {
            thermal_status_t r = telemetry_append(ctx, t,
                                                  TSIG_MODIFIER_PWM_CAP_AT(s), path);
            if (r != THERMAL_OK) return r;
        }
        return THERMAL_OK;
    }
    /* fault_* expands to all four detector-state sub-signals per
     * relevant slot range. */
    if (strcmp(sel, "fault_*") == 0) {
        int any = 0;
        for (uint8_t s = 0; s < cfg->actuator_count; s++) {
            thermal_status_t r = telemetry_append(ctx, t,
                                                  TSIG_FAULT_STALL_STATE(s), path);
            if (r != THERMAL_OK) return r;
            any = 1;
        }
        for (uint8_t s = 0; s < cfg->sensor_count; s++) {
            thermal_status_t r = telemetry_append(ctx, t,
                                                  TSIG_FAULT_STUCK_SENSOR_STATE(s), path);
            if (r != THERMAL_OK) return r;
            any = 1;
        }
        for (uint8_t s = 0; s < cfg->zone_count; s++) {
            thermal_status_t r = telemetry_append(ctx, t,
                                                  TSIG_FAULT_RUNAWAY_STATE(s), path);
            if (r != THERMAL_OK) return r;
            any = 1;
        }
        for (uint8_t s = 0; s < cfg->context_count; s++) {
            thermal_status_t r = telemetry_append(ctx, t,
                                                  TSIG_FAULT_STALE_CONTEXT_STATE(s), path);
            if (r != THERMAL_OK) return r;
            any = 1;
        }
        if (!any) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "wildcard expanded to nothing");
        }
        return THERMAL_OK;
    }
#if THERMALCORE_ENABLE_FAN_HEALTH
    /* fan_health_* expands to the four fan-health sub-signals per
     * actuator slot (Stage 17, PRD Appendix C). */
    if (strcmp(sel, "fan_health_*") == 0) {
        if (cfg->actuator_count == 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "wildcard expanded to nothing");
        }
        for (uint8_t s = 0; s < cfg->actuator_count; s++) {
            thermal_status_t r;
            if ((r = telemetry_append(ctx, t, TSIG_FAN_HEALTH_DELTA(s),
                                      path)) != THERMAL_OK) return r;
            if ((r = telemetry_append(ctx, t, TSIG_FAN_HEALTH_SEVERITY(s),
                                      path)) != THERMAL_OK) return r;
            if ((r = telemetry_append(ctx, t, TSIG_FAN_HEALTH_BASELINE_SOURCE(s),
                                      path)) != THERMAL_OK) return r;
            if ((r = telemetry_append(ctx, t, TSIG_FAN_HEALTH_CONFIDENCE(s),
                                      path)) != THERMAL_OK) return r;
        }
        return THERMAL_OK;
    }
#endif

    /* Named selectors -------------------------------------------- */
    if (strcmp(sel, "speed_kmh") == 0) {
        int idx = resolve_name_in_contexts(cfg, "vehicle_speed");
        if (idx < 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path,
                           "speed_kmh requires a 'vehicle_speed' context");
        }
        return telemetry_append(ctx, t, TSIG_CONTEXT_VALUE((uint8_t)idx), path);
    }
    if (str_has_prefix(sel, "context_", &rest)) {
        int idx = resolve_name_in_contexts(cfg, rest);
        if (idx < 0) {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, path,
                            "unknown context '%s'", rest);
        }
        return telemetry_append(ctx, t, TSIG_CONTEXT_VALUE((uint8_t)idx), path);
    }
    if (str_has_prefix(sel, "zone_temp_", &rest)) {
        int idx = resolve_name_in_zones(cfg, rest);
        if (idx < 0) {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, path,
                            "unknown zone '%s'", rest);
        }
        return telemetry_append(ctx, t, TSIG_ZONE_TEMP((uint8_t)idx), path);
    }
    if (str_has_prefix(sel, "zone_aggregation_valid_", &rest)) {
        int idx = resolve_name_in_zones(cfg, rest);
        if (idx < 0) {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, path,
                            "unknown zone '%s'", rest);
        }
        return telemetry_append(ctx, t,
                                TSIG_ZONE_AGGREGATION_VALID((uint8_t)idx),
                                path);
    }
    if (str_has_prefix(sel, "actuator_pwm_", &rest)) {
        int idx = resolve_name_in_actuators(cfg, rest);
        if (idx < 0) {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, path,
                            "unknown actuator '%s'", rest);
        }
        return telemetry_append(ctx, t, TSIG_ACTUATOR_DUTY((uint8_t)idx), path);
    }
    if (str_has_prefix(sel, "actuator_rpm_", &rest)) {
        int idx = resolve_name_in_actuators(cfg, rest);
        if (idx < 0) {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, path,
                            "unknown actuator '%s'", rest);
        }
        return telemetry_append(ctx, t, TSIG_ACTUATOR_RPM((uint8_t)idx), path);
    }
    if (str_has_prefix(sel, "pid_terms_", &rest)) {
        int idx = resolve_name_in_zones(cfg, rest);
        if (idx < 0) {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, path,
                            "unknown zone '%s'", rest);
        }
        for (uint8_t term = 0; term < 4; term++) {
            thermal_status_t r = telemetry_append(ctx, t,
                                                  TSIG_PID((uint8_t)idx, term), path);
            if (r != THERMAL_OK) return r;
        }
        return THERMAL_OK;
    }

    return set_errf(ctx, THERMAL_ERR_INVALID_ARG, path,
                    "unknown telemetry selector '%s'", sel);
}

static thermal_status_t parse_telemetry_object(parse_ctx_t *ctx,
                                               int obj_idx,
                                               const thermal_config_t *cfg_view,
                                               thermal_telemetry_cfg_t *out,
                                               runtime_global_cfg_t *r_glob,
                                               const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        if (tok_str_eq(ctx, k, "enable")) {
            int val;
            if (tok_parse_bool(ctx, v, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected boolean");
            }
            out->enable = (uint8_t)val;
        } else if (tok_str_eq(ctx, k, "period_ticks")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected integer in [0, 65535]");
            }
            out->period_ticks = (uint16_t)val;
        } else if (tok_str_eq(ctx, k, "signals")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path, "expected array");
            }
            int sn = ctx->toks[v].size;
            int sk = v + 1;
            for (int j = 0; j < sn; j++) {
                char sel[THERMAL_NAME_MAX * 2];
                if (tok_str_copy(ctx, sk, sel, sizeof(sel)) != 0) {
                    char elem_path[PATH_MAX_LEN + 32];
                    snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, elem_path,
                                   "expected string selector");
                }
                char elem_path[PATH_MAX_LEN + 32];
                snprintf(elem_path, sizeof(elem_path), "%s[%d]", sub_path, j);
                thermal_status_t r = telemetry_expand_one(ctx, cfg_view, out,
                                                          sel, elem_path);
                if (r != THERMAL_OK) return r;
                sk = skip_token(ctx->toks, sk);
            }
        } else if (tok_str_eq(ctx, k, "transport")) {
            if (r_glob) {
                if (tok_str_copy(ctx, v, r_glob->telemetry_transport,
                                 sizeof(r_glob->telemetry_transport)) != 0) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                                   "transport URI too long or wrong type");
                }
            }
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }
    return THERMAL_OK;
}

/* =============================================================== */
/* Top-level control object (BSP-owned)                             */
/* =============================================================== */

static thermal_status_t parse_control_object(parse_ctx_t *ctx,
                                              int obj_idx,
                                              runtime_global_cfg_t *r_glob,
                                              const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        if (tok_str_eq(ctx, k, "listen")) {
            if (r_glob) {
                if (tok_str_copy(ctx, v, r_glob->control_listen,
                                 sizeof(r_glob->control_listen)) != 0) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                                   "listen spec too long or wrong type");
                }
            }
        } else if (tok_str_eq(ctx, k, "enable")) {
            /* PRD §7.3 line 945: control plane listener defaults off;
             * caller must opt in with `control.enable = true`. */
            int val;
            if (tok_parse_bool(ctx, v, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                               "control.enable must be true or false");
            }
            if (r_glob) r_glob->control_enable = (uint8_t)val;
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }
    return THERMAL_OK;
}

/* =============================================================== */
/* Top-level HIL object (Stage 14, BSP-owned)                       */
/* =============================================================== */
/* {
 *   "hil": { "transport": "serial:/dev/ttyACM0" }
 * }
 *
 * Sibling to "telemetry" and "control".  When set, the daemon runs
 * in HIL_PERIPHERAL mode and opens the named device for binary TC
 * frames in both directions (PRD §8.3). */

static thermal_status_t parse_hil_object(parse_ctx_t *ctx,
                                          int obj_idx,
                                          runtime_global_cfg_t *r_glob,
                                          const char *path)
{
    if (ctx->toks[obj_idx].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "expected object");
    }
    int n = ctx->toks[obj_idx].size;
    int k = obj_idx + 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char sub_path[PATH_MAX_LEN];
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, path, "non-string key");
        }
        snprintf(sub_path, sizeof(sub_path), "%s.%s", path, key_str);

        if (tok_str_eq(ctx, k, "transport")) {
            if (r_glob) {
                if (tok_str_copy(ctx, v, r_glob->hil_transport,
                                 sizeof(r_glob->hil_transport)) != 0) {
                    return set_err(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                                   "hil.transport URI too long or wrong type");
                }
            }
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, sub_path,
                            "unknown key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }
    return THERMAL_OK;
}

/* =============================================================== */
/* Top-level passes                                                 */
/* =============================================================== */

/* Pass A: scan top-level keys; parse sensors / context_signals /
 * actuators. Other keys are left for pass B.  runtime may be NULL. */
static thermal_status_t pass_a_top_level(parse_ctx_t *ctx,
                                         thermal_config_t *cfg,
                                         thermalcored_runtime_cfg_t *runtime)
{
    if (ctx->toks[0].type != JSMN_OBJECT) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, "$", "expected root object");
    }
    int n = ctx->toks[0].size;
    int k = 1;
    for (int i = 0; i < n; i++) {
        int v = k + 1;
        if (tok_str_eq(ctx, k, "sensors")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, "sensors", "expected array");
            }
            int an = ctx->toks[v].size;
            if (an > THERMAL_MAX_SENSORS) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, "sensors", "too many sensors");
            }
            int ak = v + 1;
            for (int j = 0; j < an; j++) {
                char elem_path[PATH_MAX_LEN + 32];
                snprintf(elem_path, sizeof(elem_path), "sensors[%d]", j);
                runtime_sensor_cfg_t *r = runtime ? &runtime->sensors[j] : NULL;
                thermal_status_t s = parse_sensor(ctx, ak, &cfg->sensors[j], r, elem_path);
                if (s != THERMAL_OK) return s;
                ak = skip_token(ctx->toks, ak);
            }
            cfg->sensor_count = (uint8_t)an;
            if (runtime) runtime->sensor_count = (uint8_t)an;
        } else if (tok_str_eq(ctx, k, "context_signals")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, "context_signals", "expected array");
            }
            int an = ctx->toks[v].size;
            if (an > THERMAL_MAX_CONTEXT_SIGNALS) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, "context_signals", "too many context signals");
            }
            int ak = v + 1;
            for (int j = 0; j < an; j++) {
                char elem_path[PATH_MAX_LEN + 32];
                snprintf(elem_path, sizeof(elem_path), "context_signals[%d]", j);
                runtime_context_cfg_t *r = runtime ? &runtime->contexts[j] : NULL;
                thermal_status_t s = parse_context_signal(ctx, ak,
                                                          &cfg->contexts[j], r, elem_path);
                if (s != THERMAL_OK) return s;
                ak = skip_token(ctx->toks, ak);
            }
            cfg->context_count = (uint8_t)an;
            if (runtime) runtime->context_count = (uint8_t)an;
        } else if (tok_str_eq(ctx, k, "actuators")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, "actuators", "expected array");
            }
            int an = ctx->toks[v].size;
            if (an > THERMAL_MAX_ACTUATORS) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, "actuators", "too many actuators");
            }
            int ak = v + 1;
            for (int j = 0; j < an; j++) {
                char elem_path[PATH_MAX_LEN + 32];
                snprintf(elem_path, sizeof(elem_path), "actuators[%d]", j);
                runtime_actuator_cfg_t *r = runtime ? &runtime->actuators[j] : NULL;
                thermal_status_t s = parse_actuator(ctx, ak, (uint8_t)j,
                                                    cfg, r, elem_path);
                if (s != THERMAL_OK) return s;
                ak = skip_token(ctx->toks, ak);
            }
            cfg->actuator_count = (uint8_t)an;
            if (runtime) runtime->actuator_count = (uint8_t)an;
        }
        /* Other top-level keys handled in pass B. */
        k = skip_token(ctx->toks, v);
    }
    return THERMAL_OK;
}

/* Pass B: scan top-level keys; parse everything pass A didn't.
 * Also validates that there are no unknown top-level keys.
 * runtime may be NULL. */
static thermal_status_t pass_b_top_level(parse_ctx_t *ctx,
                                         thermal_config_t *cfg,
                                         thermalcored_runtime_cfg_t *runtime)
{
    int n = ctx->toks[0].size;
    int k = 1;
    int saw_config_version  = 0;
    int saw_control_period  = 0;

    for (int i = 0; i < n; i++) {
        int v = k + 1;
        char key_str[THERMAL_NAME_MAX];
        if (tok_str_copy(ctx, k, key_str, sizeof(key_str)) != 0) {
            return set_err(ctx, THERMAL_ERR_INVALID_ARG, "$", "non-string top-level key");
        }

        if (tok_str_eq(ctx, k, "config_version")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, "config_version",
                               "expected integer in [0, 65535]");
            }
            cfg->config_version = (uint16_t)val;
            saw_config_version = 1;
        } else if (tok_str_eq(ctx, k, "control_period_ms")) {
            long long val;
            if (tok_parse_int_range(ctx, v, 0, UINT16_MAX, &val) != 0) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, "control_period_ms",
                               "expected integer in [0, 65535]");
            }
            cfg->control_period_ms = (uint16_t)val;
            saw_control_period = 1;
        } else if (tok_str_eq(ctx, k, "sensors") ||
                   tok_str_eq(ctx, k, "context_signals") ||
                   tok_str_eq(ctx, k, "actuators")) {
            /* handled by pass A */
        } else if (tok_str_eq(ctx, k, "zones")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, "zones", "expected array");
            }
            int an = ctx->toks[v].size;
            if (an > THERMAL_MAX_ZONES) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, "zones", "too many zones");
            }
            int ak = v + 1;
            for (int j = 0; j < an; j++) {
                char elem_path[PATH_MAX_LEN + 32];
                snprintf(elem_path, sizeof(elem_path), "zones[%d]", j);
                thermal_status_t s = parse_zone(ctx, ak, cfg,
                                                &cfg->zones[j], elem_path);
                if (s != THERMAL_OK) return s;
                ak = skip_token(ctx->toks, ak);
            }
            cfg->zone_count = (uint8_t)an;
        } else if (tok_str_eq(ctx, k, "policy_modifiers")) {
            if (ctx->toks[v].type != JSMN_ARRAY) {
                return set_err(ctx, THERMAL_ERR_INVALID_ARG, "policy_modifiers", "expected array");
            }
            int an = ctx->toks[v].size;
            if (an > THERMAL_MAX_MODIFIERS) {
                return set_err(ctx, THERMAL_ERR_NO_SPACE, "policy_modifiers", "too many modifiers");
            }
            int ak = v + 1;
            for (int j = 0; j < an; j++) {
                char elem_path[PATH_MAX_LEN + 32];
                snprintf(elem_path, sizeof(elem_path), "policy_modifiers[%d]", j);
                thermal_status_t s = parse_modifier(ctx, ak, cfg,
                                                    &cfg->modifiers[j], elem_path);
                if (s != THERMAL_OK) return s;
                ak = skip_token(ctx->toks, ak);
            }
            cfg->modifier_count = (uint8_t)an;
        } else if (tok_str_eq(ctx, k, "fault_detection")) {
            thermal_status_t s = parse_fault_detection_object(
                                     ctx, v, cfg, &cfg->faults, "fault_detection");
            if (s != THERMAL_OK) return s;
        } else if (tok_str_eq(ctx, k, "telemetry")) {
            runtime_global_cfg_t *r_glob = runtime ? &runtime->global : NULL;
            thermal_status_t s = parse_telemetry_object(ctx, v, cfg,
                                                        &cfg->telemetry,
                                                        r_glob, "telemetry");
            if (s != THERMAL_OK) return s;
        } else if (tok_str_eq(ctx, k, "control")) {
            runtime_global_cfg_t *r_glob = runtime ? &runtime->global : NULL;
            thermal_status_t s = parse_control_object(ctx, v, r_glob, "control");
            if (s != THERMAL_OK) return s;
        } else if (tok_str_eq(ctx, k, "hil")) {
            runtime_global_cfg_t *r_glob = runtime ? &runtime->global : NULL;
            thermal_status_t s = parse_hil_object(ctx, v, r_glob, "hil");
            if (s != THERMAL_OK) return s;
        } else {
            return set_errf(ctx, THERMAL_ERR_INVALID_ARG, "$",
                            "unknown top-level key '%s'", key_str);
        }
        k = skip_token(ctx->toks, v);
    }

    if (!saw_config_version) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, "$",
                       "required field 'config_version' missing");
    }
    if (!saw_control_period) {
        return set_err(ctx, THERMAL_ERR_INVALID_ARG, "$",
                       "required field 'control_period_ms' missing");
    }
    return THERMAL_OK;
}

/* =============================================================== */
/* Public entry point                                               */
/* =============================================================== */

thermal_status_t thermal_config_jsmn_parse(const char *json_text,
                                           size_t json_len,
                                           thermal_config_t *cfg,
                                           thermalcored_runtime_cfg_t *runtime,
                                           char *err_msg,
                                           size_t err_msg_size)
{
    if (!json_text || !cfg) return THERMAL_ERR_INVALID_ARG;
    if (json_len > INT_MAX) return THERMAL_ERR_NO_SPACE;

    if (err_msg && err_msg_size > 0) err_msg[0] = '\0';
    memset(cfg, 0, sizeof(*cfg));
    if (runtime) memset(runtime, 0, sizeof(*runtime));

    /* JSMN tokenisation on a stack-resident token array. */
    jsmntok_t tokens[JSMN_MAX_TOKENS];
    jsmn_parser parser;
    jsmn_init(&parser);
    int n_toks = jsmn_parse(&parser, json_text, json_len,
                            tokens, JSMN_MAX_TOKENS);

    parse_ctx_t ctx = {
        .json     = json_text,
        .toks     = tokens,
        .n_toks   = 0,
        .err      = err_msg,
        .err_size = err_msg_size,
    };

    if (n_toks == JSMN_ERROR_NOMEM) {
        return set_err(&ctx, THERMAL_ERR_NO_SPACE, "$", "exceeded JSMN token budget");
    }
    if (n_toks == JSMN_ERROR_INVAL) {
        return set_err(&ctx, THERMAL_ERR_INVALID_ARG, "$", "invalid character in JSON");
    }
    if (n_toks == JSMN_ERROR_PART) {
        return set_err(&ctx, THERMAL_ERR_INVALID_ARG, "$", "incomplete JSON document");
    }
    if (n_toks < 1) {
        return set_err(&ctx, THERMAL_ERR_INVALID_ARG, "$", "empty JSON document");
    }
    ctx.n_toks = n_toks;

    thermal_status_t s;
    if ((s = pass_a_top_level(&ctx, cfg, runtime)) != THERMAL_OK) return s;
    if ((s = pass_b_top_level(&ctx, cfg, runtime)) != THERMAL_OK) return s;

    /* Final semantic gate — PRD §5.3 rules live in core/. */
    s = thermal_core_validate_config(cfg);
    if (s != THERMAL_OK) {
        if (err_msg && err_msg_size > 0 && err_msg[0] == '\0') {
            snprintf(err_msg, err_msg_size,
                     "validate_config: rejected (status=%d)", (int)s);
        }
    }
    return s;
}
