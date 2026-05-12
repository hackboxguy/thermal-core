/* test/replay/pid_replay.c
 *
 * Stage 5 PID step-response module golden driver. Runs thermal_pid_step
 * directly (the function-level test, not via thermal_core_step), so the
 * golden can capture the full P/I/D telemetry that thermal_state_snapshot_t
 * doesn't expose. The thermal_core_step PID-dispatch wiring is verified
 * separately by core_only_runner's extended assertion.
 *
 * Profile: 50 mc -> 85 mc step at tick 10. 200 ticks total. Gains tuned
 * for visible response (kp=0.0125, ki=0.005, kd=0): cold-phase output
 * saturates low (anti-windup holds I=0); after step, P=125 PWM mid-range,
 * I grows ~5 PWM/tick until output saturates around tick 36; I then
 * freezes via anti-windup.
 *
 * The same generator and PID math live in test/reference/pid.py; both
 * must produce byte-equal CSV output.
 */
#include <stdio.h>
#include <stdint.h>
#include "thermal_pid.h"

#define N_TICKS    200
#define TICK_MS    100
#define KP_Q16     819     /* ~0.0125 PWM/mc */
#define KI_Q16     327     /* ~0.005  PWM/(mc*s) */
#define KD_Q16     0
#define SETPOINT   75000
#define DT_MIN     50
#define DT_MAX     500
#define PWM_MIN    0
#define PWM_MAX    255

static int32_t synth_temp(int tick) {
    return (tick < 10) ? 50000 : 85000;
}

int main(void) {
    thermal_pid_state_t s;
    thermal_pid_reset(&s);

    printf("tick,now_ms,temp_mc,error_mc,p_q16,i_q16,d_q16,output_pwm,sat_hi,sat_lo\n");
    for (int t = 0; t < N_TICKS; t++) {
        uint32_t now_ms = (uint32_t)(t * TICK_MS);
        int32_t temp = synth_temp(t);
        thermal_pid_step_result_t r;
        thermal_pid_step(&s, KP_Q16, KI_Q16, KD_Q16, SETPOINT, temp,
                         now_ms, DT_MIN, DT_MAX, PWM_MIN, PWM_MAX, &r);
        printf("%d,%u,%d,%d,%d,%d,%d,%d,%u,%u\n",
               t, (unsigned)now_ms, temp, temp - SETPOINT,
               r.p_term_q16, r.i_term_q16, r.d_term_q16,
               (int)r.output_pwm,
               (unsigned)r.saturated_high, (unsigned)r.saturated_low);
    }
    return 0;
}
