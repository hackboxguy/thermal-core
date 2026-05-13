/* platform/esp32_idf/main/replay_fixture.c
 *
 * Stage 13 13c -- canonical fixture for REPLAY_STANDALONE.
 *
 * 100 ticks at temp_mc = 50000 / tach_rpm = 0, matching the
 * host `scenarios/idle_steady_state.scn`:
 *
 *     plant ambient_mc      50000
 *     plant initial_temp_mc 50000
 *     plant duration_ms     10000
 *
 * At control_period_ms = 100 the host daemon emits ~100 sample
 * rows for this scenario; the ESP32 REPLAY firmware will too,
 * which is what Stage 15 SHA-256-compares.
 *
 * Behaviour: at 50000 mc the step_wise zone's lowest trip
 * (30000 mc) is already exceeded -- but trips fire on rising
 * temperature past the threshold -- and the second trip
 * (45000 mc) sits below 50000 too, so we should see the
 * governor settle on cooling state 2 (state_pwm 160).  No
 * faults expected; no events.  Stage 15 will verify the exact
 * byte sequence; here we just ship the fixture.
 */
#include "replay_fixture.h"

const replay_tick_t G_REPLAY_TICKS[] = {
    /*  0.. 9 */
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    /* 10..19 */
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    /* 20..29 */
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    /* 30..39 */
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    /* 40..49 */
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    /* 50..59 */
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    /* 60..69 */
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    /* 70..79 */
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    /* 80..89 */
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    /* 90..99 */
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
    {50000,0}, {50000,0}, {50000,0}, {50000,0}, {50000,0},
};

const uint16_t G_REPLAY_TICK_COUNT =
    sizeof(G_REPLAY_TICKS) / sizeof(G_REPLAY_TICKS[0]);
