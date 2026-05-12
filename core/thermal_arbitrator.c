/* core/thermal_arbitrator.c -- v1 max-wins arbitration (PRD §4.5). */
#include "thermal_arbitrator.h"

uint8_t thermal_arbitrator_max_wins(const uint8_t *demands, uint8_t count) {
    uint8_t winner = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (demands[i] > winner) winner = demands[i];
    }
    return winner;
}
