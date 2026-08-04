/* seat_router.c  --  multiseat input router (v3)
 *
 * The "native input path" neo_multiseat leaves unfinished.
 *
 * Runs in the PHYSICAL CONSOLE session (seat A). Uses the Interception
 * driver to:
 *   - let seat A's keyboard/mouse pass through to the console normally;
 *   - capture each extra seat's keyboard/mouse, BLOCK them from the console,
 *     and forward the raw events over a loopback TCP socket to that seat's
 *     seatB_agent.exe, which runs INSIDE the seat's session and replays them
 *     with SendInput.
 *
 * v2 -> v3:
 *   - MULTI-SEAT. The router now takes <kbd> <mouse> <port> triples and
 *     drives up to MAX_SEATS extra seats (B, C, ...), each with its own
 *     agent, port, and independent backpressure state. The two-arg form
 *     (<kbd> <mouse>, default port) still works, so existing invocations
 *     don't change. One agent instance runs inside each seat's session,
 *     pointed at that seat's port.
 *   - ABSOLUTE DEVICES. Interception marks absolute-mode strokes (tablets,
 *     some touchpads) in the stroke flags with x/y as 0..65535 normalized
 *     coordinates. Those flags now travel in the spare high bits of
 *     WireEvent.a (button/wheel state only uses 0x001..0x800), and the agent
 *     injects MOUSEEVENTF_ABSOLUTE [+VIRTUALDESK] accordingly. The i16
 *     dx/dy fields carry the u16 coordinates bit-preserved.
 *   - HANG WATCHDOG. A monitor thread checks that the heartbeat thread is
 *     still ticking; if it stalls HANG_MS (a deadlock in the socket path, or
 *     a blocked console write), the process logs and terminates itself with
 *     a nonzero code so respawn.exe brings up a fresh one. NOTE the honest
 *     limit: a hang inside interception_wait() is indistinguishable from an
 *     idle box, so the *input* loop is not covered -- only the socket path.
 *   - QUICKEDIT DISABLED. Classic console gotcha: selecting text in a
 *     QuickEdit console blocks every write to it. Our heartbeat logs under
 *     g_lock, forward() takes the same lock from the input loop -- so one
 *     accidental drag-select in the router window would have frozen every
 *     extra seat's input until a keypress. The console input mode now has
 *     QuickEdit cleared at startup.
 *
 * (v1 -> v2 recap: non-blocking drop-on-full client sockets with framing-
 * preserving partial-send handling; 200 ms 'H' keepalive records; outer
 * restart loop around the Interception context; WSAStartup/bind retry.)
 *
 * Interception SDK: https://github.com/oblitum/Interception
 *
 * Build (x64 Native Tools Command Prompt, Interception SDK unpacked):
 *   cl /O2 seat_router.c /I <sdk>\include <sdk>\library\x64\interception.lib
 * (ws2_32 is pulled in via the pragma below.)
 *
 * 1) Identify device numbers:
 *      seat_router.exe --learn
 *    Press keys on each keyboard and wiggle each mouse; note the dev numbers
 *    that belong to each extra seat.
 *
 * 2) Run it:
 *      seat_router.exe <kbd> <mouse> [port]                      (one seat)
 *      seat_router.exe <kbd1> <mouse1> <port1> [<kbd2> <mouse2> <port2> ...]
 *    e.g.  seat_router.exe 2 12 56789
 *          seat_router.exe 2 12 56789 3 13 56790
 *
 * Exit codes: 2 = configuration error (respawn.exe will NOT restart);
 * anything else nonzero = fault (respawn.exe restarts).
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>   /* must precede windows.h */
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>     /* towlower (case-insensitive hardware-ID match) */
#include "interception.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

/* Heartbeat cadence. Each agent's stall timeout is a multiple of this (4x as
 * shipped: 200 ms pulse, 800 ms timeout), so a couple of missed beats under
 * momentary load don't trip a false wedge. */
#define HEARTBEAT_MS 200
/* Heartbeat-thread silence past this -> socket path is wedged -> self-kill
 * (nonzero exit) and let respawn bring up a fresh process. */
#define HANG_MS      5000

#define MAX_SEATS 4
#define EXIT_CONFIG 2

#pragma pack(push, 1)
typedef struct {
    unsigned char  kind;  /* 'K' keyboard, 'M' mouse, 'H' keepalive (no-op) */
    unsigned short a;     /* K: scancode        ; M: state + WIRE_M_* flags */
    unsigned short b;     /* K: key state flags ; M: wheel rolling (as u16)  */
    short          dx;    /* M: relative dx, or absolute x (u16 bits)       */
    short          dy;    /* M: relative dy, or absolute y (u16 bits)       */
} WireEvent;
#pragma pack(pop)

/* Move-mode flags carried in the high bits of WireEvent.a for kind 'M'.
 * Interception button/wheel state uses bits 0x001..0x800, so these are free.
 * MUST match seatB_agent.c. */
#define WIRE_M_ABS   0x1000   /* dx/dy are absolute 0..65535 coordinates */
#define WIRE_M_VDESK 0x2000   /* absolute coords span the virtual desktop */

/* ------------------------------------------------------------------------- *
 * Seats. One extra seat = one captured kbd + mouse, one listener/port, one
 * connected agent, and its own backpressure state. All mutable socket state
 * is guarded by the single g_lock -- contention is two threads exchanging
 * 9-byte sends, not worth per-seat locks.
 * ------------------------------------------------------------------------- */
typedef struct {
    int           kbd, mouse, port;
    /* Stable hardware-ID matching. When kbdId/mouseId are non-empty, the seat
     * matches on a case-insensitive SUBSTRING of the device's Interception
     * hardware ID instead of the volatile device number -- surviving reboots
     * and re-plugs. Empty => fall back to numeric kbd/mouse. */
    wchar_t       kbdId[256];
    wchar_t       mouseId[256];
    SOCKET        listener;
    SOCKET        client;                    /* guarded by g_lock            */
    /* Separate INJECT listener, on port + 1000.
     *
     * Injectors (mirror's windowed seat view) send us events to forward to the
     * seat, which is the OPPOSITE direction to the agent connection. They must
     * not share the agent's port: this accept loop is "newest connection wins",
     * so an injector arriving on the agent port silently DISPLACES the real
     * agent and the seat loses input entirely. That is exactly what happened
     * the first time this was tried. */
    SOCKET        inj_listener;
    SOCKET        inj_client;
    unsigned char tail[sizeof(WireEvent)];   /* unsent remainder of a record */
    int           tail_len;
    unsigned long drops;
    ULONGLONG     drop_log_tick;
} Seat;

static Seat g_seat[MAX_SEATS];
static int  g_nseat = 0;

static CRITICAL_SECTION g_lock;
static volatile LONG    g_hb_tick = 0;       /* heartbeat-thread liveness */

static char seat_name(const Seat *st) { return (char)('B' + (int)(st - g_seat)); }

/* Classic console gotcha: with QuickEdit on, selecting text blocks every
 * write to the console. Our logging happens under g_lock, which the input
 * loop also takes -- so a drag-select would freeze every extra seat's input
 * until a keypress. Turn it off. */
static void disable_quickedit(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode))
        SetConsoleMode(in, (mode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
}

/* Accept loop, one thread per seat: keep one connected agent. If a new one
 * connects, it wins. */
static DWORD WINAPI accept_thread(LPVOID arg) {
    Seat *st = (Seat *)arg;
    for (;;) {
        SOCKET s = accept(st->listener, NULL, NULL);
        if (s == INVALID_SOCKET) { Sleep(200); continue; }
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);       /* never block the input loop */
        EnterCriticalSection(&g_lock);
        if (st->client != INVALID_SOCKET) closesocket(st->client);
        st->client        = s;
        st->tail_len      = 0;              /* fresh stream, fresh framing */
        st->drops         = 0;
        st->drop_log_tick = 0;
        LeaveCriticalSection(&g_lock);
        fprintf(stderr, "[router] seat %c agent connected (port %d)\n",
                seat_name(st), st->port);
    }
    /* not reached */
    return 0;
}

/* Inject loop, one thread per seat. Accepts a connection on port+1000 and reads
 * 9-byte WireEvents from it, forwarding each into the seat exactly as if it had
 * come from the captured hardware. Used by mirror's windowed view so a teacher
 * can drive the seat from a window on the console screen.
 *
 * Deliberately single-injector and blocking: this path carries hand-driven mouse
 * and keyboard, not a firehose, so simplicity beats throughput. */
static void forward(Seat *st, const WireEvent *ev);   /* fwd decl */

static DWORD WINAPI inject_thread(LPVOID arg) {
    Seat *st = (Seat *)arg;
    for (;;) {
        SOCKET s = accept(st->inj_listener, NULL, NULL);
        if (s == INVALID_SOCKET) { Sleep(200); continue; }
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));
        fprintf(stderr, "[router] seat %c injector connected (port %d)\n",
                seat_name(st), st->port + 1000);

        unsigned char buf[sizeof(WireEvent) * 16];
        int have = 0;
        for (;;) {
            int n = recv(s, (char *)buf + have, (int)sizeof(buf) - have, 0);
            if (n <= 0) break;
            have += n;
            int off = 0;
            while (have - off >= (int)sizeof(WireEvent)) {
                WireEvent ev;
                memcpy(&ev, buf + off, sizeof(ev));
                off += (int)sizeof(ev);
                forward(st, &ev);
            }
            if (off && off < have) memmove(buf, buf + off, (size_t)(have - off));
            have -= off;
        }
        closesocket(s);
        fprintf(stderr, "[router] seat %c injector disconnected\n", seat_name(st));
    }
    return 0;
}

/* Call with g_lock held. */
static void kill_client_locked(Seat *st) {
    if (st->client != INVALID_SOCKET) closesocket(st->client);
    st->client   = INVALID_SOCKET;          /* accept_thread re-accepts */
    st->tail_len = 0;
}

/* Call with g_lock held. */
static void drop_one_locked(Seat *st) {
    st->drops++;
    ULONGLONG now = GetTickCount64();
    if (now - st->drop_log_tick >= 1000) {  /* at most one log line a second */
        st->drop_log_tick = now;
        fprintf(stderr, "[router] seat %c backpressure: %lu record(s) dropped "
                        "this connection\n", seat_name(st), st->drops);
    }
}

/* Non-blocking forward. Never stalls the interception loop: a full buffer
 * drops the record, a broken socket hands the client back to accept_thread,
 * and a partial send parks the remainder in st->tail (flushed before anything
 * newer, with whole records dropped meanwhile, to keep framing intact).
 *
 * Safe from both the interception loop and the heartbeat thread: all shared
 * state is touched only under g_lock, and complete records are emitted
 * atomically w.r.t. each other (a partial send's tail is drained first, under
 * the lock, before any thread sends its next record), so the two senders can
 * interleave at record granularity but never mid-record. */
static void forward(Seat *st, const WireEvent *ev) {
    EnterCriticalSection(&g_lock);
    SOCKET s = st->client;
    if (s == INVALID_SOCKET) { LeaveCriticalSection(&g_lock); return; }

    if (st->tail_len) {                     /* finish the owed record first */
        int n = send(s, (const char *)st->tail, st->tail_len, 0);
        if (n > 0) {
            if (n < st->tail_len)
                memmove(st->tail, st->tail + n, st->tail_len - n);
            st->tail_len -= n;
        } else if (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
            kill_client_locked(st);
            fprintf(stderr, "[router] seat %c agent connection lost\n",
                    seat_name(st));
            LeaveCriticalSection(&g_lock);
            return;
        }
        if (st->tail_len) {                 /* still owed: drop the new one whole */
            drop_one_locked(st);
            LeaveCriticalSection(&g_lock);
            return;
        }
    }

    int n = send(s, (const char *)ev, (int)sizeof(*ev), 0);
    if (n == (int)sizeof(*ev)) {
        /* sent */
    } else if (n >= 0) {                    /* partial: stash the remainder */
        st->tail_len = (int)sizeof(*ev) - n;
        memcpy(st->tail, (const char *)ev + n, st->tail_len);
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        drop_one_locked(st);
    } else {
        kill_client_locked(st);
        fprintf(stderr, "[router] seat %c agent connection lost\n",
                seat_name(st));
    }
    LeaveCriticalSection(&g_lock);
}

/* Keepalive pulse. One zero-payload record per seat every HEARTBEAT_MS,
 * forever; forward() no-ops on unconnected seats, so this is cheap when idle
 * and needs no connection bookkeeping of its own. It rides the same
 * backpressure path as real input -- under congestion a heartbeat is dropped
 * like any other record, which is harmless: congestion means real input is
 * already flowing and resetting the agent's timer. The pulse only has to
 * survive the IDLE case, where the buffer is empty and it always goes out.
 * ABOVE_NORMAL so its wakeup isn't delayed, but below the input loop's
 * HIGHEST so it never preempts a real stroke. */
static DWORD WINAPI heartbeat_thread(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    WireEvent hb = { 'H', 0, 0, 0, 0 };
    for (;;) {
        InterlockedIncrement(&g_hb_tick);
        for (int i = 0; i < g_nseat; i++)
            forward(&g_seat[i], &hb);
        Sleep(HEARTBEAT_MS);
    }
    /* not reached */
    return 0;
}

/* Watches the heartbeat thread. If it stops ticking for HANG_MS, the socket
 * path is wedged (lock deadlock, blocked console write, ...): die nonzero so
 * respawn.exe replaces us with a working process. Does NOT cover a hang
 * inside interception_wait() -- from out here that is indistinguishable from
 * two idle seats, so it stays an honest limit (see README). */
static DWORD WINAPI hang_watchdog(LPVOID arg) {
    (void)arg;
    LONG      last  = g_hb_tick;
    ULONGLONG since = GetTickCount64();
    for (;;) {
        Sleep(1000);
        LONG      now = g_hb_tick;
        ULONGLONG t   = GetTickCount64();
        if (now != last) { last = now; since = t; continue; }
        if (t - since >= HANG_MS) {
            fprintf(stderr, "[router] socket path wedged for %lu ms; dying "
                            "for respawn\n", (unsigned long)(t - since));
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    /* not reached */
    return 0;
}

/* Fetch a device's Interception hardware ID into buf (wide). Returns 1 on
 * success with a non-empty string, else 0. Stable across reboots/re-plugs,
 * unlike the device number. */
static InterceptionContext g_ctx = NULL;

static int get_hwid(InterceptionContext ctx, InterceptionDevice dev,
                    wchar_t *buf, size_t cap) {
    if (cap == 0) return 0;
    buf[0] = 0;
    size_t n = interception_get_hardware_id(ctx, dev, buf,
                                            (unsigned int)(cap * sizeof(wchar_t)));
    if (n == 0 || n > cap * sizeof(wchar_t)) { buf[0] = 0; return 0; }
    buf[cap - 1] = 0;
    return buf[0] != 0;
}

/* Case-insensitive substring test for wide strings. */
static int wcontains_ci(const wchar_t *hay, const wchar_t *needle) {
    if (!needle[0]) return 0;
    for (const wchar_t *h = hay; *h; ++h) {
        const wchar_t *a = h, *b = needle;
        while (*a && *b && towlower(*a) == towlower(*b)) { ++a; ++b; }
        if (!*b) return 1;
    }
    return 0;
}

/* Match a captured device to a seat: prefer stable hardware-ID substring when
 * the seat specifies an ID, else fall back to the numeric index. */
static Seat *match_kbd(InterceptionDevice dev) {
    wchar_t hwid[256];
    int have = g_ctx && get_hwid(g_ctx, dev, hwid, 256);
    for (int i = 0; i < g_nseat; i++) {
        if (g_seat[i].kbdId[0]) {
            if (have && wcontains_ci(hwid, g_seat[i].kbdId)) return &g_seat[i];
        } else if (g_seat[i].kbd == dev) {
            return &g_seat[i];
        }
    }
    return NULL;
}
static Seat *match_mouse(InterceptionDevice dev) {
    wchar_t hwid[256];
    int have = g_ctx && get_hwid(g_ctx, dev, hwid, 256);
    for (int i = 0; i < g_nseat; i++) {
        if (g_seat[i].mouseId[0]) {
            if (have && wcontains_ci(hwid, g_seat[i].mouseId)) return &g_seat[i];
        } else if (g_seat[i].mouse == dev) {
            return &g_seat[i];
        }
    }
    return NULL;
}

/* Bound listener on 127.0.0.1:port; retries forever rather than exiting. */
static SOCKET make_listener(int port) {
    for (;;) {
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   (char *)&one, sizeof(one));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons((unsigned short)port);

        if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
            listen(listener, 1) == 0)
            return listener;
        fprintf(stderr, "[router] bind/listen failed on 127.0.0.1:%d; "
                        "retrying in 1s\n", port);
        closesocket(listener);
        Sleep(1000);
    }
}

static int usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s --learn\n"
        "       %s --seat <port> (--kbd <n> | --kbd-id \"<hwid>\")\n"
        "                        (--mouse <n> | --mouse-id \"<hwid>\") [--seat ...]\n"
        "       %s <kbd> <mouse> [port]                  (legacy, one seat)\n"
        "       %s <kbd1> <mouse1> <port1> [...]         (legacy, positional)\n",
        argv0, argv0, argv0, argv0);
    return EXIT_CONFIG;                     /* config error: respawn stops */
}

/* Narrow (UTF-8/ANSI) argv string -> wide, into a fixed seat ID buffer. */
static void set_id_from_arg(wchar_t *dst, size_t cap, const char *src) {
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    if (n == 0) { /* fall back to ANSI if not valid UTF-8 */
        n = MultiByteToWideChar(CP_ACP, 0, src, -1, dst, (int)cap);
    }
    if (n == 0 && cap) dst[0] = 0;
    dst[cap - 1] = 0;
}

int main(int argc, char **argv) {
    int learn = (argc >= 2 && strcmp(argv[1], "--learn") == 0);

    disable_quickedit();

    if (!learn) {
        /* Two accepted forms. Flag form (preferred, supports hardware IDs):
         *   --seat <port> (--kbd n|--kbd-id "s") (--mouse n|--mouse-id "s") ...
         * Legacy positional form (numbers only): <kbd> <mouse> <port> triples. */
        int is_flagged = (argc >= 2 && strncmp(argv[1], "--", 2) == 0);

        if (is_flagged) {
            int cur = -1;                       /* index of seat being filled */
            for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--seat") == 0 && i + 1 < argc) {
                    if (g_nseat >= MAX_SEATS) { return usage(argv[0]); }
                    cur = g_nseat++;
                    g_seat[cur].port = atoi(argv[++i]);
                } else if (cur < 0) {
                    return usage(argv[0]);      /* flag before any --seat */
                } else if (strcmp(argv[i], "--kbd") == 0 && i + 1 < argc) {
                    g_seat[cur].kbd = atoi(argv[++i]);
                } else if (strcmp(argv[i], "--mouse") == 0 && i + 1 < argc) {
                    g_seat[cur].mouse = atoi(argv[++i]);
                } else if (strcmp(argv[i], "--kbd-id") == 0 && i + 1 < argc) {
                    set_id_from_arg(g_seat[cur].kbdId, 256, argv[++i]);
                } else if (strcmp(argv[i], "--mouse-id") == 0 && i + 1 < argc) {
                    set_id_from_arg(g_seat[cur].mouseId, 256, argv[++i]);
                } else {
                    return usage(argv[0]);
                }
            }
            /* Each seat needs a port and (kbd or kbd_id) and (mouse or mouse_id). */
            for (int i = 0; i < g_nseat; i++) {
                int haveK = g_seat[i].kbd   || g_seat[i].kbdId[0];
                int haveM = g_seat[i].mouse || g_seat[i].mouseId[0];
                if (!g_seat[i].port || !haveK || !haveM) return usage(argv[0]);
            }
        } else {
            int n = argc - 1;
            if (n == 2) {                       /* back-compat: default port */
                g_seat[0].kbd   = atoi(argv[1]);
                g_seat[0].mouse = atoi(argv[2]);
                g_seat[0].port  = 56789;
                g_nseat = 1;
            } else if (n >= 3 && n % 3 == 0 && n / 3 <= MAX_SEATS) {
                g_nseat = n / 3;
                for (int i = 0; i < g_nseat; i++) {
                    g_seat[i].kbd   = atoi(argv[1 + 3 * i]);
                    g_seat[i].mouse = atoi(argv[2 + 3 * i]);
                    g_seat[i].port  = atoi(argv[3 + 3 * i]);
                }
            } else {
                return usage(argv[0]);
            }
        }

        for (int i = 0; i < g_nseat; i++) { /* reject overlapping config */
            for (int j = i + 1; j < g_nseat; j++) {
                /* Ports must differ always; numeric devices must differ when
                 * both are numeric (ID-matched seats can't collide numerically). */
                int kbdClash   = g_seat[i].kbd   && g_seat[i].kbd   == g_seat[j].kbd;
                int mouseClash = g_seat[i].mouse && g_seat[i].mouse == g_seat[j].mouse;
                if (g_seat[i].port == g_seat[j].port || kbdClash || mouseClash) {
                    fprintf(stderr, "[router] seats %c and %c share a device "
                                    "or port\n", 'B' + i, 'B' + j);
                    return EXIT_CONFIG;
                }
            }
        }

        /* Input forwarding and the heartbeat are both timing-sensitive; same
         * boost the agent and clamp carry. Not REALTIME -- that starves the
         * console's own DWM/input. Main thread HIGHEST so a real stroke
         * always outranks the heartbeat thread's ABOVE_NORMAL wakeup. */
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        WSADATA wsa;
        while (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "[router] WSAStartup failed; retrying in 1s\n");
            Sleep(1000);
        }
        InitializeCriticalSection(&g_lock);

        for (int i = 0; i < g_nseat; i++) {
            g_seat[i].client   = INVALID_SOCKET;
            g_seat[i].inj_client = INVALID_SOCKET;
            g_seat[i].listener = make_listener(g_seat[i].port);
            CreateThread(NULL, 0, accept_thread, &g_seat[i], 0, NULL);

            /* Inject port = agent port + 1000. Separate listener because the
             * agent accept loop is "newest wins" -- an injector on the agent
             * port displaces the real agent and the seat goes dead. */
            g_seat[i].inj_listener = make_listener(g_seat[i].port + 1000);
            CreateThread(NULL, 0, inject_thread, &g_seat[i], 0, NULL);

            fprintf(stderr, "[router] seat %c: kbd=%d mouse=%d port=%d inject=%d\n",
                    'B' + i, g_seat[i].kbd, g_seat[i].mouse,
                    g_seat[i].port, g_seat[i].port + 1000);
        }
        CreateThread(NULL, 0, heartbeat_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);
        fprintf(stderr, "[router] waiting for agent(s); heartbeat %d ms\n",
                HEARTBEAT_MS);
    } else {
        fprintf(stderr, "[learn] press keys / move each mouse; Ctrl-C to quit\n");
    }

    /* Outer restart loop -- a driver hiccup reinitialises instead of killing
     * the process. Ctrl-C still terminates (default handler). */
    for (;;) {
        InterceptionContext context = interception_create_context();
        if (!context) {
            fprintf(stderr, "[router] interception_create_context failed -- "
                            "is the Interception driver installed (+reboot)? "
                            "retrying in 1s\n");
            Sleep(1000);
            continue;
        }
        interception_set_filter(context, interception_is_keyboard,
                                INTERCEPTION_FILTER_KEY_ALL);
        interception_set_filter(context, interception_is_mouse,
                                INTERCEPTION_FILTER_MOUSE_ALL);
        g_ctx = context;   /* match_kbd/match_mouse query hardware IDs via this */

        InterceptionDevice device;
        InterceptionStroke stroke;

        while (interception_receive(context,
                                    device = interception_wait(context),
                                    &stroke, 1) > 0) {

            if (learn) {
                wchar_t hwid[256];
                int have = get_hwid(context, device, hwid, 256);
                if (interception_is_keyboard(device)) {
                    InterceptionKeyStroke *k = (InterceptionKeyStroke *)&stroke;
                    fprintf(stderr, "keyboard dev=%d  code=%u state=%u\n",
                            device, k->code, k->state);
                } else {
                    InterceptionMouseStroke *m = (InterceptionMouseStroke *)&stroke;
                    fprintf(stderr, "mouse    dev=%d  state=%u flags=%u x=%d y=%d\n",
                            device, m->state, m->flags, m->x, m->y);
                }
                /* Print the STABLE hardware ID once identified -- this is what to
                 * put in seats.toml as kbd_id/mouse_id so the mapping survives
                 * reboots. Convert to UTF-8 and print NARROW: mixing fwprintf
                 * with the fprintf calls above on the same stream is UB in the
                 * Windows CRT (it silently swallowed a diagnostic earlier). */
                if (have) {
                    char utf8[512];
                    int n = WideCharToMultiByte(CP_UTF8, 0, hwid, -1,
                                                utf8, sizeof(utf8), NULL, NULL);
                    if (n > 0) fprintf(stderr, "         hwid: %s\n", utf8);
                    fflush(stderr);
                }
                interception_send(context, device, &stroke, 1); /* stay usable */
                continue;
            }

            if (interception_is_keyboard(device)) {
                InterceptionKeyStroke *k = (InterceptionKeyStroke *)&stroke;
                Seat *st = match_kbd(device);
                if (st) {
                    WireEvent ev = { 'K', k->code, k->state, 0, 0 };
                    forward(st, &ev);            /* capture, do NOT pass on */
                } else {
                    interception_send(context, device, &stroke, 1);  /* seat A */
                }
            } else { /* mouse */
                InterceptionMouseStroke *m = (InterceptionMouseStroke *)&stroke;
                Seat *st = match_mouse(device);
                if (st) {
                    unsigned short a = m->state;
                    if (m->flags & INTERCEPTION_MOUSE_MOVE_ABSOLUTE)
                        a |= WIRE_M_ABS;
                    if (m->flags & INTERCEPTION_MOUSE_VIRTUAL_DESKTOP)
                        a |= WIRE_M_VDESK;
                    /* (short) casts are bit-preserving: absolute coords are
                     * 0..65535 and reinterpreted as unsigned on the agent. */
                    WireEvent ev = { 'M', a,
                                     (unsigned short)m->rolling,
                                     (short)m->x, (short)m->y };
                    forward(st, &ev);
                } else {
                    interception_send(context, device, &stroke, 1);
                }
            }
        }

        fprintf(stderr, "[router] interception loop ended; reinitialising in 1s\n");
        interception_destroy_context(context);
        Sleep(1000);
    }
    /* not reached */
}
