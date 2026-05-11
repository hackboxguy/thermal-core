/* core/thermal_commands.h
 *
 * Runtime command surface. thermal_command_id_t is the single source of
 * truth for command-ID numeric values across core, wire (protocol/), and
 * tools (PRD §7.2 line 941). Do not redeclare these constants anywhere
 * else; wire encoders include this header.
 *
 * Sourced from PRD §4.3 (lines 251-273). The polymorphic thermal_command_t
 * is a typed payload — wire framing, CRC, and seq tracking live in
 * protocol/, not here.
 */
#ifndef THERMAL_COMMANDS_H
#define THERMAL_COMMANDS_H

#include <stdint.h>
#include "thermal_types.h"

typedef enum {
    THERMAL_CMD_SET_PID         = 0x0001,
    THERMAL_CMD_SET_SETPOINT    = 0x0002,
    THERMAL_CMD_SET_TRIP        = 0x0003,
    THERMAL_CMD_SET_CURVE_POINT = 0x0004,
    THERMAL_CMD_CLEAR_FAULT     = 0x0005
} thermal_command_id_t;

typedef struct {
    uint16_t command_id;        /* thermal_command_id_t */
    union {
        struct { uint16_t zone_id; int32_t kp_q16, ki_q16, kd_q16; }                    set_pid;
        struct { uint16_t zone_id; int32_t setpoint_mc; }                               set_setpoint;
        struct { uint16_t zone_id, trip_idx; int32_t temp_mc, hyst_mc; }                set_trip;
        struct { uint16_t modifier_id, point_idx; int32_t x, value0, value1; }          set_curve_point;
        struct { uint16_t fault_type, target_id; }                                      clear_fault;
    } u;
} thermal_command_t;

typedef struct {
    thermal_status_t status;
    uint32_t detail_code;       /* command/result-specific detail namespace */
} thermal_command_result_t;

#endif /* THERMAL_COMMANDS_H */
