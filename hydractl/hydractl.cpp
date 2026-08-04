/* hydractl.cpp  --  Hydra control CLI.
 *
 * A thin client for the hydrad control pipe (\\.\pipe\hydra_control). It sends a
 * one-line command and prints hydrad's reply. All the real work lives in hydrad;
 * this is just a mouth and an ear so you don't need to poke a named pipe by hand.
 *
 *   hydractl status              show IDD + per-process state
 *   hydractl reload              re-read seats.toml and reconcile
 *   hydractl restart <seat>      restart one seat's mirror + agent (e.g. B)
 *   hydractl restart all         restart every supervised helper
 *   hydractl learn               run seat_router --learn on the console to read
 *                                keyboard/mouse device numbers
 *   hydractl stop                ask the service to stop supervising
 *
 * BUILD (x64 Native Tools): cl /O2 /EHsc hydractl.cpp
 *   Pure Win32; could also be built with MinGW. Trivial enough that it isn't
 *   separately unit-tested — its whole contract is "write line, read reply".
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <string>

#include "../common/hydra_ipc.h"   /* HYDRA_CONTROL_PIPE */

static int usage()
{
    wprintf(L"usage: hydractl <command>\n"
            L"  status            show virtual monitors and helper processes\n"
            L"  reload            re-read seats.toml and apply changes\n"
            L"  restart <seat>    restart one seat's mirror + agent (e.g. B)\n"
            L"  restart all       restart all supervised helpers\n"
            L"  learn             read input device numbers on the console\n"
            L"  stop              stop the supervisor\n");
    return 2;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return usage();

    /* Reassemble the args into one command line for the daemon. */
    std::wstring cmd;
    for (int i = 1; i < argc; ++i) { if (i > 1) cmd += L" "; cmd += argv[i]; }

    /* Wait briefly for the pipe in case the service is mid-start. */
    if (!WaitNamedPipeW(HYDRA_CONTROL_PIPE, 3000)) {
        fwprintf(stderr, L"hydrad not reachable on %s\n"
                         L"(is the Hydra service running? try: hydrad run)\n",
                 HYDRA_CONTROL_PIPE);
        return 1;
    }

    HANDLE pipe = CreateFileW(HYDRA_CONTROL_PIPE, GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"could not open control pipe (err %lu)\n", GetLastError());
        return 1;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    DWORD wr = 0;
    WriteFile(pipe, cmd.c_str(), (DWORD)(cmd.size() * sizeof(wchar_t)), &wr, nullptr);

    /* Read the (possibly multi-message) reply until the pipe drains. */
    for (;;) {
        wchar_t buf[2048]; DWORD rd = 0;
        BOOL ok = ReadFile(pipe, buf, sizeof(buf) - sizeof(wchar_t), &rd, nullptr);
        if (ok && rd) {
            buf[rd / sizeof(wchar_t)] = 0;
            fwprintf(stdout, L"%s", buf);
        }
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_MORE_DATA) continue;    /* message longer than buf */
            break;                                  /* ERROR_BROKEN_PIPE = done */
        }
        if (rd == 0) break;
    }

    CloseHandle(pipe);
    return 0;
}
