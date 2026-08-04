/* respawn.c  --  dirt-simple process supervisor for the multiseat components
 *
 * "Autorestart on stall/error" has two layers. The router, agent and clamp
 * each heal their OWN internal faults (dropped sockets, driver hiccups,
 * wedged connections, dead hooks) without dying, and their hang watchdogs
 * convert an undetectable wedge into a detectable death. This handles the
 * other layer: the process going away -- crash, kill, or deliberate
 * self-termination by a hang watchdog. It launches one child, waits, and
 * relaunches it when it exits -- with capped exponential backoff so a child
 * that dies instantly and forever doesn't spin the CPU.
 *
 * v2 -> v3: EXIT-CODE CONTRACT. v2 stopped only on exit code 0, which meant
 * a configuration error (bad monitor index, malformed router args) restarted
 * forever at max backoff -- restarting can't fix a wrong command line. The
 * contract is now:
 *     0  deliberate stop (Ctrl-C, informational run)   -> do not restart
 *     2  configuration error                           -> do not restart
 *   else  fault (crash, hang-watchdog self-kill, ...)  -> restart
 * The components return 2 for bad arguments accordingly.
 *
 * It is deliberately generic: run one per component, in that component's
 * session. This is what you point the Startup folder / logon Scheduled Task
 * at, instead of the component directly.
 *
 *   Console session (seat A):
 *     respawn.exe seat_router.exe 2 12 56789
 *     respawn.exe clip_console.exe 0
 *   Each extra seat's session:
 *     respawn.exe seatB_agent.exe 127.0.0.1 56789
 *
 * Everything after argv[1] is the child's command line, passed through.
 * Ctrl-C stops the supervisor; the console delivers it to the child too, and
 * the child's resulting exit (code 0 from a handled Ctrl-C, or termination)
 * is not restarted because the stop flag is already set.
 *
 * Build: cl /O2 respawn.c
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")

#define BACKOFF_MIN_MS  500
#define BACKOFF_MAX_MS  15000
/* A run that lasted at least this long is "healthy" -> reset backoff, so a
 * process that ran fine for an hour and then died restarts promptly rather
 * than inheriting a long-crash penalty. */
#define HEALTHY_RUN_MS  10000

#define EXIT_CONFIG 2

static volatile LONG   g_stop  = 0;
static volatile HANDLE g_child = NULL;

static BOOL WINAPI on_ctrl(DWORD type) {
    (void)type;
    InterlockedExchange(&g_stop, 1);
    HANDLE h = g_child;
    if (h) TerminateProcess(h, 1);   /* take the child down with us */
    return TRUE;                     /* handled: don't run default handler */
}

/* Rebuild a command line from argv[first..], quoting any argument that
 * contains whitespace. Good enough for these fixed, simple invocations. */
static void build_cmdline(int argc, char **argv, int first,
                          char *out, size_t cap) {
    size_t pos = 0;
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        int needq = (*a == '\0') || strpbrk(a, " \t") != NULL;
        const char *piece_pre  = needq ? "\"" : "";
        const char *piece_post = needq ? "\"" : "";
        int wrote = _snprintf(out + pos, cap - pos, "%s%s%s%s",
                              (i > first) ? " " : "",
                              piece_pre, a, piece_post);
        if (wrote < 0 || (size_t)wrote >= cap - pos) {
            fprintf(stderr, "[respawn] command line too long\n");
            out[cap - 1] = '\0';
            return;
        }
        pos += (size_t)wrote;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <child.exe> [args...]\n"
            "  supervises <child.exe>, restarting it on faults with capped\n"
            "  backoff. Exit 0 (deliberate stop) and exit 2 (config error)\n"
            "  are NOT restarted; everything else is.\n",
            argv[0]);
        return EXIT_CONFIG;
    }

    char cmdline[2048];
    build_cmdline(argc, argv, 1, cmdline, sizeof(cmdline));

    SetConsoleCtrlHandler(on_ctrl, TRUE);

    DWORD backoff = BACKOFF_MIN_MS;

    while (!g_stop) {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        /* CreateProcess may write to the command-line buffer, so hand it a
         * fresh mutable copy each spawn. */
        char mutable_cmd[2048];
        lstrcpynA(mutable_cmd, cmdline, sizeof(mutable_cmd));

        fprintf(stderr, "[respawn] starting: %s\n", cmdline);
        ULONGLONG t0 = GetTickCount64();

        if (!CreateProcessA(NULL, mutable_cmd, NULL, NULL, FALSE,
                            0, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "[respawn] CreateProcess failed (err %lu); "
                            "retrying in %lu ms\n", GetLastError(), backoff);
            Sleep(backoff);
            backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
            continue;
        }

        g_child = pi.hProcess;
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        ULONGLONG ran = GetTickCount64() - t0;

        g_child = NULL;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (g_stop) break;

        if (code == 0 || code == EXIT_CONFIG) {
            fprintf(stderr, "[respawn] child exited %lu (%s); not restarting\n",
                    code, code == 0 ? "deliberate stop" : "config error");
            break;
        }

        if (ran >= HEALTHY_RUN_MS) backoff = BACKOFF_MIN_MS;  /* was healthy */

        fprintf(stderr, "[respawn] child exited (code %lu) after %lu ms; "
                        "restarting in %lu ms\n",
                code, (unsigned long)ran, backoff);
        Sleep(backoff);
        backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
    }

    fprintf(stderr, "[respawn] stopped\n");
    return 0;
}
