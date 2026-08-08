/* seatB_agent.c  --  multiseat input router, seat replay agent (v3)
 *
 * Runs INSIDE an extra seat's Windows session (seat B, C, ...). Connects to
 * seat_router (console session) over loopback TCP on that seat's port,
 * receives raw input events, and replays them into THIS session with
 * SendInput. SendInput targets the calling thread's session, so the events
 * land in this seat and never on the console.
 *
 * Coalescing (from the first C port): zero per-event allocation, and a
 * drained burst is folded into ONE SendInput call -- a flood of relative
 * moves collapses to a single injected move, while lone keystrokes still go
 * one-in-one-out. Wire format is byte-identical to seat_router.c's WireEvent.
 *
 * v2 -> v3:
 *   - SELF HANG-WATCHDOG. The v2 keepalive detects a dead ROUTER connection,
 *     but a hung AGENT -- this process's own thread wedged, e.g. inside
 *     SendInput -- isn't reading the socket at all, so no recv timeout can
 *     ever fire, and it looks alive to a plain supervisor. A cross-session
 *     kill from the router would need privileges (the seats run as different
 *     users), so the fix lives where no privileges are needed: a watchdog
 *     thread INSIDE this process checks that the pump keeps making progress
 *     (on a healthy link the 200 ms heartbeat guarantees a tick at least
 *     every ~800 ms) and, after HANG_MS of stillness, terminates its own
 *     process with a nonzero code. respawn.exe then starts a fresh one.
 *   - ABSOLUTE DEVICES. Records flagged WIRE_M_ABS in the high bits of
 *     WireEvent.a carry 0..65535 normalized coordinates (u16 bit-preserved
 *     in the i16 dx/dy) and are injected with MOUSEEVENTF_ABSOLUTE
 *     [+VIRTUALDESK]. Absolute moves coalesce by REPLACEMENT (newest
 *     position wins) and never fold across modes.
 *   - HORIZONTAL WHEEL. Interception state bit 0x800 has ridden the wire
 *     since v1 but was silently ignored on replay; it now maps to
 *     MOUSEEVENTF_HWHEEL. Tilt wheels tilt.
 *   - MODIFIER RELEASE ON (RE)CONNECT. The router drops records under
 *     backpressure, and any disconnect can eat a keyup mid-chord -- either
 *     way a modifier can be left stuck down in this session. On every
 *     connection the agent injects keyups for both Shifts, Ctrls, Alts and
 *     Win keys; a keyup for a key that's already up is a no-op, so this is
 *     free when nothing was stuck.
 *   - QUICKEDIT DISABLED on our console, so a stray drag-select can't block
 *     the pump's logging writes (a blocked pump would otherwise trip the
 *     watchdog for no good reason).
 *
 * (v1 -> v2 recap: 800 ms SO_RCVTIMEO stall detection backed by the router's
 * 200 ms 'H' keepalive; SO_KEEPALIVE; uniform reconnect on any error.)
 *
 * Build (x64 Native Tools Command Prompt -- pure Win32):
 *   cl /O2 seatB_agent.c
 *
 * Run (INSIDE the seat's session -- Startup folder or logon Scheduled Task):
 *   seatB_agent.exe [router_host] [port]
 * defaults: 127.0.0.1 56789  (extra seats: match the port from the router's
 * triple for that seat, e.g. seatB_agent.exe 127.0.0.1 56790)
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>   /* must precede windows.h */
#include <ws2tcpip.h>
#include <windows.h>
#include "../common/hydra_ipc.h"
#include <mmsystem.h>   /* timeBeginPeriod / timeEndPeriod */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>      /* wcscpy_s (used by the UIPI diagnostic) */

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x1000
#endif

/* recv silence past this -> assume the router side is wedged, reconnect.
 * Sub-second, and 4x the router's 200 ms heartbeat so a couple of late beats
 * under momentary load don't trip a false reconnect. Tighten only in lockstep
 * with the router's HEARTBEAT_MS. */
#define STALL_MS 800

/* Pump stillness past this -> OUR OWN thread is wedged -> self-terminate
 * nonzero and let respawn.exe start a fresh process. On a healthy link the
 * pump ticks at least every ~STALL_MS; in the reconnect loop, about once a
 * second. 5 s is therefore 5+ consecutive missed ticks. */
#define HANG_MS 5000

/* ------------------------------------------------------------------------- *
 * Wire record -- MUST byte-match WireEvent in seat_router.c.
 *   kind(u8) a(u16) b(u16) dx(i16) dy(i16)  -> 9 bytes, little-endian, packed
 *   kind: 'K' keyboard, 'M' mouse, 'H' keepalive (ignored)
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct {
    unsigned char  kind;
    unsigned short a;     /* K: scancode        ; M: state + WIRE_M_* flags */
    unsigned short b;     /* K: key state flags ; M: wheel rolling (as u16)  */
    short          dx;    /* M: relative dx, or absolute x (u16 bits)       */
    short          dy;    /* M: relative dy, or absolute y (u16 bits)       */
} WireEvent;
#pragma pack(pop)

/* Move-mode flags in the high bits of WireEvent.a for kind 'M'. Interception
 * button/wheel state uses bits 0x001..0x800, so these are free.
 * MUST match seat_router.c. */
#define WIRE_M_ABS   0x1000
#define WIRE_M_VDESK 0x2000

/* Interception key state bits (carried raw in WireEvent.b for kind 'K'). */
#define I_KEY_UP  0x01
#define I_KEY_E0  0x02

/* Interception mouse state bit -> (SendInput flag, mouseData). Mirrors the
 * I_MOUSE table in the Python agent exactly. */
#define I_MOUSE_WHEEL  0x400
#define I_MOUSE_HWHEEL 0x800
static const struct { unsigned short bit; DWORD flag; DWORD data; } MOUSE_MAP[] = {
    { 0x001, MOUSEEVENTF_LEFTDOWN,   0        },
    { 0x002, MOUSEEVENTF_LEFTUP,     0        },
    { 0x004, MOUSEEVENTF_RIGHTDOWN,  0        },
    { 0x008, MOUSEEVENTF_RIGHTUP,    0        },
    { 0x010, MOUSEEVENTF_MIDDLEDOWN, 0        },
    { 0x020, MOUSEEVENTF_MIDDLEUP,   0        },
    { 0x040, MOUSEEVENTF_XDOWN,      XBUTTON1 },
    { 0x080, MOUSEEVENTF_XUP,        XBUTTON1 },
    { 0x100, MOUSEEVENTF_XDOWN,      XBUTTON2 },
    { 0x200, MOUSEEVENTF_XUP,        XBUTTON2 },
};
#define MOUSE_MAP_N (sizeof(MOUSE_MAP) / sizeof(MOUSE_MAP[0]))

/* Pump liveness, incremented by the pump/reconnect loops, watched by the
 * hang watchdog thread. */
static volatile LONG g_tick = 0;

/* ------------------------------------------------------------------------- *
 * Batch buffer. Static storage -> no malloc in the hot path, no allocator
 * jitter. Sized well past a full recv's worth of events so flush-when-full
 * effectively never fires; the guard keeps it correct even if it does.
 * ------------------------------------------------------------------------- */
#define BATCH_MAX 4096
static INPUT batch[BATCH_MAX];
static int   batch_n = 0;

/* DIAGNOSTIC (once): SendInput failing with ACCESS_DENIED (err=5) every call
 * is the textbook signature of UIPI blocking a lower-integrity process from
 * injecting into a higher-integrity foreground window, OR the session being
 * on a secure desktop (which has no normal foreground window at all). Rather
 * than guess which, log exactly what's actually in the way -- our own
 * integrity level, and whatever currently owns the foreground window (name +
 * integrity) in THIS session/desktop -- the moment it happens. */
static void log_uipi_diagnostic(void) {
    static int logged = 0;
    if (logged) return;
    logged = 1;

    DWORD ourLevel = 0xFFFFFFFF;
    HANDLE ourTok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ourTok)) {
        BYTE buf[64]; DWORD sz = 0;
        if (GetTokenInformation(ourTok, TokenIntegrityLevel, buf, sizeof(buf), &sz)) {
            PTOKEN_MANDATORY_LABEL tml = (PTOKEN_MANDATORY_LABEL)buf;
            PSID sid = tml->Label.Sid;
            PUCHAR cnt = GetSidSubAuthorityCount(sid);
            ourLevel = *GetSidSubAuthority(sid, (DWORD)(*cnt - 1));
        }
        CloseHandle(ourTok);
    }

    HWND fg = GetForegroundWindow();
    wchar_t fgName[MAX_PATH] = L"(none)";
    DWORD fgLevel = 0xFFFFFFFF;
    if (fg) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp) {
            DWORD sz = MAX_PATH;
            QueryFullProcessImageNameW(hp, 0, fgName, &sz);
            HANDLE tok = NULL;
            if (OpenProcessToken(hp, TOKEN_QUERY, &tok)) {
                BYTE buf[64]; DWORD tsz = 0;
                if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &tsz)) {
                    PTOKEN_MANDATORY_LABEL tml = (PTOKEN_MANDATORY_LABEL)buf;
                    PSID sid = tml->Label.Sid;
                    PUCHAR cnt = GetSidSubAuthorityCount(sid);
                    fgLevel = *GetSidSubAuthority(sid, (DWORD)(*cnt - 1));
                }
                CloseHandle(tok);
            }
            CloseHandle(hp);
        }
    } else {
        wcscpy_s(fgName, MAX_PATH, L"(NO foreground window in this session)");
    }

    /* NARROW fprintf, not fwprintf: this stream's orientation is already set
     * to byte/narrow by the fprintf() calls elsewhere in this file (the line
     * right above this call, for one) -- mixing narrow and wide output on the
     * SAME stdio stream is undefined behavior in the Windows CRT and was
     * silently swallowing this diagnostic entirely. %ls prints the one wide
     * string from within a narrow-oriented call; DWORDs are unaffected. */
    fprintf(stderr,
        "[agent] UIPI DIAG: our integrity=0x%lX; foreground window owner=%ls "
        "integrity=0x%lX  (0x1000=Low 0x2000=Medium 0x3000=High/elevated "
        "0x4000=System; foreground=none usually means a secure desktop -- "
        "UAC prompt / lock screen -- is active in this session)\n",
        ourLevel, fgName, fgLevel);
}

/* Re-bind this thread to whatever is CURRENTLY the session's input desktop.
 *
 * SendInput delivers to the calling THREAD'S desktop, and we bind once at
 * startup. The moment the session switches input desktop -- lock screen, screen
 * saver, a UAC/secure desktop, or a desktop switch triggered by focus changes --
 * our handle points at a desktop that is no longer receiving, and every
 * SendInput returns 0 with ERROR_ACCESS_DENIED (5) forever after. Seat B's
 * keyboard and mouse simply stop working, the log fills with err 5, and the only
 * cure was restarting the service.
 *
 * session_capture had the identical bug and was fixed the same way: stop
 * assuming the desktop you started on is the desktop that matters, and re-attach
 * when the evidence says otherwise. */
static int reattach_input_desktop(void) {
    HDESK d = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!d) return 0;
    int ok = SetThreadDesktop(d) ? 1 : 0;
    /* Deliberately NOT closing the previous desktop handle: SetThreadDesktop
     * fails if the old desktop still has windows owned by this thread, and
     * leaking one handle per switch is cheaper than the alternative. */
    if (!ok) CloseDesktop(d);
    return ok;
}

static void flush(void) {
    if (batch_n == 0) return;
    UINT sent = SendInput((UINT)batch_n, batch, (int)sizeof(INPUT));
    if (sent != (UINT)batch_n) {
        DWORD err = GetLastError();
        /* ACCESS_DENIED almost always means "wrong desktop", not "wrong
         * permissions" -- so try re-attaching and replay the batch once before
         * giving up. Rate-limited so a genuinely blocked state (an elevated
         * foreground window we can't reach by any desktop) doesn't spin. */
        if (err == ERROR_ACCESS_DENIED) {
            static DWORD lastTry = 0;
            DWORD now = GetTickCount();
            if (now - lastTry >= 500) {
                lastTry = now;
                if (reattach_input_desktop()) {
                    sent = SendInput((UINT)batch_n, batch, (int)sizeof(INPUT));
                    if (sent == (UINT)batch_n) {
                        fprintf(stderr, "[agent] input desktop changed; re-attached and recovered\n");
                        fflush(stderr);
                        batch_n = 0;
                        return;
                    }
                }
            }
        }
        fprintf(stderr, "[agent] SendInput sent %u/%d (err %lu)\n",
                sent, batch_n, err);
        log_uipi_diagnostic();
    }
    batch_n = 0;
}

/* Append a relative mouse move -- FOLDING it into the previous input if that
 * input is also a pure relative move. Relative deltas add, so folding a run
 * of moves is lossless; and we never fold across a button/wheel/key/absolute,
 * so a click still lands at the position it was issued from. */
static void push_move(int dx, int dy) {
    if (batch_n > 0 &&
        batch[batch_n - 1].type == INPUT_MOUSE &&
        batch[batch_n - 1].mi.dwFlags == MOUSEEVENTF_MOVE) {
        batch[batch_n - 1].mi.dx += dx;
        batch[batch_n - 1].mi.dy += dy;
        return;
    }
    if (batch_n >= BATCH_MAX) flush();
    INPUT *in = &batch[batch_n++];
    memset(in, 0, sizeof(*in));
    in->type       = INPUT_MOUSE;
    in->mi.dx      = dx;
    in->mi.dy      = dy;
    in->mi.dwFlags = MOUSEEVENTF_MOVE;
}

/* Absolute moves coalesce by REPLACEMENT -- the newest position wins, which
 * is lossless for a position stream -- and only fold into an identical-flag
 * absolute move, never across modes or past a button/wheel/key. */
static void push_move_abs(LONG x, LONG y, DWORD flags) {
    if (batch_n > 0 &&
        batch[batch_n - 1].type == INPUT_MOUSE &&
        batch[batch_n - 1].mi.dwFlags == flags) {
        batch[batch_n - 1].mi.dx = x;
        batch[batch_n - 1].mi.dy = y;
        return;
    }
    if (batch_n >= BATCH_MAX) flush();
    INPUT *in = &batch[batch_n++];
    memset(in, 0, sizeof(*in));
    in->type       = INPUT_MOUSE;
    in->mi.dx      = x;
    in->mi.dy      = y;
    in->mi.dwFlags = flags;
}

static void push_mouse(DWORD flags, DWORD data) {
    if (batch_n >= BATCH_MAX) flush();
    INPUT *in = &batch[batch_n++];
    memset(in, 0, sizeof(*in));
    in->type          = INPUT_MOUSE;
    in->mi.dwFlags    = flags;
    in->mi.mouseData  = data;
}

static void push_key(unsigned short scancode, unsigned short state) {
    DWORD flags = KEYEVENTF_SCANCODE;
    if (state & I_KEY_UP) flags |= KEYEVENTF_KEYUP;
    if (state & I_KEY_E0) flags |= KEYEVENTF_EXTENDEDKEY;
    if (batch_n >= BATCH_MAX) flush();
    INPUT *in = &batch[batch_n++];
    memset(in, 0, sizeof(*in));
    in->type       = INPUT_KEYBOARD;
    in->ki.wScan   = scancode;
    in->ki.dwFlags = flags;
}

/* Release the common modifiers in THIS session. Called on every (re)connect:
 * if the router dropped records under backpressure, or the link died between
 * a keydown and its keyup, a modifier can be left stuck down here. A keyup
 * for a key that is already up is a harmless no-op. */
static void release_modifiers(void) {
    static const struct { unsigned short sc, st; } mods[] = {
        { 0x2A, I_KEY_UP            },   /* LShift        */
        { 0x36, I_KEY_UP            },   /* RShift        */
        { 0x1D, I_KEY_UP            },   /* LCtrl         */
        { 0x1D, I_KEY_UP | I_KEY_E0 },   /* RCtrl         */
        { 0x38, I_KEY_UP            },   /* LAlt          */
        { 0x38, I_KEY_UP | I_KEY_E0 },   /* RAlt / AltGr  */
        { 0x5B, I_KEY_UP | I_KEY_E0 },   /* LWin          */
        { 0x5C, I_KEY_UP | I_KEY_E0 },   /* RWin          */
    };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++)
        push_key(mods[i].sc, mods[i].st);
    flush();
}

/* One wire record -> appended INPUTs. 'K' key, 'M' mouse; anything else (the
 * router's 'H' keepalive, or an unknown future kind) injects nothing and is
 * skipped explicitly -- so a keepalive can never be mistaken for a phantom
 * mouse event even if a later revision carries data in its fields. Mouse
 * order mirrors v1: move first, then button transitions in bit order, then
 * wheel(s). */
static void process_record(const WireEvent *ev) {
    if (ev->kind == 'K') {
        push_key(ev->a, ev->b);
        return;
    }
    if (ev->kind != 'M')
        return;                                  /* 'H' keepalive / unknown */

    if (ev->a & WIRE_M_ABS) {
        DWORD f = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        if (ev->a & WIRE_M_VDESK) f |= MOUSEEVENTF_VIRTUALDESK;
        /* Absolute strokes always carry a position report (0..65535,
         * bit-preserved through the i16 wire fields) -- emit it even at
         * (0,0), which is a real corner, not "no movement". */
        push_move_abs((LONG)(unsigned short)ev->dx,
                      (LONG)(unsigned short)ev->dy, f);
    } else if (ev->dx || ev->dy) {
        push_move(ev->dx, ev->dy);
    }

    for (size_t i = 0; i < MOUSE_MAP_N; i++)
        if (ev->a & MOUSE_MAP[i].bit)
            push_mouse(MOUSE_MAP[i].flag, MOUSE_MAP[i].data);

    if (ev->a & I_MOUSE_WHEEL) {
        short wheel = (short)ev->b;              /* signed rolling delta */
        push_mouse(MOUSEEVENTF_WHEEL, (DWORD)wheel);
    }
    if (ev->a & I_MOUSE_HWHEEL) {                /* v3: tilt wheels tilt */
        short wheel = (short)ev->b;
        push_mouse(MOUSEEVENTF_HWHEEL, (DWORD)wheel);
    }
}

/* ------------------------------------------------------------------------- *
 * Receive accumulator. recv() appends after any partial record left over from
 * last time; we parse whole 9-byte records, then compact the remainder to the
 * front. Bounded, no realloc.
 *
 * Returns when the connection should be torn down and re-established:
 *   closed / error   -> reconnect
 *   STALL_MS silence -> reconnect (WSAETIMEDOUT via SO_RCVTIMEO; on a healthy
 *                       link the 200 ms keepalive keeps this from ever firing)
 * ------------------------------------------------------------------------- */
#define RECV_CHUNK 8192
#define ACC_CAP    (RECV_CHUNK + 16)

static void pump(SOCKET s) {
    unsigned char acc[ACC_CAP];
    size_t acc_len = 0;

    for (;;) {
        int room = (int)(ACC_CAP - acc_len);
        int cap  = room < RECV_CHUNK ? room : RECV_CHUNK;
        int n    = recv(s, (char *)acc + acc_len, cap, 0);
        InterlockedIncrement(&g_tick);           /* pump liveness */
        if (n == 0) {
            fprintf(stderr, "[agent] router closed connection\n");
            return;
        }
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT)
                fprintf(stderr, "[agent] no input or keepalive for %d ms; "
                                "router wedged, reconnecting\n", STALL_MS);
            else
                fprintf(stderr, "[agent] recv error %d; reconnecting\n", e);
            return;
        }
        acc_len += (size_t)n;

        size_t off = 0;
        while (acc_len - off >= sizeof(WireEvent)) {
            WireEvent ev;
            memcpy(&ev, acc + off, sizeof(ev));  /* no alignment assumptions */
            off += sizeof(ev);
            process_record(&ev);
        }
        if (off) {
            acc_len -= off;
            if (acc_len) memmove(acc, acc + off, acc_len);
        }

        /* One SendInput for the whole burst that was ready on this wakeup. A
         * lone event still flushes immediately (recv returned one record);
         * a flick's worth of moves flushes as a single folded move; a lone
         * keepalive produces nothing and flush() is a no-op. */
        flush();
    }
}

/* Watches the pump. On a healthy link g_tick advances at least every
 * ~STALL_MS (heartbeats), and about once a second while reconnecting; if it
 * goes still for HANG_MS, this thread's sibling is wedged somewhere no error
 * can reach (SendInput, a blocked write, ...). TerminateProcess -- not
 * ExitProcess, which can itself deadlock behind a wedged thread's locks --
 * with a nonzero code, and respawn.exe starts a fresh agent. */
static DWORD WINAPI hang_watchdog(LPVOID arg) {
    (void)arg;
    LONG      last  = g_tick;
    ULONGLONG since = GetTickCount64();
    for (;;) {
        Sleep(1000);
        LONG      now = g_tick;
        ULONGLONG t   = GetTickCount64();
        if (now != last) { last = now; since = t; continue; }
        if (t - since >= HANG_MS) {
            fprintf(stderr, "[agent] pump wedged for %lu ms; dying for "
                            "respawn\n", (unsigned long)(t - since));
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    /* not reached */
    return 0;
}

/* ------------------------------------------------------------------------- */
static BOOL WINAPI on_ctrl(DWORD type) {
    (void)type;
    timeEndPeriod(1);                            /* restore timer res on exit */
    return FALSE;                                /* let default handler kill us */
}

static void disable_quickedit(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode))
        SetConsoleMode(in, (mode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
}

/* Publish the cursor position for hydrardp. RDP does not send positions to a
 * client that generates no input -- see hydra_ipc.h. This agent is already
 * SYSTEM inside the seat's session with the desktop attached, so GetCursorPos
 * here is simply correct. 60 Hz, matching the client's publish rate. */
static DWORD WINAPI cursor_pos_thread(LPVOID arg) {
    const char *seat = (const char *)arg;
    wchar_t name[128];
    hydra_pixels_name(name, 128, seat);
    HANDLE map = NULL;
    HydraSeatPixels *hdr = NULL;
    for (;;) {
        if (!hdr) {
            map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
            if (map) {
                void *v = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HydraSeatPixels));
                if (v) hdr = (HydraSeatPixels *)v; else { CloseHandle(map); map = NULL; }
            }
            if (!hdr) { Sleep(2000); continue; }
            fprintf(stderr, "[agent] publishing cursor position for hydrardp\\n"); fflush(stderr);
        }
        POINT pt;
        if (GetCursorPos(&pt)) {
            hdr->curX = (int32_t)pt.x;
            hdr->curY = (int32_t)pt.y;
            MemoryBarrier();
            hdr->curSeq = hdr->curSeq + 1;
        }
        Sleep(16);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    const char *port = (argc > 2) ? argv[2] : "56789";

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    disable_quickedit();

    /* Responsiveness levers:
     *   - HIGH_PRIORITY_CLASS + THREAD_PRIORITY_HIGHEST so the scheduler
     *     doesn't deprioritise input replay when both seats are busy.
     *     (Not REALTIME/TIME_CRITICAL -- that can starve the session's own
     *     DWM/input and does more harm than good on a shared box.)
     *   - timeBeginPeriod(1) tightens the scheduler quantum. NOTE: on a
     *     laptop (Surface Book 3) this nudges power draw up; drop it if you
     *     care more about battery than the last few hundred microseconds. */
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    timeBeginPeriod(1);

    /* Cursor position publisher for hydrardp -- harmless when nothing reads it. */
    { static char seatName[8] = "B"; CreateThread(NULL, 0, cursor_pos_thread, seatName, 0, NULL); }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[agent] WSAStartup failed\n");
        return 1;                                /* fault: respawn restarts */
    }

    CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    for (;;) {
        InterlockedIncrement(&g_tick);           /* reconnect-loop liveness */

        res = NULL;
        if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
            fprintf(stderr, "[agent] cannot resolve %s:%s; retrying in 1s\n",
                    host, port);
            Sleep(1000);
            continue;
        }

        SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == INVALID_SOCKET) {
            freeaddrinfo(res);
            Sleep(1000);
            continue;
        }

        if (connect(s, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
            closesocket(s);
            freeaddrinfo(res);
            fprintf(stderr, "[agent] router not up; retrying in 1s\n");
            Sleep(1000);
            continue;
        }
        freeaddrinfo(res);

        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));

        /* Stall detection: SO_RCVTIMEO makes a silent-but-open socket return
         * WSAETIMEDOUT instead of blocking forever; the router's 200 ms
         * keepalive is what keeps a healthy idle link from ever hitting it.
         * SO_KEEPALIVE additionally catches a dead peer that never sends
         * FIN, independent of the record heartbeat. */
        DWORD tmo = STALL_MS;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof(tmo));
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char *)&one, sizeof(one));

        fprintf(stderr, "[agent] connected to router %s:%s (stall timeout "
                        "%d ms); clearing stuck modifiers\n",
                host, port, STALL_MS);
        release_modifiers();                     /* fresh chord state */

        pump(s);                                 /* blocks until disconnect/stall */

        closesocket(s);
        batch_n = 0;                             /* drop any partial batch   */
        fprintf(stderr, "[agent] reconnecting in 1s\n");
        Sleep(1000);
    }

    /* not reached */
}



