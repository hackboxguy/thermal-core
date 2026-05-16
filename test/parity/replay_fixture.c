/* test/parity/replay_fixture.c -- see replay_fixture.h for the contract.
 *
 * Ramp shape (control_period = 100 ms, so 312 ticks = 31.2 s):
 *
 *   ticks   0 .. 9    flat at 25000 mc   (filter settles)
 *   ticks  10 .. 150  ramp up   +500 mc/tick -> 95000 mc at tick 150
 *   ticks 151 .. 160  flat at 95000 mc
 *   ticks 161 .. 301  ramp down -500 mc/tick -> 25000 mc at tick 301
 *   ticks 302 .. 311  flat at 25000 mc
 *
 * tach_rpm is a deterministic function of temperature so the
 * actuator tach sample is not a flat zero -- it carries no
 * control meaning here (the standalone config enables no fault
 * detectors), it just keeps the input non-trivial.
 */
#include "replay_fixture.h"

replay_tick_t replay_fixture_tick(uint16_t i)
{
    int32_t temp;
    if      (i <  10) temp = 25000;
    else if (i < 151) temp = 25000 + (int32_t)(i -  10) * 500;
    else if (i < 161) temp = 95000;
    else if (i < 302) temp = 95000 - (int32_t)(i - 161) * 500;
    else              temp = 25000;

    replay_tick_t t;
    t.temp_mc  = temp;
    t.tach_rpm = (uint32_t)(temp / 50);
    return t;
}
