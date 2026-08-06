/* hydrad.cpp  --  Hydra control plane (the "one product" service).
 *
 * A single LocalSystem service that makes reality match seats.toml:
 *   1. Instantiates one iddseat virtual-display device per extra seat (SwDevice),
 *      pushing the seat name + mode as device properties the driver reads back.
 *   2. Launches and supervises every helper process, in the right session:
 *        - console session : seat_router.exe (all seats) , clip_console.exe ,
 *                            one mirror.exe per seat
 *        - each seat session: seatB_agent.exe
 *      Supervision absorbs respawn.exe's contract: capped exponential backoff,
 *      exit 0 (deliberate) and exit 2 (config) stay down, everything else comes
 *      back. Sessions that aren't up yet are retried, so agents start whenever
 *      their TS session logs in.
 *   3. Serves a control pipe for hydractl: status / restart / reload / stop / learn.
 *
 * WHY HELPERS ARE PROCESSES, NOT IN-PROC MODULES
 *   clip_console installs a WH_MOUSE_LL hook and seat_router pumps interactive
 *   input; both must live in the *console* session, not in a session-0 service.
 *   Session-0 isolation makes an in-proc "module" incorrect. So hydrad stays the
 *   single supervisor + device manager, and launches the proven v3 binaries into
 *   the sessions where they actually work. One config, one service, one status
 *   view — the "one product" goal — with the OS's session boundary respected.
 *
 * BUILD: MSVC (x64). Links swdevice/cfgmgr32, wtsapi32, userenv, advapi32.
 *   Not built in the Linux dev container (Windows SDK only). The pure-logic
 *   pieces (config parse, router-arg build, session-spec parse) are unit-tested
 *   natively; see tests/.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <swdevice.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <shlobj.h>

/* MSVC does not pull these in through a WIN32_LEAN_AND_MEAN <windows.h> the way
 * MinGW does. wprintf/_vsnwprintf_s/_TRUNCATE (stdio), va_list/va_start (stdarg),
 * _countof/strtoul (stdlib), _wcsicmp (string). */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

#include "hydra_config.h"
#include "../common/hydra_devprops.h"
#include "../common/hydra_ipc.h"

/* SwDeviceCreate/SwDeviceClose live in cfgmgr32.dll but are NOT exported by
 * cfgmgr32.lib -- the import lib that carries them is onecore.lib. (Chromium's
 * virtual-display controller links the same way.) Verified empirically: linking
 * cfgmgr32.lib yields LNK2019 on both symbols. */
#pragma comment(lib, "onecore.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

#define SVC_NAME       L"Hydra"
#define BACKOFF_MIN_MS 500u
#define BACKOFF_MAX_MS 15000u
#define HEALTHY_MS     10000u
#define EXIT_CONFIG    2u
#define HW_ID          L"Root\\HydraSeat"

/* ===================================================================== *
 * Logging (service log file + optional console)
 * ===================================================================== */
static std::wstring g_logDir;    /* C:\ProgramData\Hydra\logs */
static bool         g_console = false;
static std::mutex   g_logMx;

static std::wstring data_dir()
{
    PWSTR p = nullptr;
    std::wstring base = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p)) && p)
        base = p;
    if (p) CoTaskMemFree(p);
    return base + L"\\Hydra";
}

static void ensure_dirs()
{
    std::wstring d = data_dir();
    CreateDirectoryW(d.c_str(), nullptr);
    g_logDir = d + L"\\logs";
    CreateDirectoryW(g_logDir.c_str(), nullptr);
}

static void hlog(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[1200];
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%02d:%02d:%02d %s\r\n",
                 st.wHour, st.wMinute, st.wSecond, buf);

    std::lock_guard<std::mutex> lk(g_logMx);
    if (g_console) { fwprintf(stderr, L"%s", line); }
    if (!g_logDir.empty()) {
        std::wstring path = g_logDir + L"\\hydrad.log";
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr; WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &wr, nullptr);
            CloseHandle(h);
        }
    }
}

/* ===================================================================== *
 * Paths
 * ===================================================================== */
static std::wstring exe_dir()
{
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : s.substr(0, slash);
}

static std::wstring helper_path(const wchar_t* name) { return exe_dir() + L"\\" + name; }

static std::wstring config_path()
{
    /* Prefer a seats.toml next to the exe; fall back to ProgramData\Hydra. */
    std::wstring local = exe_dir() + L"\\seats.toml";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
    return data_dir() + L"\\seats.toml";
}

static bool read_file(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = out.empty() ? TRUE : ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    return ok == TRUE;
}

static std::wstring widen(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

/* ===================================================================== *
 * Session resolution  ("console" | "auto" | "<id>" | "user:NAME")
 * ===================================================================== */
static DWORD console_session() { return WTSGetActiveConsoleSessionId(); }

static bool session_username(DWORD id, std::wstring& outUser)
{
    LPWSTR p = nullptr; DWORD n = 0; bool ok = false;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, WTSUserName, &p, &n) && p) {
        outUser = p; ok = p[0] != 0;
    }
    if (p) WTSFreeMemory(p);
    return ok;
}

/* Returns a session id or 0xFFFFFFFF if not currently resolvable. */
static DWORD resolve_session(const std::string& spec)
{
    if (spec == "console" || spec.empty()) {
        DWORD c = console_session();
        return c;   /* may be 0xFFFFFFFF if no console user yet */
    }
    if (spec.rfind("user:", 0) == 0) {
        std::wstring want = widen(spec.substr(5));
        WTS_SESSION_INFOW* si = nullptr; DWORD cnt = 0; DWORD found = 0xFFFFFFFF;
        if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &si, &cnt)) {
            for (DWORD i = 0; i < cnt; ++i) {
                std::wstring u;
                if (session_username(si[i].SessionId, u) && _wcsicmp(u.c_str(), want.c_str()) == 0) {
                    found = si[i].SessionId; break;
                }
            }
            WTSFreeMemory(si);
        }
        return found;
    }
    if (spec == "auto") {
        /* First Active session that is not the console. Falls back to console. */
        DWORD con = console_session();
        WTS_SESSION_INFOW* si = nullptr; DWORD cnt = 0; DWORD pick = 0xFFFFFFFF;
        if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &si, &cnt)) {
            for (DWORD i = 0; i < cnt; ++i) {
                if (si[i].State == WTSActive && si[i].SessionId != con && si[i].SessionId != 0) {
                    std::wstring u;
                    if (session_username(si[i].SessionId, u)) { pick = si[i].SessionId; break; }
                }
            }
            WTSFreeMemory(si);
        }
        return (pick != 0xFFFFFFFF) ? pick : con;
    }
    /* numeric id */
    return (DWORD)strtoul(spec.c_str(), nullptr, 10);
}

/* ===================================================================== *
 * Launch a process into a session, logging its stdout/stderr to a file.
 * ===================================================================== */
static HANDLE open_child_log(const std::wstring& tag)
{
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    /* Tags like "mirror:B" contain a colon; a colon in a Windows path opens an
     * NTFS alternate data stream instead of a normal file (so the log silently
     * vanishes into a hidden stream). Replace any filename-illegal chars. */
    std::wstring safe = tag;
    for (wchar_t& c : safe)
        if (c == L':' || c == L'\\' || c == L'/' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|') c = L'_';
    std::wstring path = g_logDir + L"\\" + safe + L".log";
    return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

/* Pre-create the Global\HydraSeat_<seat>_meta section on behalf of
 * session_capture.
 *
 * session_capture runs as the interactive USER in the seat's session. Creating
 * anything in the Global\ namespace requires SeCreateGlobalPrivilege, which a
 * user token lacks -- so its CreateFileMapping failed with ERROR_ACCESS_DENIED
 * and the whole capture path died one step after Desktop Duplication had
 * already succeeded. hydrad is SYSTEM in session 0 and does hold the privilege,
 * so we create the section here and let the capture agent simply open it.
 *
 * The section must outlive the agent (it restarts), so handles are held for the
 * service's lifetime and never closed.
 *
 * NOTE ON THE DACL: a NULL DACL grants everyone access. That is deliberate for
 * this box -- the consumer (mirror) runs in the console session while the
 * producer runs in the seat session, i.e. two different users, and the payload
 * is a few bytes of surface metadata (dimensions + a ready flag), not secrets.
 * If Hydra ever runs somewhere untrusted, replace this with an explicit ACE for
 * the seat user and the console user rather than a NULL DACL. */
static void ensure_meta_section(const std::string& seat)
{
    static std::map<std::string, HANDLE> held;
    static std::mutex mx;
    std::lock_guard<std::mutex> lk(mx);
    if (held.count(seat)) return;

    SECURITY_DESCRIPTOR sd{};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);   /* NULL DACL = all access */
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    wchar_t name[128];
    hydra_meta_name(name, 128, seat.c_str());
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                  0, sizeof(HydraSeatMeta), name);
    if (!h) {
        hlog(L"[capture:%S] pre-create meta section failed err=%lu", seat.c_str(), GetLastError());
        return;
    }
    /* Zero it so a stale ready=1 from a previous run can't fool mirror into
     * opening a surface that no longer exists. */
    if (void* v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HydraSeatMeta))) {
        memset(v, 0, sizeof(HydraSeatMeta));
        UnmapViewOfFile(v);
    }
    held[seat] = h;     /* intentionally never closed */

    /* Same privilege problem, same solution: the pixel transport lives in the
     * Global\ namespace and the capture agent (interactive user) cannot create
     * it either. Allocate the maximum frame size once; it is pagefile-backed, so
     * untouched pages cost nothing. */
    wchar_t pname[128];
    hydra_pixels_name(pname, 128, seat.c_str());
    HANDLE ph = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                   (DWORD)(((uint64_t)HYDRA_PIX_TOTAL) >> 32),
                                   (DWORD)(((uint64_t)HYDRA_PIX_TOTAL) & 0xFFFFFFFFull),
                                   pname);
    if (!ph) {
        hlog(L"[capture:%S] pre-create pixel section failed err=%lu", seat.c_str(), GetLastError());
    } else {
        if (void* pv = MapViewOfFile(ph, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HydraSeatPixels))) {
            memset(pv, 0, sizeof(HydraSeatPixels));
            UnmapViewOfFile(pv);
        }
        held[seat + ":pix"] = ph;   /* also never closed */
    }

    /* Audio ring, same reasoning: the capture agent runs as the seat user and
     * cannot create a Global\ object. See hydra_ipc.h for why audio needs its
     * own transport at all (RDP's audio channel buffers far more than DDA
     * capture does, so the two paths drift apart). */
    wchar_t aname[128];
    hydra_audio_name(aname, 128, seat.c_str());
    HANDLE ah = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                   (DWORD)(((uint64_t)HYDRA_AUD_TOTAL) >> 32),
                                   (DWORD)(((uint64_t)HYDRA_AUD_TOTAL) & 0xFFFFFFFFull),
                                   aname);
    if (!ah) {
        hlog(L"[audio:%S] pre-create audio ring failed err=%lu", seat.c_str(), GetLastError());
    } else {
        if (void* av = MapViewOfFile(ah, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HydraAudioRing))) {
            memset(av, 0, sizeof(HydraAudioRing));
            UnmapViewOfFile(av);
        }
        held[seat + ":aud"] = ah;
    }
    hlog(L"[capture:%S] meta + pixel sections ready (created by service)", seat.c_str());
}

/* Launch a helper as a DIRECT CHILD of hydrad -- i.e. in hydrad's own context,
 * which is SYSTEM / session 0. This is REQUIRED for session_route: we proved
 * process-loopback audio capture reaches across the TS session boundary only
 * from a session-0 context (the audiotest 0.987 run was session-0). Launching it
 * into a user session via the token path would defeat that. No token dance, no
 * environment block -- just CreateProcessW inheriting our SYSTEM session-0 token. */
static HANDLE launch_in_session0(const std::wstring& tag, const std::wstring& exe,
                                 const std::wstring& args)
{
    HANDLE childLog = open_child_log(tag);
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty()) cmd += L" " + args;
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back(0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    if (childLog != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = childLog; si.hStdError = childLog;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::wstring cwd = exe_dir();
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmdbuf.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    if (childLog != INVALID_HANDLE_VALUE) CloseHandle(childLog);
    if (!ok) {
        hlog(L"[%s] session0 CreateProcess failed err=%lu", tag.c_str(), GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    hlog(L"[%s] launched pid=%lu in session 0 (SYSTEM)", tag.c_str(), pi.dwProcessId);
    return pi.hProcess;
}

/* Launch a helper as SYSTEM, but INSIDE a given terminal-services session.
 *
 * WHY THIS EXISTS (the seatB_agent "err 5" saga)
 *   The normal path below runs helpers with the interactive USER'S token. For
 *   the input agent that is a permanent liability: SendInput delivers to the
 *   calling thread's desktop, and a medium-integrity user token cannot attach to
 *   -- or inject into -- any desktop the user doesn't own. So seat B's keyboard
 *   and mouse die, silently and completely, whenever teacher's session shows:
 *       - the LOCK SCREEN or a screensaver (Winlogon secure desktop)
 *       - a UAC / consent prompt
 *       - any transient desktop switch
 *   with every SendInput returning ERROR_ACCESS_DENIED (5) and no way back
 *   except restarting the service by hand.
 *
 *   SYSTEM has SE_TCB_PRIVILEGE and can attach to ANY desktop in the session,
 *   Winlogon included. Running the agent that way makes seat B work through lock
 *   screens and UAC prompts -- you can even log teacher in from seat B, which
 *   removes the last manual step from startup.
 *
 * HOW
 *   Duplicate hydrad's own (SYSTEM) token, stamp TokenSessionId to the target
 *   session -- which requires SE_TCB_PRIVILEGE, hence SYSTEM -- and launch. The
 *   process is then SYSTEM in that session, on winsta0\default.
 *
 * SCOPE: used for the INPUT AGENT only. Capture and mirror deliberately keep the
 * user token; they need the user's own graphics context, and running them as
 * SYSTEM would break more than it fixed.
 */
static HANDLE launch_in_session_as_system(DWORD session, const std::wstring& tag,
                                          const std::wstring& exe, const std::wstring& args)
{
    HANDLE self = nullptr, primary = nullptr, proc = nullptr, childLog = INVALID_HANDLE_VALUE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &self)) {
        hlog(L"[%s] OpenProcessToken failed err=%lu", tag.c_str(), GetLastError());
        return nullptr;
    }
    if (!DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
                          TokenPrimary, &primary)) {
        hlog(L"[%s] DuplicateTokenEx failed err=%lu", tag.c_str(), GetLastError());
        CloseHandle(self);
        return nullptr;
    }
    CloseHandle(self);

    DWORD sid = session;
    if (!SetTokenInformation(primary, TokenSessionId, &sid, sizeof(sid))) {
        /* Needs SE_TCB_PRIVILEGE. If hydrad somehow isn't SYSTEM this fails, and
         * the caller falls back to the user-token path. */
        hlog(L"[%s] SetTokenInformation(session %lu) failed err=%lu -- not SYSTEM?",
             tag.c_str(), session, GetLastError());
        CloseHandle(primary);
        return nullptr;
    }

    childLog = open_child_log(tag);

    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty()) cmd += L" " + args;
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back(0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    if (childLog != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = childLog; si.hStdError = childLog;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }

    std::wstring cwd = exe_dir();
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessAsUserW(primary, exe.c_str(), cmdbuf.data(), nullptr, nullptr,
                                   TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                   nullptr, cwd.c_str(), &si, &pi);
    if (ok) {
        proc = pi.hProcess;
        CloseHandle(pi.hThread);
        hlog(L"[%s] launched pid=%lu in session %lu as SYSTEM", tag.c_str(), pi.dwProcessId, session);
    } else {
        hlog(L"[%s] CreateProcessAsUser(SYSTEM) failed err=%lu", tag.c_str(), GetLastError());
    }

    if (childLog != INVALID_HANDLE_VALUE) CloseHandle(childLog);
    CloseHandle(primary);
    return proc;
}

static HANDLE launch_in_session(DWORD session, const std::wstring& tag,
                                const std::wstring& exe, const std::wstring& args)
{
    HANDLE userTok = nullptr, primary = nullptr, proc = nullptr, childLog = INVALID_HANDLE_VALUE;
    void* env = nullptr;

    if (!WTSQueryUserToken(session, &userTok)) {
        hlog(L"[%s] WTSQueryUserToken(session %lu) failed err=%lu", tag.c_str(),
             session, GetLastError());
        return nullptr;
    }
    if (!DuplicateTokenEx(userTok, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
                          TokenPrimary, &primary)) {
        CloseHandle(userTok); return nullptr;
    }
    CreateEnvironmentBlock(&env, primary, FALSE);
    childLog = open_child_log(tag);

    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty()) cmd += L" " + args;
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back(0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    if (childLog != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = childLog; si.hStdError = childLog;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }

    std::wstring cwd = exe_dir();
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessAsUserW(primary, exe.c_str(), cmdbuf.data(), nullptr, nullptr,
                                   TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                   env, cwd.c_str(), &si, &pi);
    if (ok) {
        proc = pi.hProcess;
        CloseHandle(pi.hThread);
        hlog(L"[%s] launched pid=%lu in session %lu", tag.c_str(), pi.dwProcessId, session);
    } else {
        hlog(L"[%s] CreateProcessAsUser failed err=%lu", tag.c_str(), GetLastError());
    }

    if (childLog != INVALID_HANDLE_VALUE) CloseHandle(childLog);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(primary);
    CloseHandle(userTok);
    return proc;
}

/* ===================================================================== *
 * IDD (virtual display) instantiation via SwDevice
 * ===================================================================== */
struct SwWait { HANDLE done; HRESULT hr; };

static VOID WINAPI sw_create_cb(HSWDEVICE, HRESULT hr, PVOID ctx, PCWSTR)
{
    auto* w = (SwWait*)ctx; w->hr = hr; SetEvent(w->done);
}

static HSWDEVICE create_idd(const std::string& seat, const std::string& mode)
{
    std::wstring seatW = widen(seat);
    std::wstring modeW = widen(mode.empty() ? "1920x1080@60" : mode);
    std::wstring instanceId = L"HydraSeat_" + seatW;

    /* multi-sz hardware/compat ids (double-null terminated). */
    static const wchar_t hwids[] = HW_ID L"\0";

    SW_DEVICE_CREATE_INFO ci{};
    ci.cbSize = sizeof(ci);
    ci.pszInstanceId = instanceId.c_str();
    ci.pszzHardwareIds = hwids;
    ci.pszzCompatibleIds = hwids;
    ci.CapabilityFlags = SWDeviceCapabilitiesRemovable
                       | SWDeviceCapabilitiesSilentInstall
                       | SWDeviceCapabilitiesDriverRequired;

    DEVPROPERTY props[2]{};
    props[0].CompKey.Key = DEVPKEY_Hydra_SeatName;
    props[0].CompKey.Store = DEVPROP_STORE_SYSTEM;
    props[0].Type = DEVPROP_TYPE_STRING;
    props[0].Buffer = (PVOID)seatW.c_str();
    props[0].BufferSize = (ULONG)((seatW.size() + 1) * sizeof(wchar_t));

    props[1].CompKey.Key = DEVPKEY_Hydra_SeatMode;
    props[1].CompKey.Store = DEVPROP_STORE_SYSTEM;
    props[1].Type = DEVPROP_TYPE_STRING;
    props[1].Buffer = (PVOID)modeW.c_str();
    props[1].BufferSize = (ULONG)((modeW.size() + 1) * sizeof(wchar_t));

    SwWait wait{ CreateEventW(nullptr, TRUE, FALSE, nullptr), E_FAIL };
    HSWDEVICE h = nullptr;
    HRESULT hr = SwDeviceCreate(SVC_NAME, L"HTREE\\ROOT\\0", &ci, 2, props,
                                sw_create_cb, &wait, &h);
    if (FAILED(hr)) {
        hlog(L"[idd:%s] SwDeviceCreate hr=0x%08lx", seatW.c_str(), hr);
        if (wait.done) CloseHandle(wait.done);
        return nullptr;
    }
    WaitForSingleObject(wait.done, 10000);
    CloseHandle(wait.done);
    if (FAILED(wait.hr)) {
        hlog(L"[idd:%s] create callback hr=0x%08lx (driver installed & test-signed?)",
             seatW.c_str(), wait.hr);
        SwDeviceClose(h);
        return nullptr;
    }
    hlog(L"[idd:%s] virtual monitor created (%s)", seatW.c_str(), modeW.c_str());
    return h;
}

/* ===================================================================== *
 * Supervised process table
 * ===================================================================== */
struct Proc
{
    std::wstring tag;         /* "router" | "clip" | "mirror:B" | "agent:B"      */
    std::wstring exe;
    std::wstring args;
    std::string  sessionSpec; /* "console" for router/clip/mirror; seat's for agent */
    HANDLE       h = nullptr;
    ULONGLONG    startedAt = 0;
    ULONGLONG    nextTryAt = 0;
    DWORD        backoffMs = BACKOFF_MIN_MS;
    bool         dead = false;/* exited 0/2 -> stay down                          */
    DWORD        lastExit = STILL_ACTIVE;
    DWORD        lastSession = 0xFFFFFFFF;
};

struct Plan
{
    std::vector<Proc> procs;
    std::map<std::string, HSWDEVICE> idds;   /* seat name -> device handle       */
};

static Plan               g_plan;
static std::mutex         g_planMx;
static HydraCfg           g_cfg;
static volatile bool      g_stop = false;

/* Compose the desired process list from config (does not launch). */
static std::vector<Proc> plan_procs(const HydraCfg& cfg)
{
    std::vector<Proc> v;

    if (!cfg.seats.empty()) {
        Proc r; r.tag = L"router"; r.exe = helper_path(L"seat_router.exe");
        r.args = hydra_build_router_args(cfg); r.sessionSpec = "console";
        v.push_back(std::move(r));
    }
    if (!cfg.confineMonitor.empty()) {
        Proc c; c.tag = L"clip"; c.exe = helper_path(L"clip_console.exe");
        c.args = widen(cfg.confineMonitor); c.sessionSpec = "console";
        v.push_back(std::move(c));
    }
    for (const SeatCfg& s : cfg.seats) {
        std::wstring seatW = widen(s.name);
        /* display_mode = "off": run NO display producer or presenter for this
         * seat -- no mirror, no capture, nothing touching the panel.
         *
         * This exists because mstsc in FULLSCREEN on the seat's monitor already
         * delivers exactly what the display machinery was built to achieve:
         * teacher's real desktop, edge to edge, no visible chrome, with a working
         * cursor drawn by the RDP client itself. mirror creates its own
         * fullscreen presentation on the same panel and covers that window --
         * so both showed teacher's desktop and the only visible difference was
         * the cursor disappearing the moment Hydra started.
         *
         * With "off", input isolation and audio routing keep working and the
         * panel is left entirely to the RDP client. */
        const bool displayOff = (s.displayMode == "off");

        /* mirror is deliberately NOT launched for "capture" seats.
         *
         * It creates a window and presents a D3D swapchain, which needs an
         * interactive token with foreground-activation rights. A process
         * launched by a service into the console session does not get those --
         * the observed behaviour is that the service-launched mirror produces an
         * EMPTY log and nothing on the panel, while the identical binary run by
         * hand from an interactive shell works perfectly. Rather than fight the
         * launch context, mirror runs as a logon scheduled task in the console
         * session (see install-mirror-task.ps1); hydrad keeps only the genuinely
         * background work -- capture and the audio router. */
        const bool mirrorIsExternal = (s.displayMode == "capture");

        if (!displayOff && !mirrorIsExternal) {
            Proc m; m.tag = L"mirror:" + seatW; m.exe = helper_path(L"mirror.exe");
            m.args = seatW + L" " + widen(s.monitor); m.sessionSpec = "console";
            v.push_back(std::move(m));
        }

        /* Hold the seat's audio endpoint open, permanently.
         *
         * Without this, the endpoint goes idle and the FIRST application to open
         * it afterwards gets silence -- a second app works, and then the first
         * one does too. Browsers lose that race reliably. Restarting Audiosrv
         * does NOT fix it (tried from the console session, from a SYSTEM task in
         * session 0, from a SYSTEM token in the seat's session, and from the
         * console admin's elevated token in the seat's session -- all measured,
         * none work) because a restart leaves the endpoint idle and simply moves
         * the problem to whoever opens it next.
         *
         * Holding a silent stream open means there is never a first opener.
         * Runs with the SEAT USER'S token: this needs the user's own audio
         * session, which is exactly what SYSTEM does not have. */
        /* A/V SYNC: carry audio over shared memory rather than the RDP channel.
         *   abcap:<seat>  in the SEAT'S session -- loopback-records its mix
         *   abren:<seat>  in the CONSOLE session -- plays it to the monitor
         * Enabled by audio_bridge = "<endpoint-substr>" in seats.toml. The RDP
         * client must be muted or the same audio also arrives the slow way. */
        if (!s.audioBridge.empty()) {
            Proc ac; ac.tag = L"abcap:" + seatW;
            ac.exe = helper_path(L"audio_bridge.exe");
            ac.args = L"capture " + seatW;
            ac.sessionSpec = s.session;
            v.push_back(std::move(ac));

            Proc ar; ar.tag = L"abren:" + seatW;
            ar.exe = helper_path(L"audio_bridge.exe");
            ar.args = L"render " + seatW + L" \"" + widen(s.audioBridge) + L"\"";
            ar.sessionSpec = "console";
            v.push_back(std::move(ar));
        }

        if (s.audioPrime == "keepalive") {
            Proc ka; ka.tag = L"keepalive:" + seatW;
            ka.exe = helper_path(L"audio_keepalive.exe");
            ka.sessionSpec = s.session;
            v.push_back(std::move(ka));
        }

        Proc a; a.tag = L"agent:" + seatW; a.exe = helper_path(L"seatB_agent.exe");
        a.args = L"127.0.0.1 " + std::to_wstring(s.port); a.sessionSpec = s.session;
        v.push_back(std::move(a));

        /* GOAL 3 -- "no RDP window": if displayMode == "capture", run
         * session_capture INSIDE the seat's session. It grabs that session's REAL
         * composed desktop (cursor included) via Desktop Duplication and writes it
         * into the SAME shared surface mirror presents -- so teacher's actual
         * desktop appears on the panel with no mstsc window. mirror is unchanged;
         * it just presents whatever fills the surface. Default (empty/"idd") keeps
         * the iddseat virtual-monitor producer. NOTE: DDA must run on the session's
         * interactive input desktop or it fails E_ACCESSDENIED -- this is the
         * untested risk; the log will show which. */
        if (!displayOff && s.displayMode == "capture") {
            /* Must exist BEFORE the agent starts -- it can open but not create it. */
            ensure_meta_section(s.name);
            Proc c; c.tag = L"capture:" + seatW; c.exe = helper_path(L"session_capture.exe");
            c.args = seatW; c.sessionSpec = s.session;
            v.push_back(std::move(c));
        }

        /* Audio isolation: if the seat names a render-endpoint id substring, run
         * session_audio INSIDE the seat's session. It loopback-captures that
         * session's mix and renders it to the named endpoint (e.g. the monitor),
         * addressed explicitly by id -- bypassing the single shared "default"
         * that RDPWrap can't split. Seat 1 (console) keeps its own device
         * untouched. No arg => no audio agent for that seat. */
        if (!s.audioId.empty()) {
            Proc au; au.tag = L"audio:" + seatW; au.exe = helper_path(L"session_audio.exe");
            au.args = L"\"" + widen(s.audioId) + L"\""; au.sessionSpec = s.session;
            v.push_back(std::move(au));
        }

        /* Session-based audio routing (the robust path): session_route runs in
         * SESSION 0 (hydrad's context) -- proven to reach across the TS boundary
         * -- captures ALL of this seat's session audio and renders it to the
         * named endpoint. Pass the seat's session spec (e.g. "user:teacher") and
         * the render-id substring; session_route resolves the session number and
         * the endpoint itself. */
        if (!s.audioRoute.empty()) {
            /* Audio routing via ENDPOINT LOOPBACK (the path that works from
             * session 0 -- process-loopback delivered silence, endpoint loopback
             * carries real audio cross-session, confirmed on hardware). Requires
             * VB-CABLE: teacher's session outputs to CABLE Input, this agent
             * loopback-captures the cable and renders to the monitor.
             *   audioRoute holds "<cable-substr>,<monitor-substr>" (comma-split).
             * Runs in SESSION 0 (hydrad's context). */
            Proc ar; ar.tag = L"aroute:" + seatW; ar.exe = helper_path(L"route_endpoint.exe");
            {
                std::string route = s.audioRoute;
                size_t comma = route.find(',');
                std::string src = (comma==std::string::npos) ? route : route.substr(0,comma);
                std::string dst = (comma==std::string::npos) ? std::string() : route.substr(comma+1);
                ar.args = L"\"" + widen(src) + L"\" \"" + widen(dst) + L"\"";
            }
            ar.sessionSpec = "session0";
            v.push_back(std::move(ar));
        }
    }
    return v;
}

/* One reconcile tick: relaunch what should run, honor backoff + exit contract. */
static void reconcile_once()
{
    std::lock_guard<std::mutex> lk(g_planMx);
    ULONGLONG now = GetTickCount64();

    for (Proc& p : g_plan.procs) {
        if (p.dead) continue;

        if (p.h) {
            DWORD code = STILL_ACTIVE;
            if (GetExitCodeProcess(p.h, &code) && code != STILL_ACTIVE) {
                ULONGLONG ran = now - p.startedAt;
                p.lastExit = code;
                CloseHandle(p.h); p.h = nullptr;

                if (code == 0 || code == EXIT_CONFIG) {
                    p.dead = true;
                    hlog(L"[%s] exited %lu (%s); not restarting", p.tag.c_str(), code,
                         code == 0 ? L"deliberate stop" : L"config error");
                    continue;
                }
                if (ran >= HEALTHY_MS) p.backoffMs = BACKOFF_MIN_MS;
                p.nextTryAt = now + p.backoffMs;
                hlog(L"[%s] exited %lu after %llu ms; retry in %lu ms", p.tag.c_str(),
                     code, ran, p.backoffMs);
                p.backoffMs = (p.backoffMs * 2 > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : p.backoffMs * 2;
            } else {
                continue;   /* still running */
            }
        }

        if (!p.h && now >= p.nextTryAt) {
            /* session_route uses sessionSpec "session0": run in hydrad's own
             * SYSTEM/session-0 context (required for cross-session audio capture),
             * not launched into a user session. */
            if (p.sessionSpec == "session0") {
                p.lastSession = 0;
                p.h = launch_in_session0(p.tag, p.exe, p.args);
                if (p.h) { p.startedAt = now; }
                else {
                    p.nextTryAt = now + p.backoffMs;
                    p.backoffMs = (p.backoffMs * 2 > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : p.backoffMs * 2;
                }
                continue;
            }
            DWORD sess = resolve_session(p.sessionSpec);
            if (sess == 0xFFFFFFFF) {
                p.nextTryAt = now + 1000;   /* session not up yet; poll */
                continue;
            }
            p.lastSession = sess;
            /* The INPUT AGENT runs as SYSTEM inside the session so it can attach
             * to any desktop there -- lock screen, UAC prompt, screensaver --
             * instead of dying with ERROR_ACCESS_DENIED the moment the session
             * shows anything other than the ordinary desktop. Everything else
             * keeps the user token, which is what it needs. Falls back to the
             * user token if the SYSTEM path is unavailable. */
            const bool wantSystem = (p.tag.rfind(L"agent:", 0) == 0);
            p.h = wantSystem ? launch_in_session_as_system(sess, p.tag, p.exe, p.args)
                             : nullptr;
            if (!p.h) p.h = launch_in_session(sess, p.tag, p.exe, p.args);
            if (p.h) { p.startedAt = now; }
            else {
                p.nextTryAt = now + p.backoffMs;
                p.backoffMs = (p.backoffMs * 2 > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : p.backoffMs * 2;
            }
        }
    }
}

static void kill_proc(Proc& p)
{
    if (p.h) {
        TerminateProcess(p.h, 0);
        CloseHandle(p.h);
        p.h = nullptr;
    }
}

/* Apply a freshly parsed config to the running plan (diffing, minimal churn). */
static void apply_config(const HydraCfg& newCfg)
{
    std::lock_guard<std::mutex> lk(g_planMx);

    /* --- IDDs: add new seats, remove gone seats, leave existing. --- */
    std::map<std::string, bool> want;
    for (const SeatCfg& s : newCfg.seats) want[s.name] = true;

    for (auto it = g_plan.idds.begin(); it != g_plan.idds.end(); ) {
        if (!want.count(it->first)) {
            hlog(L"[idd:%s] removing (no longer in config)", widen(it->first).c_str());
            SwDeviceClose(it->second);
            it = g_plan.idds.erase(it);
        } else ++it;
    }
    for (const SeatCfg& s : newCfg.seats) {
        /* Only create the virtual monitor when the seat actually uses it.
         *
         * The display path is now DDA capture + the shared-memory pixel
         * transport, which never touches the IDD. Creating it regardless left a
         * SWD\HYDRA\HYDRASEAT_x device with no driver behind it (once the
         * package is removed, CM_PROB_REINSTALL) and a phantom extra display
         * hanging off the console session that windows could wander onto.
         * Also note: the remote-session IDD experiment is settled -- a
         * REMOTE_SESSION_DRIVER adapter cannot be software-enumerated, and the
         * RDP-Wrapper session never enumerates one either, so there is no path
         * where this device is needed unless display_mode is explicitly "idd". */
        if (s.displayMode != "idd") continue;
        if (!g_plan.idds.count(s.name)) {
            HSWDEVICE h = create_idd(s.name, s.edid);
            if (h) g_plan.idds[s.name] = h;
        }
    }

    /* --- Processes: rebuild the desired list, carry over live handles whose
     *     (tag,args,session) are unchanged; kill removed/changed ones. --- */
    std::vector<Proc> desired = plan_procs(newCfg);
    std::vector<Proc> merged;
    for (Proc& d : desired) {
        bool carried = false;
        for (Proc& old : g_plan.procs) {
            if (old.tag == d.tag && old.args == d.args && old.sessionSpec == d.sessionSpec) {
                merged.push_back(std::move(old));      /* unchanged: keep running */
                old.tag.clear();                       /* mark consumed */
                carried = true;
                break;
            }
        }
        if (!carried) merged.push_back(std::move(d));  /* new/changed: fresh, will launch */
    }
    for (Proc& old : g_plan.procs) {
        if (!old.tag.empty()) {                        /* not carried -> removed/changed */
            hlog(L"[%s] stopping (config change)", old.tag.c_str());
            kill_proc(old);
        }
    }
    g_plan.procs = std::move(merged);
    g_cfg = newCfg;
}

static bool load_and_apply()
{
    std::string text;
    std::wstring path = config_path();
    if (!read_file(path, text)) {
        hlog(L"config not found at %s", path.c_str());
        return false;
    }
    HydraCfg cfg; std::string err;
    if (!hydra_parse_config(text, cfg, err)) {
        hlog(L"config error: %s", widen(err).c_str());
        return false;
    }
    hlog(L"config loaded: %zu seat(s) from %s", cfg.seats.size(), path.c_str());
    apply_config(cfg);
    return true;
}

/* ===================================================================== *
 * Control pipe server (hydractl talks to this)
 * ===================================================================== */
/* "capture:B" -> "B" */
static std::string narrow_tag_seat(const std::wstring& tag)
{
    size_t c = tag.find(L':');
    std::string out;
    if (c != std::wstring::npos)
        for (size_t i = c + 1; i < tag.size(); ++i) out += (char)tag[i];
    return out;
}

/* Read the stall counter the capture agent publishes into shared metadata.
 * Opens read-only each call -- this runs once per status request, not in a loop,
 * and holding a mapping open would outlive the agent it describes. */
static uint32_t read_capture_stall(const std::string& seat)
{
    if (seat.empty()) return 0;
    wchar_t name[128];
    hydra_meta_name(name, 128, seat.c_str());
    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!m) return 0;
    uint32_t v = 0;
    if (void* p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(HydraSeatMeta))) {
        v = ((const HydraSeatMeta*)p)->stalled;
        UnmapViewOfFile(p);
    }
    CloseHandle(m);
    return v;
}

static std::wstring cmd_status()
{
    std::lock_guard<std::mutex> lk(g_planMx);
    std::wstring s = L"Hydra status\r\n";
    s += L"  IDD virtual monitors: " + std::to_wstring(g_plan.idds.size()) + L"\r\n";
    for (auto& kv : g_plan.idds) s += L"    seat " + widen(kv.first) + L": present\r\n";
    s += L"  processes:\r\n";
    ULONGLONG now = GetTickCount64();
    for (Proc& p : g_plan.procs) {
        const wchar_t* st;
        if (p.dead) st = L"stopped (exit contract)";
        else if (p.h) st = L"running";
        else st = L"waiting";
        s += L"    " + p.tag + L": " + st;
        if (p.h && p.startedAt) s += L" (up " + std::to_wstring((now - p.startedAt) / 1000) + L"s)";
        if (!p.h && !p.dead)    s += L" (session " + widen(p.sessionSpec) + L")";

        /* A capture agent can be RUNNING and yet producing nothing: attached to
         * the seat's desktop, but EnumOutputs returns no display, so it retries
         * forever. That state used to be invisible here -- "running" with a quiet
         * log -- and cost an evening of debugging the wrong layer. Read the flag
         * the agent publishes and say so plainly. */
        if (p.h && p.tag.rfind(L"capture:", 0) == 0) {
            std::string seatName = narrow_tag_seat(p.tag);
            uint32_t stalled = read_capture_stall(seatName);
            if (stalled)
                s += L"  *** STALLED: no display in session (retry "
                   + std::to_wstring(stalled) + L") -- reconnect the seat's RDP session";
        }
        s += L"\r\n";
    }
    return s;
}

/* Live A/B switch of a seat's video producer: "display <seat> <idd|capture>".
 * Updates the seat's displayMode and re-applies config, which stops the old
 * producer (capture: or the idd path) and starts the chosen one. Lets you compare
 * the two video methods without editing seats.toml or restarting. */
static std::wstring cmd_display(const std::wstring& arg)
{
    size_t sp = arg.find(L' ');
    if (sp == std::wstring::npos)
        return L"usage: display <seat> <idd|capture>\r\n";
    std::wstring seatW = arg.substr(0, sp);
    std::wstring mode  = arg.substr(sp + 1);
    while (!mode.empty() && (mode.back()==L'\r'||mode.back()==L'\n'||mode.back()==L' ')) mode.pop_back();
    while (!seatW.empty() && seatW.back()==L' ') seatW.pop_back();
    if (mode != L"idd" && mode != L"capture")
        return L"mode must be 'idd' or 'capture'\r\n";

    /* Build a mutated copy of the live config with the new displayMode, then
     * apply_config() diffs it and swaps the producer on the next reconcile. */
    HydraCfg next;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(g_planMx);
        next = g_cfg;
    }
    for (SeatCfg& s : next.seats) {
        if (widen(s.name) == seatW) {
            s.displayMode = (mode == L"capture") ? "capture"
                          : (mode == L"off") ? "off" : "idd";
            found = true;
            break;
        }
    }
    if (!found) return L"no such seat\r\n";

    apply_config(next);   /* takes g_planMx, re-plans, swaps producer */
    return L"display[" + seatW + L"] -> " + mode + L"\r\n";
}

static std::wstring cmd_restart(const std::wstring& seatOrAll)
{
    std::lock_guard<std::mutex> lk(g_planMx);
    int n = 0;
    for (Proc& p : g_plan.procs) {
        bool match = (seatOrAll == L"all") ||
                     (p.tag == L"mirror:" + seatOrAll) ||
                     (p.tag == L"agent:" + seatOrAll) ||
                     (p.tag == L"capture:" + seatOrAll) ||
                     (p.tag == L"audio:" + seatOrAll) ||
                     (p.tag == L"aroute:" + seatOrAll) ||
                     (p.tag == L"keepalive:" + seatOrAll) ||
                     (p.tag == L"abcap:" + seatOrAll) ||
                     (p.tag == L"abren:" + seatOrAll);
        if (match) {
            kill_proc(p);
            p.dead = false; p.backoffMs = BACKOFF_MIN_MS; p.nextTryAt = 0;
            ++n;
        }
    }
    return L"restarted " + std::to_wstring(n) + L" process(es)\r\n";
}

/* Launch as the CONSOLE user's ELEVATED token, stamped into another session.
 *
 * Needed for exactly one job: restarting the audio service inside a seat's
 * session. Getting there required eliminating three other combinations, all
 * measured on hardware:
 *   - console session, any account     -> doesn't fix the seat
 *   - SYSTEM scheduled task (session 0)-> doesn't fix the seat
 *   - SYSTEM token IN the seat session -> launches fine, fixes nothing
 *                                         (SYSTEM has no interactive audio
 *                                          session, so the per-user audio graph
 *                                          that needs rebuilding isn't its own)
 * What DOES work is the console admin's elevated token running in the seat's
 * session -- which is what "run elevated inside the RDP window" actually does,
 * because elevating there prompts for admin credentials and switches account.
 *
 * The seat user's own token is no good either: a standard user can't restart a
 * service at all.
 *
 * So: take the console user's token, ask for its LINKED (elevated) token, stamp
 * TokenSessionId to the target session, and launch. Setting the session id needs
 * SE_TCB_PRIVILEGE, which hydrad has as SYSTEM.
 */
static HANDLE launch_in_session_as_console_admin(DWORD session, const std::wstring& tag,
                                                 const std::wstring& exe, const std::wstring& args)
{
    HANDLE conTok = nullptr, elevated = nullptr, primary = nullptr, proc = nullptr;
    HANDLE childLog = INVALID_HANDLE_VALUE;

    DWORD con = console_session();
    if (con == 0xFFFFFFFF) { hlog(L"[%s] no console session", tag.c_str()); return nullptr; }
    if (!WTSQueryUserToken(con, &conTok)) {
        hlog(L"[%s] WTSQueryUserToken(console %lu) failed err=%lu", tag.c_str(), con, GetLastError());
        return nullptr;
    }

    /* The interactive token is the FILTERED (non-elevated) one when UAC is on;
     * its linked token is the full-privilege admin one. */
    TOKEN_LINKED_TOKEN lt{};
    DWORD cb = 0;
    if (GetTokenInformation(conTok, TokenLinkedToken, &lt, sizeof(lt), &cb) && lt.LinkedToken) {
        elevated = lt.LinkedToken;
    } else {
        /* Already elevated, or UAC off -- use it as-is. */
        elevated = conTok;
    }

    if (!DuplicateTokenEx(elevated, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
                          TokenPrimary, &primary)) {
        hlog(L"[%s] DuplicateTokenEx failed err=%lu", tag.c_str(), GetLastError());
        if (elevated != conTok) CloseHandle(elevated);
        CloseHandle(conTok);
        return nullptr;
    }

    DWORD sid = session;
    if (!SetTokenInformation(primary, TokenSessionId, &sid, sizeof(sid))) {
        hlog(L"[%s] SetTokenInformation(session %lu) failed err=%lu",
             tag.c_str(), session, GetLastError());
        CloseHandle(primary);
        if (elevated != conTok) CloseHandle(elevated);
        CloseHandle(conTok);
        return nullptr;
    }

    childLog = open_child_log(tag);
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty()) cmd += L" " + args;
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back(0);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    if (childLog != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = childLog; si.hStdError = childLog;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }

    std::wstring cwd = exe_dir();
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessAsUserW(primary, exe.c_str(), cmdbuf.data(), nullptr, nullptr,
                                   TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                   nullptr, cwd.c_str(), &si, &pi);
    if (ok) {
        proc = pi.hProcess; CloseHandle(pi.hThread);
        hlog(L"[%s] launched pid=%lu in session %lu as console admin (elevated)",
             tag.c_str(), pi.dwProcessId, session);
    } else {
        hlog(L"[%s] CreateProcessAsUser(console admin) failed err=%lu", tag.c_str(), GetLastError());
    }

    if (childLog != INVALID_HANDLE_VALUE) CloseHandle(childLog);
    CloseHandle(primary);
    if (elevated != conTok) CloseHandle(elevated);
    CloseHandle(conTok);
    return proc;
}

/* Restart the Windows audio service INSIDE a seat's session.
 *
 * WHY IT HAS TO BE DONE THERE
 *   The Remote Audio endpoint in the seat's RDP session goes bad when idle: the
 *   FIRST application to open it gets silence, while a second app opened
 *   afterwards works -- and then the first one works too. Browsers lose that race
 *   reliably. Restarting the audio service clears it.
 *
 *   Audiosrv is machine-wide, so you would think restarting it from anywhere
 *   works. Measured, it does not: a restart issued from the CONSOLE session
 *   doesn't fix the seat, and one issued from a SYSTEM scheduled task (session 0)
 *   doesn't either. Only a restart issued from within the seat's own session
 *   does -- presumably because of the order in which each session's audio graph
 *   is rebuilt and what state the RDP client's stream is left in.
 *
 *   That is an awkward combination: it needs to be elevated AND in a session
 *   belonging to a standard user. hydrad can already do exactly that --
 *   launch_in_session_as_system is what the input agent uses to reach any
 *   desktop. Same tool, different job.
 */
/* Play one short sound in a seat's session, to prime the audio endpoint.
 *
 * The endpoint goes bad when idle and the FIRST opener gets silence. Making a
 * throwaway sound the first opener means the user's next app is second, and
 * works. Verified by hand: any sound before a browser fixes the browser.
 *
 * Uses the same launch path as audiofix -- the seat's session, console admin's
 * elevated token -- because it needs to reach the seat's own audio session. */
static std::wstring cmd_chime(const std::wstring& seatName)
{
    std::lock_guard<std::mutex> lk(g_planMx);
    std::wstring want = seatName.empty() ? L"B" : seatName;

    for (const SeatCfg& s : g_cfg.seats) {
        if (widen(s.name) != want) continue;
        DWORD sess = resolve_session(s.session);
        if (sess == 0xFFFFFFFF)
            return L"seat " + widen(s.name) + L": session not up\r\n";

        wchar_t sysdir[MAX_PATH]{};
        GetSystemDirectoryW(sysdir, MAX_PATH);
        std::wstring ps = std::wstring(sysdir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";

        /* A stock Windows .wav, played synchronously so the process holds the
         * endpoint for its full duration rather than exiting immediately. */
        std::wstring args =
            L"-NoProfile -WindowStyle Hidden -Command "
            L"\"(New-Object Media.SoundPlayer "
            L"'C:\\Windows\\Media\\Windows Notify System Generic.wav').PlaySync()\"";

        HANDLE h = launch_in_session_as_console_admin(
            sess, L"chime:" + widen(s.name), ps, args);
        if (!h) return L"seat " + widen(s.name) + L": chime launch failed (see log)\r\n";
        CloseHandle(h);
        return L"seat " + widen(s.name) + L": audio primed in session "
             + std::to_wstring(sess) + L"\r\n";
    }
    return L"no such seat: " + seatName + L"\r\n";
}

static std::wstring cmd_audiofix(const std::wstring& seatName)
{
    std::lock_guard<std::mutex> lk(g_planMx);
    std::wstring want = seatName.empty() ? L"B" : seatName;

    for (const SeatCfg& s : g_cfg.seats) {
        if (widen(s.name) != want) continue;
        DWORD sess = resolve_session(s.session);
        if (sess == 0xFFFFFFFF)
            return L"seat " + widen(s.name) + L": session not up\r\n";

        wchar_t sysdir[MAX_PATH]{};
        GetSystemDirectoryW(sysdir, MAX_PATH);
        std::wstring ps = std::wstring(sysdir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";

        /* USER token, not SYSTEM.
         *
         * Measured: launching this as SYSTEM in the seat's session runs fine
         * (the log shows the pid) and fixes nothing, while the identical command
         * run by hand from an elevated shell in that same session works. The
         * difference is the ACCOUNT: SYSTEM has no interactive audio session of
         * its own, so the per-user audio graph that actually needs rebuilding is
         * not the one it touches. launch_in_session uses the session user's
         * token, which is what the successful manual case had. */
        HANDLE h = launch_in_session_as_console_admin(
            sess, L"audiofix:" + widen(s.name), ps,
            L"-NoProfile -WindowStyle Hidden -Command \"Restart-Service Audiosrv -Force\"");
        if (!h) return L"seat " + widen(s.name) + L": launch failed (see log)\r\n";
        /* Fire and forget: it exits on its own once the service is back. */
        CloseHandle(h);
        return L"seat " + widen(s.name) + L": audio service restarting in session "
             + std::to_wstring(sess) + L" (allow ~10s before playing)\r\n";
    }
    return L"no such seat: " + seatName + L"\r\n";
}

static std::wstring cmd_learn()
{
    DWORD sess = console_session();
    if (sess == 0xFFFFFFFF) return L"no console session to run --learn in\r\n";
    /* Fire-and-forget the router's device enumerator into the console session,
     * with its own visible console so the user can watch device numbers. */
    HANDLE userTok = nullptr, primary = nullptr; void* env = nullptr;
    if (!WTSQueryUserToken(sess, &userTok)) return L"WTSQueryUserToken failed\r\n";
    DuplicateTokenEx(userTok, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary);
    CreateEnvironmentBlock(&env, primary, FALSE);
    std::wstring exe = helper_path(L"seat_router.exe");
    std::wstring cmd = L"\"" + exe + L"\" --learn";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessAsUserW(primary, exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                                   CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE, env,
                                   exe_dir().c_str(), &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(primary); CloseHandle(userTok);
    return ok ? L"launched seat_router --learn on the console; watch that window\r\n"
              : L"failed to launch --learn\r\n";
}

static std::wstring dispatch(const std::wstring& cmd)
{
    /* tokens: verb [arg] */
    std::wstring verb = cmd, arg;
    size_t sp = cmd.find(L' ');
    if (sp != std::wstring::npos) { verb = cmd.substr(0, sp); arg = cmd.substr(sp + 1); }
    while (!arg.empty() && (arg.back() == L'\r' || arg.back() == L'\n' || arg.back() == L' ')) arg.pop_back();
    while (!verb.empty() && (verb.back() == L'\r' || verb.back() == L'\n')) verb.pop_back();

    if (verb == L"status")  return cmd_status();
    if (verb == L"reload")  return load_and_apply() ? L"reloaded\r\n" : L"reload failed (see log)\r\n";
    if (verb == L"restart") return cmd_restart(arg.empty() ? L"all" : arg);
    if (verb == L"learn")   return cmd_learn();
    if (verb == L"chime")   return cmd_chime(arg);      /* prime the seat's audio endpoint */
    if (verb == L"audiofix") return cmd_audiofix(arg);  /* restart audio IN the seat's session */
    if (verb == L"display") return cmd_display(arg);   /* <seat> <idd|capture> -- live A/B switch */
    if (verb == L"stop")    { g_stop = true; return L"stopping\r\n"; }
    return L"unknown command; try: status | reload | restart <seat|all> | display <seat> <idd|capture> | audiofix <seat> | learn | stop\r\n";
}

static DWORD WINAPI control_thread(LPVOID)
{
    for (;;) {
        if (g_stop) break;
        HANDLE pipe = CreateNamedPipeW(HYDRA_CONTROL_PIPE,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 8192, 8192, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(500); continue; }

        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE :
                         (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            wchar_t buf[1024]; DWORD rd = 0;
            if (ReadFile(pipe, buf, sizeof(buf) - sizeof(wchar_t), &rd, nullptr) && rd) {
                buf[rd / sizeof(wchar_t)] = 0;
                std::wstring resp = dispatch(buf);
                DWORD wr = 0;
                WriteFile(pipe, resp.c_str(), (DWORD)(resp.size() * sizeof(wchar_t)), &wr, nullptr);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

/* ===================================================================== *
 * Core run loop (shared by service + console mode)
 * ===================================================================== */
static void hydra_run()
{
    ensure_dirs();
    hlog(L"hydrad starting; exe dir %s", exe_dir().c_str());

    if (!load_and_apply())
        hlog(L"starting with no valid config; waiting for `hydractl reload`");

    HANDLE ctl = CreateThread(nullptr, 0, control_thread, nullptr, 0, nullptr);

    while (!g_stop) {
        reconcile_once();
        Sleep(500);
    }

    /* Shutdown: stop helpers, drop IDDs. */
    hlog(L"hydrad stopping; tearing down");
    {
        std::lock_guard<std::mutex> lk(g_planMx);
        for (Proc& p : g_plan.procs) kill_proc(p);
        for (auto& kv : g_plan.idds) SwDeviceClose(kv.second);
        g_plan.procs.clear();
        g_plan.idds.clear();
    }
    if (ctl) {
        /* nudge the control thread out of ConnectNamedPipe by opening the pipe */
        HANDLE poke = CreateFileW(HYDRA_CONTROL_PIPE, GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (poke != INVALID_HANDLE_VALUE) CloseHandle(poke);
        WaitForSingleObject(ctl, 2000);
        CloseHandle(ctl);
    }
}

/* ===================================================================== *
 * Service plumbing
 * ===================================================================== */
static SERVICE_STATUS        g_svcStatus{};
static SERVICE_STATUS_HANDLE g_svcHandle = nullptr;

static void set_state(DWORD state, DWORD exitCode = 0)
{
    g_svcStatus.dwCurrentState = state;
    g_svcStatus.dwWin32ExitCode = exitCode;
    g_svcStatus.dwControlsAccepted = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(g_svcHandle, &g_svcStatus);
}

static DWORD WINAPI svc_ctrl(DWORD ctrl, DWORD, LPVOID, LPVOID)
{
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        set_state(SERVICE_STOP_PENDING);
        g_stop = true;
        return NO_ERROR;
    }
    return NO_ERROR;
}

static VOID WINAPI svc_main(DWORD, LPWSTR*)
{
    g_svcHandle = RegisterServiceCtrlHandlerExW(SVC_NAME, svc_ctrl, nullptr);
    if (!g_svcHandle) return;
    g_svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    set_state(SERVICE_START_PENDING);
    set_state(SERVICE_RUNNING);
    hydra_run();
    set_state(SERVICE_STOPPED);
}

/* ---- install / uninstall helpers ---- */
static int svc_install()
{
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { wprintf(L"OpenSCManager failed %lu\n", GetLastError()); return 1; }
    SC_HANDLE svc = CreateServiceW(scm, SVC_NAME, L"Hydra Multiseat",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, path, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) { wprintf(L"CreateService failed %lu\n", GetLastError()); CloseServiceHandle(scm); return 1; }
    wprintf(L"installed service '%s'\n", SVC_NAME);
    CloseServiceHandle(svc); CloseServiceHandle(scm);
    return 0;
}

static int svc_uninstall()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return 1;
    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS | DELETE);
    if (svc) {
        SERVICE_STATUS st{}; ControlService(svc, SERVICE_CONTROL_STOP, &st);
        DeleteService(svc);
        wprintf(L"removed service '%s'\n", SVC_NAME);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return 0;
}

/* ===================================================================== *
 * Entry
 * ===================================================================== */
int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2) {
        std::wstring a = argv[1];
        if (a == L"install")   return svc_install();
        if (a == L"uninstall") return svc_uninstall();
        if (a == L"run" || a == L"console") {   /* run in foreground for debugging */
            g_console = true;
            SetConsoleCtrlHandler([](DWORD) -> BOOL { g_stop = true; return TRUE; }, TRUE);
            hydra_run();
            return 0;
        }
        wprintf(L"usage: hydrad [install|uninstall|run]\n");
        wprintf(L"  (no args) : run as a service (invoked by the SCM)\n");
        return 2;
    }

    /* No args: assume the SCM is starting us. */
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(SVC_NAME), svc_main },
        { nullptr, nullptr }
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        /* Not started by SCM and no verb: hint the user. */
        wprintf(L"hydrad is a service. Try: hydrad run  (foreground) or hydrad install\n");
        return 1;
    }
    return 0;
}
