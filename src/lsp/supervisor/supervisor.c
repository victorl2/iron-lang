/* Phase 7 Plan 07-01 Task 02 (HARD-13, D-01) -- Fork/proxy supervisor.
 *
 * See supervisor.h for the public contract. This TU implements:
 *
 *   - The backoff table (static const int s_backoff_secs[])
 *   - The 5-in-60s crash-window tracker (sliding ring of 5 timestamps)
 *   - The byte-forwarder (poll(2) over {editor_in, worker_out, self_pipe})
 *   - The synthetic window/showMessage frame builder (hand-rolled JSON
 *     with Content-Length header -- no yyjson dep to keep the
 *     supervisor transport-hermetic)
 *   - The fork + dup2 + re-argv child boot
 *   - SIGCHLD self-pipe handler
 *
 * Single-file: the supervisor is small (~300 LoC) and self-contained
 * per D-01. We do NOT include any Phase 2 transport headers; the only
 * LSP knowledge baked in is the on-wire frame format (`Content-Length:
 * <N>\r\n\r\n<body>`).
 *
 * T-07-01-03 mitigation (fork-bomb avoidance): the 5-in-60s bailout
 * guarantees the parent exits after at most 5 respawns in rapid
 * succession. The editor's LSP client then sees the whole process
 * group go down and surfaces the failure to the user (we don't eat
 * crashes silently).
 *
 * T-07-01-08 mitigation (zombie leak): SIGCHLD handler always calls
 * waitpid(WNOHANG) in a loop until it returns 0 or -1 -- the APUE §10
 * standard pattern. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsp/supervisor/supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include "util/os.h"
#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#endif

/* ── Tunables (plan-mandated literals) ──────────────────────────────── */

#define ILSP_SUPERVISOR_PIPE_BUF     65536u
#define ILSP_SUPERVISOR_BAILOUT_N    5u
#define ILSP_SUPERVISOR_BAILOUT_WIN  60    /* seconds */

static const int s_backoff_secs[] = { 1, 2, 4, 8, 16 };
static const unsigned s_backoff_len =
    sizeof(s_backoff_secs) / sizeof(s_backoff_secs[0]);

#ifndef _WIN32
/* ── Self-pipe for SIGCHLD delivery to the poll() loop ───────────────── */

static int s_sigchld_pipe[2] = { -1, -1 };

static void sigchld_handler(int signo) {
    (void)signo;
    /* Wake the poll() loop. A single byte is enough; repeated signals
     * coalesce into a single readable event. write(2) is AS-Safe. */
    int saved = errno;
    if (s_sigchld_pipe[1] >= 0) {
        unsigned char b = 1;
        ssize_t ignored = write(s_sigchld_pipe[1], &b, 1);
        (void)ignored;
    }
    errno = saved;
}

static int install_sigchld(void) {
    if (pipe(s_sigchld_pipe) != 0) return -1;
    /* Make both ends non-blocking. */
    int fl;
    fl = fcntl(s_sigchld_pipe[0], F_GETFL, 0);
    fcntl(s_sigchld_pipe[0], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(s_sigchld_pipe[1], F_GETFL, 0);
    fcntl(s_sigchld_pipe[1], F_SETFL, fl | O_NONBLOCK);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGCHLD, &sa, NULL);
}
#endif /* !_WIN32 */

/* ── Backoff + bailout ───────────────────────────────────────────────── */

static int backoff_secs_for(unsigned crash_count) {
    unsigned idx = (crash_count < s_backoff_len) ? crash_count
                                                 : (s_backoff_len - 1);
    return s_backoff_secs[idx];
}

/* 5-entry sliding window of crash timestamps. */
static int64_t s_crash_window[ILSP_SUPERVISOR_BAILOUT_N];
static unsigned s_crash_window_count = 0;

static int record_crash(int64_t now_secs) {
    /* Shift-insert: drop the oldest, append the newest. */
    if (s_crash_window_count < ILSP_SUPERVISOR_BAILOUT_N) {
        s_crash_window[s_crash_window_count++] = now_secs;
    } else {
        for (unsigned i = 1; i < ILSP_SUPERVISOR_BAILOUT_N; i++) {
            s_crash_window[i - 1] = s_crash_window[i];
        }
        s_crash_window[ILSP_SUPERVISOR_BAILOUT_N - 1] = now_secs;
    }
    /* Bailout when the oldest-recorded-crash is within the window. */
    if (s_crash_window_count >= ILSP_SUPERVISOR_BAILOUT_N) {
        int64_t oldest = s_crash_window[0];
        if ((now_secs - oldest) <= ILSP_SUPERVISOR_BAILOUT_WIN) {
            return 1;  /* BAIL */
        }
    }
    return 0;
}

/* ── Synthetic window/showMessage frame builder ─────────────────────── */

static size_t build_showmessage(char *out, size_t cap, const char *message) {
    /* Hand-rolled LSP frame: no yyjson dep. Message is embedded verbatim;
     * any double-quotes or backslashes inside must be escaped by the
     * caller (we hard-code a safe message so this is moot for v1). */
    char body[1024];
    int nb = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"method\":\"window/showMessage\","
        "\"params\":{\"type\":2,\"message\":\"%s\"}}",
        message ? message : "Iron LSP worker crashed and was restarted");
    if (nb <= 0 || (size_t)nb >= sizeof(body)) return 0;

    int nt = snprintf(out, cap,
                      "Content-Length: %d\r\n\r\n%s", nb, body);
    if (nt <= 0 || (size_t)nt >= cap) return 0;
    return (size_t)nt;
}

#ifndef _WIN32
/* ── Non-blocking byte forwarder for one direction ───────────────────── */

static ssize_t forward_bytes(int in_fd, int out_fd, size_t n) {
    char buf[ILSP_SUPERVISOR_PIPE_BUF];
    size_t to_copy = (n < sizeof(buf)) ? n : sizeof(buf);
    ssize_t r;
    do {
        r = read(in_fd, buf, to_copy);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) return r;

    size_t written = 0;
    while (written < (size_t)r) {
        ssize_t w;
        do {
            w = write(out_fd, buf + written, (size_t)r - written);
        } while (w < 0 && errno == EINTR);
        if (w < 0) {
            if (errno == EPIPE || errno == EBADF) return -1;
            return (ssize_t)written;
        }
        written += (size_t)w;
    }
    return (ssize_t)r;
}

/* ── Child boot: dup pipes + exec ourselves with --__worker ─────────── */

static int worker_child_run(int argc, char **argv,
                            int in_pipe_r, int out_pipe_w) {
    /* stdin <- editor-to-worker pipe read end */
    dup2(in_pipe_r, STDIN_FILENO);
    /* stdout -> worker-to-editor pipe write end */
    dup2(out_pipe_w, STDOUT_FILENO);
    close(in_pipe_r);
    close(out_pipe_w);

    /* Build argv without --supervised and with --__worker appended. */
    char **new_argv = (char **)calloc((size_t)argc + 2, sizeof(char *));
    if (!new_argv) _exit(127);
    int j = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--supervised") == 0) continue;
        new_argv[j++] = argv[i];
    }
    new_argv[j++] = (char *)"--__worker";
    new_argv[j]   = NULL;

    /* execve to our own binary so the worker is a fresh process (no
     * parent state carried over). /proc/self/exe on Linux; argv[0] as
     * fallback. */
#if defined(__linux__)
    execv("/proc/self/exe", new_argv);
#endif
    execvp(argv[0], new_argv);
    /* If execv returned, we're broken. */
    _exit(127);
}

/* ── Public entry ───────────────────────────────────────────────────── */

int ilsp_supervisor_run(int argc, char **argv) {
    if (install_sigchld() != 0) {
        fprintf(stderr, "ironls supervisor: sigchld/self-pipe init failed\n");
        return 1;
    }

    unsigned consecutive_crashes = 0;
    s_crash_window_count = 0;
    memset(s_crash_window, 0, sizeof(s_crash_window));

    while (1) {
        /* Spawn a new worker. */
        int e2w[2], w2e[2];
        if (pipe(e2w) != 0 || pipe(w2e) != 0) {
            fprintf(stderr, "ironls supervisor: pipe() failed errno=%d\n", errno);
            return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "ironls supervisor: fork() failed errno=%d\n", errno);
            return 1;
        }
        if (pid == 0) {
            /* CHILD */
            close(e2w[1]);  /* child reads e2w[0] */
            close(w2e[0]);  /* child writes w2e[1] */
            worker_child_run(argc, argv, e2w[0], w2e[1]);
            _exit(127);  /* unreachable */
        }
        /* PARENT */
        close(e2w[0]);
        close(w2e[1]);
        int editor_in  = STDIN_FILENO;
        int editor_out = STDOUT_FILENO;
        int worker_in  = e2w[1];
        int worker_out = w2e[0];

        bool worker_alive = true;
        bool editor_eof   = false;
        while (worker_alive) {
            struct pollfd pfd[3];
            pfd[0].fd = editor_eof ? -1 : editor_in;
            pfd[0].events = POLLIN;
            pfd[1].fd = worker_out;
            pfd[1].events = POLLIN;
            pfd[2].fd = s_sigchld_pipe[0];
            pfd[2].events = POLLIN;

            int pr = poll(pfd, 3, -1);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (pfd[1].revents & POLLIN) {
                ssize_t f = forward_bytes(worker_out, editor_out,
                                          ILSP_SUPERVISOR_PIPE_BUF);
                if (f <= 0 && !(f == -1 && errno == EAGAIN)) {
                    /* Worker stdout closed -- wait for SIGCHLD below. */
                }
            }
            if (pfd[0].revents & POLLIN) {
                ssize_t f = forward_bytes(editor_in, worker_in,
                                          ILSP_SUPERVISOR_PIPE_BUF);
                if (f == 0) {
                    editor_eof = true;
                    close(worker_in);
                    worker_in = -1;
                }
            }
            if (pfd[2].revents & POLLIN) {
                /* Drain self-pipe. */
                unsigned char drain[64];
                while (read(s_sigchld_pipe[0], drain, sizeof(drain)) > 0) {}
                /* Reap any dead children. */
                int status = 0;
                pid_t r;
                while ((r = waitpid(-1, &status, WNOHANG)) > 0) {
                    if (r == pid) worker_alive = false;
                }
            }
            /* Hang-up from editor on stdin-eof and worker clean exit: exit
             * cleanly out of the respawn loop. */
            if (editor_eof && !worker_alive) {
                close(worker_out);
                return 0;
            }
        }

        /* Worker died. Close pipes before next iteration. */
        if (worker_in >= 0) close(worker_in);
        close(worker_out);

        if (editor_eof) {
            /* Editor already gone -- no reason to respawn. */
            return 0;
        }

        /* Inject synthetic window/showMessage. */
        char frame[1280];
        size_t n = build_showmessage(frame, sizeof(frame), NULL);
        if (n > 0) {
            ssize_t ignored = write(editor_out, frame, n);
            (void)ignored;
        }

        /* Bailout check. */
        int64_t now = (int64_t)time(NULL);
        if (record_crash(now)) {
            fprintf(stderr,
                    "ironls supervisor: bailout: %u crashes in %d seconds\n",
                    ILSP_SUPERVISOR_BAILOUT_N, ILSP_SUPERVISOR_BAILOUT_WIN);
            return 1;
        }

        /* Backoff sleep. */
        int d = backoff_secs_for(consecutive_crashes++);
        sleep((unsigned)d);
    }
    return 0;
}

/* ── Test surface ────────────────────────────────────────────────────── */

#ifdef ILSP_SUPERVISOR_TESTING
ssize_t ilsp_supervisor_forward_bytes_for_test(int in_fd, int out_fd,
                                                size_t n) {
    return forward_bytes(in_fd, out_fd, n);
}
#endif  /* ILSP_SUPERVISOR_TESTING */

#else /* _WIN32 */

/* ── Windows supervisor ───────────────────────────────────────────────
 *
 * The same contract with Win32 primitives: the worker is this executable
 * started again with CreateProcess (`--supervised` replaced by
 * `--__worker`) on two anonymous pipes. Anonymous pipes cannot be polled,
 * so a thread pumps the editor's stdin into the current worker's stdin
 * while the main thread pumps the worker's stdout to the editor and then
 * waits for the process. The editor's EOF is handed over by closing the
 * worker's stdin; a dead worker gets the showMessage frame, the backoff
 * and a respawn, with the same 5-in-60s bailout. */

static CRITICAL_SECTION s_in_lock;        /* guards s_worker_in */
static HANDLE s_worker_in = NULL;          /* current worker's stdin (write end) */
static volatile LONG s_editor_eof = 0;

static bool write_all(HANDLE h, const char *buf, size_t n) {
    while (n > 0) {
        DWORD w = 0;
        if (!WriteFile(h, buf, (DWORD)n, &w, NULL)) return false;
        buf += w; n -= w;
    }
    return true;
}

static DWORD WINAPI editor_pump(LPVOID arg) {
    (void)arg;
    HANDLE editor_in = GetStdHandle(STD_INPUT_HANDLE);
    char buf[ILSP_SUPERVISOR_PIPE_BUF];
    for (;;) {
        DWORD r = 0;
        if (!ReadFile(editor_in, buf, sizeof(buf), &r, NULL) || r == 0) break;
        EnterCriticalSection(&s_in_lock);
        HANDLE w = s_worker_in;
        bool ok = w ? write_all(w, buf, r) : true;
        LeaveCriticalSection(&s_in_lock);
        (void)ok;  /* a dead worker loses the bytes it could not take; the editor retries after the restart */
    }
    InterlockedExchange(&s_editor_eof, 1);
    EnterCriticalSection(&s_in_lock);
    if (s_worker_in) { CloseHandle(s_worker_in); s_worker_in = NULL; }
    LeaveCriticalSection(&s_in_lock);
    return 0;
}

/* Quote one argument for CreateProcess's command line (CommandLineToArgvW rules). */
static void append_quoted(char *dst, size_t cap, size_t *len, const char *arg) {
    if (*len + 1 < cap) dst[(*len)++] = '"';
    size_t bs = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == '\\') { bs++; continue; }
        if (*p == '"') { for (size_t i = 0; i < bs * 2 + 1 && *len + 1 < cap; i++) dst[(*len)++] = '\\'; bs = 0; }
        else { for (size_t i = 0; i < bs && *len + 1 < cap; i++) dst[(*len)++] = '\\'; bs = 0; }
        if (*len + 1 < cap) dst[(*len)++] = *p;
    }
    for (size_t i = 0; i < bs * 2 && *len + 1 < cap; i++) dst[(*len)++] = '\\';
    if (*len + 1 < cap) dst[(*len)++] = '"';
    dst[*len] = '\0';
}

int ilsp_supervisor_run(int argc, char **argv) {
    InitializeCriticalSection(&s_in_lock);
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    HANDLE editor_out = GetStdHandle(STD_OUTPUT_HANDLE);

    char exe[MAX_PATH];
    DWORD el = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (el == 0 || el >= sizeof(exe)) {
        fprintf(stderr, "ironls supervisor: cannot locate the executable\n");
        return 1;
    }
    char cmd[32768];
    size_t cl = 0;
    cmd[0] = '\0';
    append_quoted(cmd, sizeof(cmd), &cl, exe);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--supervised") == 0) continue;
        if (cl + 1 < sizeof(cmd)) cmd[cl++] = ' ';
        append_quoted(cmd, sizeof(cmd), &cl, argv[i]);
    }
    if (cl + 1 < sizeof(cmd)) cmd[cl++] = ' ';
    append_quoted(cmd, sizeof(cmd), &cl, "--__worker");

    HANDLE pump = CreateThread(NULL, 0, editor_pump, NULL, 0, NULL);
    if (!pump) {
        fprintf(stderr, "ironls supervisor: cannot start the stdin pump\n");
        return 1;
    }

    unsigned consecutive_crashes = 0;
    s_crash_window_count = 0;
    memset(s_crash_window, 0, sizeof(s_crash_window));

    while (1) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        HANDLE e2w_r = NULL, e2w_w = NULL, w2e_r = NULL, w2e_w = NULL;
        if (!CreatePipe(&e2w_r, &e2w_w, &sa, 0) || !CreatePipe(&w2e_r, &w2e_w, &sa, 0)) {
            fprintf(stderr, "ironls supervisor: CreatePipe failed (%lu)\n", GetLastError());
            return 1;
        }
        /* The parent keeps its ends private so the worker's exit closes
         * the pipe (otherwise the worker-out read never sees EOF). */
        SetHandleInformation(e2w_w, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(w2e_r, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = e2w_r;
        si.hStdOutput = w2e_w;
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        char cmd_copy[32768];
        memcpy(cmd_copy, cmd, cl + 1);
        if (!CreateProcessA(NULL, cmd_copy, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "ironls supervisor: CreateProcess failed (%lu)\n", GetLastError());
            return 1;
        }
        CloseHandle(pi.hThread);
        CloseHandle(e2w_r);
        CloseHandle(w2e_w);

        EnterCriticalSection(&s_in_lock);
        if (s_editor_eof) { CloseHandle(e2w_w); e2w_w = NULL; }
        s_worker_in = e2w_w;
        LeaveCriticalSection(&s_in_lock);

        /* Pump the worker's output until it closes its stdout (it exited). */
        char buf[ILSP_SUPERVISOR_PIPE_BUF];
        for (;;) {
            DWORD r = 0;
            if (!ReadFile(w2e_r, buf, sizeof(buf), &r, NULL) || r == 0) break;
            if (!write_all(editor_out, buf, r)) break;
        }
        CloseHandle(w2e_r);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);

        EnterCriticalSection(&s_in_lock);
        if (s_worker_in) { CloseHandle(s_worker_in); s_worker_in = NULL; }
        bool eof = s_editor_eof != 0;
        LeaveCriticalSection(&s_in_lock);
        if (eof) return 0;

        char frame[1280];
        size_t n = build_showmessage(frame, sizeof(frame), NULL);
        if (n > 0) (void)write_all(editor_out, frame, n);

        int64_t now = (int64_t)time(NULL);
        if (record_crash(now)) {
            fprintf(stderr, "ironls supervisor: %u crashes in %d seconds, giving up\n",
                    ILSP_SUPERVISOR_BAILOUT_N, ILSP_SUPERVISOR_BAILOUT_WIN);
            return 1;
        }
        Sleep((DWORD)backoff_secs_for(consecutive_crashes) * 1000);
        consecutive_crashes++;
    }
}

#endif /* _WIN32 */

#ifdef ILSP_SUPERVISOR_TESTING

int ilsp_supervisor_backoff_secs_for_test(unsigned crash_count) {
    return backoff_secs_for(crash_count);
}

int ilsp_supervisor_record_crash_for_test(int64_t ts_secs) {
    return record_crash(ts_secs);
}

void ilsp_supervisor_reset_bailout_for_test(void) {
    s_crash_window_count = 0;
    memset(s_crash_window, 0, sizeof(s_crash_window));
}

size_t ilsp_supervisor_build_showmessage_for_test(char *out, size_t cap,
                                                   const char *message) {
    return build_showmessage(out, cap, message);
}

#endif  /* ILSP_SUPERVISOR_TESTING */

