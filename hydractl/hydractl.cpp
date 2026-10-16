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
#include <fstream>
#include <filesystem>
#include <iostream>
#include "../hydrad/hydra_config_json.h"

#include "../common/hydra_ipc.h"   /* HYDRA_CONTROL_PIPE */

static int usage()
{
    wprintf(L"usage: hydractl <command>\n"
            L"  status            show virtual monitors and helper processes\n"
            L"  config <path>     validate a config offline and print its seat map (JSON)\n"
            L"  live-config       print the daemon's applied seat map (JSON)\n"
            L"  reload            re-read seats.toml and apply changes\n"
            L"  restart <seat>    restart one seat's mirror + agent (e.g. B)\n"
            L"  restart all       restart all supervised helpers\n"
            L"  learn             read input device numbers on the console\n"
            L"  stop              stop the supervisor\n");
    return 2;
}

/* No pipe, service, driver, registry or session changes on this path. */
static int print_config(const wchar_t* path)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file) { std::cerr << "cannot open config\n"; return 1; }
    const auto size = file.tellg();
    if (size < 0 || size > 1024 * 1024) {
        std::cerr << "config must be at most 1 MiB\n"; return 2;
    }
    std::string text((size_t)size, '\0');
    file.seekg(0);
    if (!text.empty() && !file.read(&text[0], (std::streamsize)text.size())) {
        std::cerr << "cannot read complete config\n"; return 1;
    }
    HydraCfg cfg; std::string error;
    if (!hydra_parse_config(text, cfg, error)) { std::cerr << error << '\n'; return 2; }
    std::cout << hydra_config_json(cfg) << '\n';
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) return usage();
    if (wcscmp(argv[1], L"config") == 0) {
        if (argc != 3) return usage();
        return print_config(argv[2]);
    }

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
        /* ERROR_MORE_DATA still returns the first part of the message. A live
         * multi-seat config can exceed this buffer; never discard that part. */
        if (rd) {
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
