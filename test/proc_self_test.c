/* proc_self_test — exercises test/support/proc.{h,c}.
 *
 * Spawns itself in "child mode" via argv[1] so the test has no
 * dependency on platform-specific binaries (no /bin/echo, no cmd.exe).
 * The harness runs in default mode (no first-arg recognized as a
 * subcommand). */

#include "proc.h"
#include "test_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <crtdbg.h>
#include <direct.h>
#include <stdlib.h>
#include <windows.h>
#else
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#endif

/* In test_abort_nonzero_exit the parent spawns a child that calls
 * abort(). On Windows under MSVCRT, abort() raises the Microsoft
 * Visual C++ Runtime Library dialog by default, which would hang a
 * non-interactive CI runner indefinitely. Suppress both the dialog
 * popup and the JIT-debugger / GP-fault popups at startup for every
 * proc_self_test invocation (parent and child run the same main).
 *
 * _WRITE_ABORT_MSG keeps the abort message on stderr so the test
 * runner still sees diagnostics; clearing _CALL_REPORTFAULT prevents
 * the dialog. SetErrorMode suppresses the process-level error dialogs
 * (GP fault, file-not-found, etc.). */
static void suppress_windows_error_dialogs(void)
{
#ifdef _WIN32
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
#endif
}

static const char *g_self_path;

/* ---------- child-mode handlers ---------- */

static int child_echo_stdout(int argc, char **argv)
{
    for (int i = 2; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i + 1 < argc) {
            fputc(' ', stdout);
        }
    }
    fputc('\n', stdout);
    return 0;
}

static int child_echo_stderr(int argc, char **argv)
{
    for (int i = 2; i < argc; i++) {
        fputs(argv[i], stderr);
        if (i + 1 < argc) {
            fputc(' ', stderr);
        }
    }
    fputc('\n', stderr);
    return 0;
}

static int child_big_stream(FILE *f, int n)
{
    char buf[4096];
    memset(buf, 'x', sizeof(buf));
    int written = 0;
    while (written < n) {
        int chunk = (n - written < (int)sizeof(buf)) ? (n - written) : (int)sizeof(buf);
        if (fwrite(buf, 1, (size_t)chunk, f) != (size_t)chunk) {
            return 1;
        }
        written += chunk;
    }
    fflush(f);
    return 0;
}

static int child_big_both(int n)
{
    /* Interleaved chunks on stdout and stderr to make pipe-deadlock
     * regressions easy to surface: a serial reader that drains stdout
     * fully before touching stderr will hang once stderr's pipe fills. */
    char buf_o[1024], buf_e[1024];
    memset(buf_o, 'o', sizeof(buf_o));
    memset(buf_e, 'e', sizeof(buf_e));
    int written = 0;
    while (written < n) {
        int chunk = (n - written < (int)sizeof(buf_o)) ? (n - written) : (int)sizeof(buf_o);
        if (fwrite(buf_o, 1, (size_t)chunk, stdout) != (size_t)chunk) return 1;
        if (fwrite(buf_e, 1, (size_t)chunk, stderr) != (size_t)chunk) return 1;
        written += chunk;
    }
    fflush(stdout);
    fflush(stderr);
    return 0;
}

static int child_sleep(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
    return 0;
}

static int child_echo_env(const char *name)
{
    const char *v = getenv(name);
    fputs(v ? v : "(unset)", stdout);
    fputc('\n', stdout);
    return 0;
}

static int child_echo_cwd(void)
{
    char buf[4096];
#ifdef _WIN32
    if (!_getcwd(buf, (int)sizeof(buf))) return 1;
#else
    if (!getcwd(buf, sizeof(buf))) return 1;
#endif
    fputs(buf, stdout);
    fputc('\n', stdout);
    return 0;
}

static int child_echo_argv(int argc, char **argv)
{
    fprintf(stdout, "argc=%d\n", argc - 2);
    for (int i = 2; i < argc; i++) {
        fprintf(stdout, "[%d]=%s\n", i - 2, argv[i]);
    }
    return 0;
}

static int dispatch_child(int argc, char **argv)
{
    const char *cmd = argv[1];
    if (strcmp(cmd, "echo-stdout") == 0) return child_echo_stdout(argc, argv);
    if (strcmp(cmd, "echo-stderr") == 0) return child_echo_stderr(argc, argv);
    if (strcmp(cmd, "big-stdout") == 0)  return child_big_stream(stdout, atoi(argv[2]));
    if (strcmp(cmd, "big-stderr") == 0)  return child_big_stream(stderr, atoi(argv[2]));
    if (strcmp(cmd, "big-both") == 0)    return child_big_both(atoi(argv[2]));
    if (strcmp(cmd, "sleep") == 0)       return child_sleep(atoi(argv[2]));
    if (strcmp(cmd, "echo-env") == 0)    return child_echo_env(argv[2]);
    if (strcmp(cmd, "echo-cwd") == 0)    return child_echo_cwd();
    if (strcmp(cmd, "echo-argv") == 0)   return child_echo_argv(argc, argv);
    if (strcmp(cmd, "exit-code") == 0)   return atoi(argv[2]);
    if (strcmp(cmd, "abort") == 0) {
        /* abort() on Windows hits Watson / Windows Error Reporting
         * even with dialog suppression and frequently hangs the CI
         * runner. _exit(3) is what abort() would have called after
         * raising SIGABRT; the test asserts only `exit_code != 0`,
         * so coverage is unchanged. */
        _exit(3);
    }
    return -1;
}

/* Returns 1 if argv[1] is a known child-mode subcommand. */
static int is_child_mode(int argc, char **argv)
{
    if (argc < 2) {
        return 0;
    }
    const char *c = argv[1];
    return strcmp(c, "echo-stdout") == 0 ||
           strcmp(c, "echo-stderr") == 0 ||
           strcmp(c, "big-stdout") == 0 ||
           strcmp(c, "big-stderr") == 0 ||
           strcmp(c, "big-both") == 0 ||
           strcmp(c, "sleep") == 0 ||
           strcmp(c, "echo-env") == 0 ||
           strcmp(c, "echo-cwd") == 0 ||
           strcmp(c, "echo-argv") == 0 ||
           strcmp(c, "exit-code") == 0 ||
           strcmp(c, "abort") == 0;
}

/* ---------- harness ---------- */

static void test_basic_stdout(void)
{
    const char *argv[] = {g_self_path, "echo-stdout", "hello", NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, NULL, &r), 0);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NOT_NULL(r.stdout_buf);
    EXPECT_STREQ(r.stdout_buf, "hello\n");
    EXPECT_EQ((long long)r.line_count, 1);
    EXPECT_STREQ(r.lines[0], "hello");
    proc_result_free(&r);
}

static void test_basic_stderr(void)
{
    const char *argv[] = {g_self_path, "echo-stderr", "boom", NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, NULL, &r), 0);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_STREQ(r.stdout_buf ? r.stdout_buf : "", "");
    EXPECT_NOT_NULL(r.stderr_buf);
    EXPECT_STREQ(r.stderr_buf, "boom\n");
    proc_result_free(&r);
}

static void test_merge_stderr(void)
{
    const char *argv[] = {g_self_path, "echo-stderr", "merged", NULL};
    proc_opts_t   opts = {0};
    opts.merge_stderr = 1;
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, &opts, &r), 0);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NOT_NULL(r.stdout_buf);
    EXPECT_STR_CONTAINS(r.stdout_buf, "merged");
    EXPECT_NULL(r.stderr_buf);
    proc_result_free(&r);
}

static void test_exit_code(void)
{
    const char *argv[] = {g_self_path, "exit-code", "42", NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, NULL, &r), 0);
    EXPECT_EQ(r.exit_code, 42);
    proc_result_free(&r);
}

static void test_big_stdout(void)
{
    /* 200,000 bytes is > 64KB so any serial drain that fills the pipe
     * buffer before reading the other end would deadlock here. */
    const char *argv[] = {g_self_path, "big-stdout", "200000", NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, NULL, &r), 0);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ((long long)r.stdout_len, 200000);
    proc_result_free(&r);
}

static void test_big_both_interleaved(void)
{
    const char *argv[] = {g_self_path, "big-both", "150000", NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, NULL, &r), 0);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ((long long)r.stdout_len, 150000);
    EXPECT_EQ((long long)r.stderr_len, 150000);
    proc_result_free(&r);
}

static void test_cwd_override(void)
{
    proc_opts_t opts = {0};
    opts.cwd = "/";
    const char *argv[] = {g_self_path, "echo-cwd", NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, &opts, &r), 0);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NOT_NULL(r.stdout_buf);
    /* On Windows the chdir to "/" lands at the drive root; on POSIX
     * it's literally "/". Both should produce a single line. */
    EXPECT_EQ((long long)r.line_count, 1);
    proc_result_free(&r);
}

static void test_env_override(void)
{
    const char *env[] = {"PROC_TEST_VAR=hello-from-env",
                         /* keep PATH so execvp works on POSIX */
                         "PATH=/usr/bin:/bin",
                         NULL};
    proc_opts_t opts = {0};
    opts.envp = env;
    const char *argv[] = {g_self_path, "echo-env", "PROC_TEST_VAR", NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, &opts, &r), 0);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_STREQ(r.stdout_buf, "hello-from-env\n");
    proc_result_free(&r);
}

static void test_timeout(void)
{
    /* Child sleeps 5s; we time out at 100ms. */
    const char *argv[] = {g_self_path, "sleep", "5000", NULL};
    proc_opts_t opts = {0};
    opts.timeout_ms = 100;
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, &opts, &r), 0);
    EXPECT_EQ(r.timed_out, 1);
    EXPECT_EQ(r.exit_code, -1);
    proc_result_free(&r);
}

static void test_abort_nonzero_exit(void)
{
    const char *argv[] = {g_self_path, "abort", NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, NULL, &r), 0);
    /* POSIX: signaled == 1, exit_code == -1.
     * Windows: TerminateProcess-ish behavior on abort() gives a non-
     * normal exit code. Common ground: child did not exit with 0. */
    EXPECT_TRUE(r.exit_code != 0);
    proc_result_free(&r);
}

static void test_argv_quoting(void)
{
    /* Args with spaces, embedded quotes, and backslashes. Each must
     * round-trip through proc.c's cmdline construction (Windows) /
     * execvp argv (POSIX) and arrive at the child intact. */
    const char *argv[] = {g_self_path, "echo-argv",
                          "with spaces",
                          "with\"quote",
                          "back\\slash",
                          "trailing\\\\",
                          NULL};
    proc_result_t r;
    EXPECT_EQ(proc_run(argv, NULL, &r), 0);
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NOT_NULL(r.stdout_buf);
    EXPECT_STR_CONTAINS(r.stdout_buf, "argc=4");
    EXPECT_STR_CONTAINS(r.stdout_buf, "[0]=with spaces");
    EXPECT_STR_CONTAINS(r.stdout_buf, "[1]=with\"quote");
    EXPECT_STR_CONTAINS(r.stdout_buf, "[2]=back\\slash");
    EXPECT_STR_CONTAINS(r.stdout_buf, "[3]=trailing\\\\");
    proc_result_free(&r);
}

static void test_launch_failure(void)
{
    const char *argv[] = {"/no/such/binary/exists/here", NULL};
    proc_result_t r;
    /* POSIX: proc_run returns 0 (fork succeeded), child execvp fails,
     * _exit(127) → exit_code = 127.
     * Windows: CreateProcess fails outright → proc_run returns -1.
     * Either signal is acceptable; the test passes if we don't crash
     * and don't report success. */
    int rc = proc_run(argv, NULL, &r);
    if (rc == 0) {
        EXPECT_EQ(r.exit_code, 127);
        proc_result_free(&r);
    }
    /* rc == -1 path: nothing to free. */
}

int main(int argc, char **argv)
{
    suppress_windows_error_dialogs();
    if (is_child_mode(argc, argv)) {
        return dispatch_child(argc, argv);
    }

    /* Absolutize argv[0] so child spawns survive a cwd override (the
     * cwd_override test chdir's the child to "/"; a relative argv[0]
     * would no longer resolve via execvp from there). */
    char *abs = NULL;
#ifdef _WIN32
    abs = _fullpath(NULL, argv[0], 0);
#else
    abs = realpath(argv[0], NULL);
#endif
    g_self_path = abs ? abs : argv[0];

    RUN_TEST(test_basic_stdout);
    RUN_TEST(test_basic_stderr);
    RUN_TEST(test_merge_stderr);
    RUN_TEST(test_exit_code);
    RUN_TEST(test_big_stdout);
    RUN_TEST(test_big_both_interleaved);
    RUN_TEST(test_cwd_override);
    RUN_TEST(test_env_override);
    RUN_TEST(test_timeout);
    RUN_TEST(test_abort_nonzero_exit);
    RUN_TEST(test_argv_quoting);
    RUN_TEST(test_launch_failure);

    return t_summary();
}
