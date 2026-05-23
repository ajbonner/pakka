#include "test_macros.h"

int         t_failures = 0;
int         t_skipped  = 0;
int         t_passed   = 0;
const char *t_current_test = "(none)";

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
}

int t_summary(void)
{
    fprintf(stdout, "\n%d passed, %d failed, %d skipped\n",
            t_passed, t_failures, t_skipped);
    fflush(stdout);
    return t_failures > 0 ? 1 : 0;
}
