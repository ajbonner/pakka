#include "proc.h"

#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} buf_t;

static int buf_append(buf_t *b, const char *src, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t new_cap = b->cap ? b->cap : 4096;
        while (new_cap < b->len + n + 1) {
            new_cap *= 2;
        }
        char *p = (char *)realloc(b->data, new_cap);
        if (!p) {
            return -1;
        }
        b->data = p;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static void finalize_buf(buf_t *b, char **out_buf, size_t *out_len)
{
    /* Normalize CRLF → LF on output so test assertions written against
     * Unix line endings work on Windows. */
    if (b->data) {
        b->len = text_strip_crlf(b->data, b->len);
        b->data[b->len] = '\0';
    }
    *out_buf = b->data;
    *out_len = b->len;
}

void proc_result_free(proc_result_t *r)
{
    if (!r) {
        return;
    }
    free(r->stdout_buf);
    free(r->stderr_buf);
    text_free_lines(r->lines, r->line_count);
    memset(r, 0, sizeof(*r));
}

#ifndef _WIN32

/* ---------- POSIX implementation ---------- */

static long elapsed_ms_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L +
           (now.tv_nsec - start->tv_nsec) / 1000000L;
}

int proc_run(const char *const argv[], const proc_opts_t *opts, proc_result_t *out)
{
    memset(out, 0, sizeof(*out));
    proc_opts_t o = {0};
    if (opts) {
        o = *opts;
    }

    int sp[2];
    int ep[2] = {-1, -1};
    if (pipe(sp) != 0) {
        return -1;
    }
    if (!o.merge_stderr && pipe(ep) != 0) {
        close(sp[0]);
        close(sp[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(sp[0]);
        close(sp[1]);
        if (ep[0] >= 0) {
            close(ep[0]);
            close(ep[1]);
        }
        return -1;
    }

    if (pid == 0) {
        /* Child */
        dup2(sp[1], STDOUT_FILENO);
        dup2(o.merge_stderr ? sp[1] : ep[1], STDERR_FILENO);
        close(sp[0]);
        close(sp[1]);
        if (ep[0] >= 0) {
            close(ep[0]);
            close(ep[1]);
        }
        if (o.cwd && chdir(o.cwd) != 0) {
            _exit(127);
        }
        if (o.envp) {
            execve(argv[0], (char *const *)argv, (char *const *)o.envp);
        } else {
            execvp(argv[0], (char *const *)argv);
        }
        _exit(127);
    }

    /* Parent */
    close(sp[1]);
    if (ep[1] >= 0) {
        close(ep[1]);
    }

    /* Non-blocking reads so a single poll() call can drain whichever
     * end has data without blocking on the other. */
    fcntl(sp[0], F_SETFL, O_NONBLOCK);
    if (ep[0] >= 0) {
        fcntl(ep[0], F_SETFL, O_NONBLOCK);
    }

    buf_t sb = {0};
    buf_t eb = {0};

    struct pollfd fds[2];
    fds[0].fd      = sp[0];
    fds[0].events  = POLLIN;
    fds[1].fd      = ep[0];
    fds[1].events  = POLLIN;
    int stdout_open = 1;
    int stderr_open = (ep[0] >= 0);

    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    int timed_out = 0;

    while (stdout_open || stderr_open) {
        int poll_timeout = -1;
        if (o.timeout_ms > 0) {
            long remaining = (long)o.timeout_ms - elapsed_ms_since(&start_ts);
            if (remaining <= 0) {
                timed_out = 1;
                break;
            }
            poll_timeout = (int)remaining;
        }

        int n = poll(fds, 2, poll_timeout);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0 && o.timeout_ms > 0) {
            timed_out = 1;
            break;
        }

        for (int i = 0; i < 2; i++) {
            int *open_flag = (i == 0) ? &stdout_open : &stderr_open;
            buf_t *buf    = (i == 0) ? &sb : &eb;
            if (!*open_flag) {
                continue;
            }
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            char    tmp[4096];
            ssize_t r            = read(fds[i].fd, tmp, sizeof(tmp));
            int     should_close = 0;
            if (r > 0) {
                if (buf_append(buf, tmp, (size_t)r) < 0) {
                    should_close = 1;
                }
            } else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
                                  errno != EINTR)) {
                /* r == 0: EOF.
                 * r <  0: fatal error (anything besides the transient
                 *         EAGAIN / EWOULDBLOCK / EINTR triad). */
                should_close = 1;
            }
            if (should_close) {
                *open_flag = 0;
                fds[i].fd  = -1;
            }
        }
    }

    if (timed_out) {
        kill(pid, SIGKILL);
    }

    close(sp[0]);
    if (ep[0] >= 0) {
        close(ep[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* retry */
    }

    if (timed_out) {
        out->timed_out = 1;
        out->exit_code = -1;
    } else if (WIFEXITED(status)) {
        out->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out->signaled  = 1;
        out->exit_code = -1;
    }

    finalize_buf(&sb, &out->stdout_buf, &out->stdout_len);
    if (!o.merge_stderr) {
        finalize_buf(&eb, &out->stderr_buf, &out->stderr_len);
    }
    out->lines = text_split_lines(
        out->stdout_buf ? out->stdout_buf : "", out->stdout_len, &out->line_count);
    return 0;
}

#else /* _WIN32 */

/* ---------- Windows implementation ---------- */

typedef struct {
    HANDLE handle;
    buf_t *buf;
} reader_ctx_t;

static unsigned __stdcall reader_thread(void *arg)
{
    reader_ctx_t *r = (reader_ctx_t *)arg;
    char          tmp[4096];
    DWORD         got = 0;
    while (ReadFile(r->handle, tmp, (DWORD)sizeof(tmp), &got, NULL) && got > 0) {
        if (buf_append(r->buf, tmp, (size_t)got) < 0) {
            break;
        }
    }
    return 0;
}

/* Convert UTF-8 → UTF-16 via MultiByteToWideChar. Caller frees. NULL
 * input returns NULL. */
static wchar_t *utf8_to_wide(const char *utf8)
{
    if (!utf8) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

/* Build a CreateProcessW cmdline from argv (UTF-8) following MSVCRT
 * parsing rules: backslashes are literal except when preceding a
 * double-quote, in which case 2N backslashes + " emits N literal
 * backslashes and a literal quote. Each argv entry is UTF-8 → UTF-16
 * converted first; the quoting walk then operates on wide chars (all
 * structural characters — space, tab, ", \\ — are ASCII so the rules
 * carry across unchanged). */
static wchar_t *build_cmdline_w(const char *const argv[])
{
    size_t   cap = 256;
    size_t   len = 0;
    wchar_t *out = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!out) {
        return NULL;
    }

#define ENSURE_W(n)                                                        \
    do {                                                                   \
        if (len + (n) + 1 > cap) {                                         \
            size_t newcap = cap;                                           \
            while (newcap < len + (n) + 1) newcap *= 2;                    \
            wchar_t *_p = (wchar_t *)realloc(out, newcap * sizeof(wchar_t)); \
            if (!_p) { free(out); return NULL; }                           \
            out = _p; cap = newcap;                                        \
        }                                                                  \
    } while (0)

    for (size_t i = 0; argv[i]; i++) {
        wchar_t *aw = utf8_to_wide(argv[i]);
        if (!aw) {
            free(out);
            return NULL;
        }
        if (i > 0) {
            ENSURE_W(1);
            out[len++] = L' ';
        }
        int needs_quote = (*aw == L'\0' || wcspbrk(aw, L" \t\"") != NULL);
        if (needs_quote) {
            ENSURE_W(1);
            out[len++] = L'"';
        }
        size_t backslashes = 0;
        for (size_t j = 0; aw[j]; j++) {
            if (aw[j] == L'\\') {
                backslashes++;
            } else if (aw[j] == L'"') {
                ENSURE_W(backslashes * 2 + 2);
                for (size_t k = 0; k < backslashes * 2; k++) out[len++] = L'\\';
                out[len++] = L'\\';
                out[len++] = L'"';
                backslashes = 0;
            } else {
                ENSURE_W(backslashes + 1);
                for (size_t k = 0; k < backslashes; k++) out[len++] = L'\\';
                backslashes  = 0;
                out[len++]   = aw[j];
            }
        }
        if (needs_quote) {
            ENSURE_W(backslashes * 2 + 1);
            for (size_t k = 0; k < backslashes * 2; k++) out[len++] = L'\\';
            out[len++] = L'"';
        } else {
            ENSURE_W(backslashes);
            for (size_t k = 0; k < backslashes; k++) out[len++] = L'\\';
        }
        free(aw);
    }
    ENSURE_W(1);
    out[len] = L'\0';
    return out;
#undef ENSURE_W
}

/* Build a CreateProcessW lpEnvironment block: wchar_t array with each
 * "K=V" NUL-terminated, double-NUL at end. Each entry is UTF-8 → UTF-16
 * converted from the input array. */
static wchar_t *build_env_block_w(const char *const *envp)
{
    /* First pass: convert each entry, accumulate lengths. */
    size_t   n_entries = 0;
    while (envp[n_entries]) n_entries++;

    wchar_t **wide = (wchar_t **)calloc(n_entries + 1, sizeof(wchar_t *));
    if (!wide) return NULL;
    size_t total = 2; /* trailing double-NUL */
    for (size_t i = 0; i < n_entries; i++) {
        wide[i] = utf8_to_wide(envp[i]);
        if (!wide[i]) {
            for (size_t j = 0; j < i; j++) free(wide[j]);
            free(wide);
            return NULL;
        }
        total += wcslen(wide[i]) + 1;
    }

    wchar_t *block = (wchar_t *)malloc(total * sizeof(wchar_t));
    if (!block) {
        for (size_t i = 0; i < n_entries; i++) free(wide[i]);
        free(wide);
        return NULL;
    }
    size_t pos = 0;
    for (size_t i = 0; i < n_entries; i++) {
        size_t l = wcslen(wide[i]) + 1; /* include NUL */
        memcpy(block + pos, wide[i], l * sizeof(wchar_t));
        pos += l;
        free(wide[i]);
    }
    free(wide);
    block[pos]     = L'\0';
    block[pos + 1] = L'\0';
    return block;
}

int proc_run(const char *const argv[], const proc_opts_t *opts, proc_result_t *out)
{
    memset(out, 0, sizeof(*out));
    proc_opts_t o = {0};
    if (opts) {
        o = *opts;
    }

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength             = sizeof(sa);
    sa.bInheritHandle      = TRUE;

    HANDLE stdout_r = NULL, stdout_w = NULL;
    HANDLE stderr_r = NULL, stderr_w = NULL;

    if (!CreatePipe(&stdout_r, &stdout_w, &sa, 0)) {
        return -1;
    }
    SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);

    if (o.merge_stderr) {
        if (!DuplicateHandle(GetCurrentProcess(), stdout_w,
                             GetCurrentProcess(), &stderr_w,
                             0, TRUE, DUPLICATE_SAME_ACCESS)) {
            CloseHandle(stdout_r);
            CloseHandle(stdout_w);
            return -1;
        }
    } else {
        if (!CreatePipe(&stderr_r, &stderr_w, &sa, 0)) {
            CloseHandle(stdout_r);
            CloseHandle(stdout_w);
            return -1;
        }
        SetHandleInformation(stderr_r, HANDLE_FLAG_INHERIT, 0);
    }

    /* CreateProcessW + UTF-8 → UTF-16 conversion at the boundary lets
     * the test runner pass UTF-8 byte sequences in argv / env / cwd
     * without going through Windows ACP. Necessary for the
     * unicode_paths suite — pakka itself uses wmain on Windows and
     * receives wchar_t* argv from the OS's command-line parser, which
     * handles wide bytes correctly. CreateProcessA would funnel through
     * CP_ACP and mangle non-ASCII bytes on most installs (where ACP
     * is CP1252, not UTF-8). */
    wchar_t *cmdline = build_cmdline_w(argv);
    wchar_t *envblk  = o.envp ? build_env_block_w(o.envp) : NULL;
    wchar_t *cwd_w   = o.cwd ? utf8_to_wide(o.cwd) : NULL;
    if (!cmdline || (o.envp && !envblk) || (o.cwd && !cwd_w)) {
        free(cmdline);
        free(envblk);
        free(cwd_w);
        if (stdout_r) CloseHandle(stdout_r);
        if (stdout_w) CloseHandle(stdout_w);
        if (stderr_r) CloseHandle(stderr_r);
        if (stderr_w) CloseHandle(stderr_w);
        return -1;
    }

    STARTUPINFOW si = {0};
    si.cb           = sizeof(si);
    si.dwFlags      = STARTF_USESTDHANDLES;
    si.hStdInput    = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput   = stdout_w;
    si.hStdError    = stderr_w;

    PROCESS_INFORMATION pi = {0};
    BOOL                ok = CreateProcessW(NULL, cmdline, NULL, NULL, TRUE,
                                            CREATE_UNICODE_ENVIRONMENT,
                                            envblk, cwd_w, &si, &pi);
    free(cmdline);
    free(envblk);
    free(cwd_w);

    /* Parent must close the write ends so the read ends see EOF when
     * the child exits. */
    CloseHandle(stdout_w);
    CloseHandle(stderr_w);

    if (!ok) {
        CloseHandle(stdout_r);
        if (stderr_r) CloseHandle(stderr_r);
        return -1;
    }

    buf_t        sb   = {0};
    buf_t        eb   = {0};
    reader_ctx_t r1   = {stdout_r, &sb};
    reader_ctx_t r2   = {stderr_r, &eb};
    HANDLE       t1   = (HANDLE)_beginthreadex(NULL, 0, reader_thread, &r1, 0, NULL);
    HANDLE       t2   = NULL;
    if (!o.merge_stderr) {
        t2 = (HANDLE)_beginthreadex(NULL, 0, reader_thread, &r2, 0, NULL);
    }

    /* Without a stdout reader, the child can fill its pipe buffer and
     * block on write while we wait on the process — classic deadlock.
     * Treat reader-thread creation failure as launch failure: terminate,
     * drain handles, return -1. */
    if (!t1 || (!o.merge_stderr && !t2)) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (t1) { WaitForSingleObject(t1, INFINITE); CloseHandle(t1); }
        if (t2) { WaitForSingleObject(t2, INFINITE); CloseHandle(t2); }
        CloseHandle(stdout_r);
        if (stderr_r) CloseHandle(stderr_r);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        free(sb.data);
        free(eb.data);
        return -1;
    }

    DWORD wait_ms  = o.timeout_ms > 0 ? (DWORD)o.timeout_ms : INFINITE;
    DWORD wr       = WaitForSingleObject(pi.hProcess, wait_ms);
    int   timed_out = 0;
    if (wr == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        timed_out = 1;
    }

    if (t1) {
        WaitForSingleObject(t1, INFINITE);
        CloseHandle(t1);
    }
    if (t2) {
        WaitForSingleObject(t2, INFINITE);
        CloseHandle(t2);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(stdout_r);
    if (stderr_r) CloseHandle(stderr_r);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (timed_out) {
        out->timed_out = 1;
        out->exit_code = -1;
    } else {
        out->exit_code = (int)exit_code;
    }

    finalize_buf(&sb, &out->stdout_buf, &out->stdout_len);
    if (!o.merge_stderr) {
        finalize_buf(&eb, &out->stderr_buf, &out->stderr_len);
    }
    out->lines = text_split_lines(
        out->stdout_buf ? out->stdout_buf : "", out->stdout_len, &out->line_count);
    return 0;
}

#endif
