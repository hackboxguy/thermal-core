/* core/thermal_arbitrator.h
 *
 * Per-actuator arbitration across zones (PRD §4.5 line 551 + §4.6 step 7).
 * v1 = max-wins. Pure function: caller passes a small uint8_t array of
 * per-zone demands directed at one actuator. Returns the highest.
 */
#ifndef THERMAL_ARBITRATOR_H
#define THERMAL_ARBITRATOR_H

#include <stdint.h>

uint8_t thermal_arbitrator_max_wins(const uint8_t *demands, uint8_t count);

#endif /* THERMAL_ARBITRATOR_H */
