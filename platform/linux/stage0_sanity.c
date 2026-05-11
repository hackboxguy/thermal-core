/* Stage 0 sanity build: verifies core/thermal_config.h compiles under the
 * daemon's flags. Removed in Stage 9 when the real thermalcored.c lands. */
#include "thermal_config.h"

int main(void) {
    return 0;
}
