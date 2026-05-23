#ifndef PAKKA_TEST_PROC_H
#define PAKKA_TEST_PROC_H

#include <stddef.h>

typedef struct {
    /* Working directory for the child. NULL = inherit. */
    const char *cwd;

    /* Environment for the child. NULL = inherit. Otherwise a NULL-
     * terminated array of "NAME=VALUE" strings that REPLACES the entire
     * inherited environment (caller assembles it). */
    const char *const *envp;

    /* Wall-clock timeout in milliseconds. 0 = no timeout. On expiry the
     * child is SIGKILL'd (POSIX) / TerminateProcess'd (Windows). */
    int timeout_ms;

    /* If non-zero, merge stderr into stdout (matches bats `run` default). */
    int merge_stderr;
} proc_opts_t;

typedef struct {
    int      exit_code;       /* -1 if signaled / timed_out */
    int      signaled;        /* 1 if killed by signal */
    int      timed_out;       /* 1 if hit timeout_ms */
    char    *stdout_buf;      /* NUL-terminated; CRLF→LF normalized */
    size_t   stdout_len;
    char    *stderr_buf;      /* NULL if merge_stderr */
    size_t   stderr_len;
    char   **lines;           /* stdout split on '\n'; NULL-terminated array */
    size_t   line_count;
} proc_result_t;

/* Run argv (argv[0] is the executable; on POSIX, found via execvp).
 * Caller-supplied `out` is fully populated on success; call
 * proc_result_free when done. Returns 0 on launch success (regardless
 * of child exit code), -1 on launch failure. */
int proc_run(const char *const argv[], const proc_opts_t *opts, proc_result_t *out);

void proc_result_free(proc_result_t *r);

#endif
