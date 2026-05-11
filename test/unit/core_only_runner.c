/* test/unit/core_only_runner.c
 *
 * Stage 0 portability-guard runtime check, paired with the static nm -u
 * check in CI. Links only against core/libthermal_core.a + this file +
 * harness.h, with -Wl,--wrap=<forbidden> on the link line so any call
 * from core/ to a wrapped symbol aborts.
 *
 * Per implementation plan §5 Stage 0: "Because the binary contains no
 * platform/test code, any wrapped call is necessarily a core call —
 * no caller-attribution magic required."
 *
 * The wrap set is intentionally a small representative subset of the
 * full forbidden set; the authoritative check is the static nm -u
 * against ci/core-symbol-denylist.txt.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "harness.h"
#include "thermal_core.h"

static void abort_forbidden(const char *name) {
    fprintf(stderr,
            "FATAL: core/ called forbidden symbol %s "
            "— see ci/core-symbol-denylist.txt\n",
            name);
    abort();
}

void *__wrap_malloc(size_t s)                          { (void)s;                    abort_forbidden("malloc");  return NULL; }
void *__wrap_calloc(size_t n, size_t s)                { (void)n; (void)s;           abort_forbidden("calloc");  return NULL; }
void *__wrap_realloc(void *p, size_t s)                { (void)p; (void)s;           abort_forbidden("realloc"); return NULL; }
void  __wrap_free(void *p)                             { (void)p;                    abort_forbidden("free");                 }
long  __wrap_read(int fd, void *b, size_t n)           { (void)fd; (void)b; (void)n; abort_forbidden("read");    return -1;   }
long  __wrap_write(int fd, const void *b, size_t n)    { (void)fd; (void)b; (void)n; abort_forbidden("write");   return -1;   }

TEST_CASE(core_only_no_forbidden_calls) {
    /* Stage 1 onward: exercise the real API surface from inside the
     * portability check. validate_config returns THERMAL_ERR_UNAVAILABLE
     * until Stage 9 fills in the body. Extended as real core functions
     * land. */
    thermal_status_t s = thermal_core_validate_config(NULL);
    EXPECT_EQ(s, THERMAL_ERR_UNAVAILABLE);
}
