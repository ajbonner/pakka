#include "test_macros.h"

#include <stdlib.h>

int         t_failures = 0;
int         t_skipped  = 0;
int         t_passed   = 0;
const char *t_current_test = "(none)";

/* Per-test allocation arena. under_scratch() and similar helpers
 * track their malloc'd path strings here; t_test_end releases the
 * arena so each test starts with an empty pool. Capacity is overkill —
 * any single test allocates O(10) paths, not O(1024). */
#define T_ARENA_CAP 1024
static void *t_arena[T_ARENA_CAP];
static int   t_arena_n;

void *t_track(void *p)
{
    if (p && t_arena_n < T_ARENA_CAP) {
        t_arena[t_arena_n++] = p;
    }
    return p;
}

void t_untrack(void *p)
{
    if (!p) {
        return;
    }
    for (int i = 0; i < t_arena_n; i++) {
        if (t_arena[i] == p) {
            t_arena[i] = NULL;
            return;
        }
    }
}

static void t_arena_release(void)
{
    for (int i = 0; i < t_arena_n; i++) {
        free(t_arena[i]);
        t_arena[i] = NULL;
    }
    t_arena_n = 0;
}

static int t_failures_at_start;
static int t_skipped_at_start;

void t_test_begin(const char *name)
{
    t_current_test       = name;
    t_failures_at_start  = t_failures;
    t_skipped_at_start   = t_skipped;
    fprintf(stdout, "==> %s\n", name);
    fflush(stdout);
}

void t_test_end(void)
{
    /* FAIL and SKIP each print their own diagnostic; only the all-clear
     * path needs to emit "ok" here. */
    if (t_failures == t_failures_at_start && t_skipped == t_skipped_at_start) {
        t_passed++;
        fprintf(stdout, "  ok\n");
    }
    t_current_test = "(none)";
    fflush(stdout);
    /* Release per-test path arena so each test starts fresh. */
    t_arena_release();
}

int t_summary(void)
{
    fprintf(stdout, "\n%d passed, %d failed, %d skipped\n",
            t_passed, t_failures, t_skipped);
    fflush(stdout);
    return t_failures > 0 ? 1 : 0;
}
