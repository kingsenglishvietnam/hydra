/* test_config.c(pp) -- native unit test for hydra_config.h. */
#include "../hydrad/hydra_config.h"
#include <cstdio>
#include <cassert>
#include "../common/hydra_ipc.h"
#include "../common/hydra_session.h"
#include "../hydrad/hydra_config_json.h"

static int fails = 0;
#define CHECK(c, m) do{ if(c){printf("  ok   %s\n",m);} else {printf("  FAIL %s\n",m);++fails;} }while(0)

static std::string seat(const char* name, int kbd, int mouse, int port, int monitor,
                        const char* extra = "") {
    return "[[seat]]\nname='" + std::string(name) + "'\nkbd=" + std::to_string(kbd) +
           "\nmouse=" + std::to_string(mouse) + "\nport=" + std::to_string(port) +
           "\nmonitor='\\\\.\\DISPLAY" + std::to_string(monitor) + "'\n" +
           "session='user:seat-" + name + "'\n" + extra;
}

static void rejects(const std::string& text, const char* message) {
    HydraCfg out; std::string err;
    CHECK(!hydra_parse_config(text, out, err) && !err.empty(), message);
}

int main() {
    puts("== hydra_config unit test ==\n");

    const std::string cfg =
        "# Hydra seats\n"
        "[hostA]\n"
        "confine_monitor = '\\\\.\\DISPLAY1'   # console cursor stays here\n"
        "\n"
        "[[seat]]\n"
        "name    = \"B\"\n"
        "kbd     = 2\n"
        "mouse   = 12\n"
        "port    = 56789\n"
        "monitor = '\\\\.\\DISPLAY2'\n"
        "session = \"user:student1\"\n"
        "edid    = \"1920x1080@60\"\n"
        "\n"
        "[[seat]]\n"
        "name    = \"C\"\n"
        "kbd     = 3\n"
        "mouse   = 13\n"
        "port    = 56790\n"
        "monitor = '\\\\.\\DISPLAY3'\n"
        "session = \"user:student2\"\n"
        "edid    = \"2560x1440@60\"\n";

    HydraCfg out; std::string err;
    bool ok = hydra_parse_config(cfg, out, err);
    if (!ok) printf("  parse error: %s\n", err.c_str());
    CHECK(ok, "parses cleanly");
    CHECK(out.confineMonitor == "\\\\.\\DISPLAY1", "hostA confine_monitor = \\\\.\\DISPLAY1");
    CHECK(out.seats.size() == 2, "two seats");

    if (out.seats.size() == 2) {
        const SeatCfg& b = out.seats[0];
        CHECK(b.name == "B", "seat B name");
        CHECK(b.kbd == 2 && b.mouse == 12 && b.port == 56789, "seat B kbd/mouse/port");
        CHECK(b.monitor == "\\\\.\\DISPLAY2", "seat B monitor (literal backslashes)");
        CHECK(b.session == "user:student1", "seat B has an explicit user session");
        CHECK(b.edid == "1920x1080@60", "seat B edid");

        const SeatCfg& c = out.seats[1];
        CHECK(c.name == "C" && c.port == 56790, "seat C name/port");
        CHECK(c.session == "user:student2", "seat C session=user:student2");
        CHECK(c.edid == "2560x1440@60", "seat C edid");
    }

    /* rejects [seat] (must be [[seat]]) */
    { HydraCfg o; std::string e;
      CHECK(!hydra_parse_config("[seat]\nname='X'\n", o, e), "rejects [seat] singular"); }

    /* rejects incomplete seat */
    { HydraCfg o; std::string e;
      bool r = hydra_parse_config("[[seat]]\nname=\"Z\"\nkbd=1\n", o, e);
      CHECK(!r, "rejects seat missing mouse/port/monitor");
      if (!r) printf("       (got expected error: %s)\n", e.c_str()); }

    /* The production router now uses explicit flags for numeric/ID selectors. */
    {
        std::wstring ra = hydra_build_router_args(out);
        bool okArgs = (ra == L"--seat 56789 --kbd 2 --mouse 12 --seat 56790 --kbd 3 --mouse 13");
        CHECK(okArgs, "hydra_build_router_args uses flagged groups");
        if (!okArgs) wprintf(L"       got: [%ls]\n", ra.c_str());
    }

    const std::string b = seat("B", 2, 12, 56789, 2, "session='user:seat-b'\n");
    const std::string c = seat("C", 3, 13, 56790, 3, "session='user:seat-c'\n");
    const std::string d = seat("D", 4, 14, 56791, 4, "session='user:seat-d'\n");
    {
        HydraCfg four; std::string e;
        CHECK(hydra_parse_config(b + c + d, four, e) && four.seats.size() == 3,
              "four local users = console plus three configured seats");
        CHECK(hydra_build_agent_args(four.seats[1]) == L"127.0.0.1 56790 C",
              "seat C agent receives its own cursor identity");
        CHECK(hydra_build_agent_args(four.seats[2]) == L"127.0.0.1 56791 D",
              "seat D agent receives its own cursor identity");
        wchar_t bn[128], cn[128], dn[128];
        hydra_pixels_name(bn, 128, four.seats[0].name.c_str());
        hydra_pixels_name(cn, 128, four.seats[1].name.c_str());
        hydra_pixels_name(dn, 128, four.seats[2].name.c_str());
        CHECK(wcscmp(bn, cn) && wcscmp(bn, dn) && wcscmp(cn, dn),
              "three agents address distinct shared cursor mappings");
        CHECK(hydra_parse_config(b, four, e) && four.seats.size() == 1,
              "reusing output replaces instead of appending seats");
        CHECK(!hydra_parse_config(c + "invalid\n", four, e) && four.seats.size() == 1 &&
              four.seats[0].name == "B", "failed reload preserves previous config");
    }
    rejects(b + seat("b", 3, 13, 56790, 3), "reject case-insensitive duplicate seat names");
    rejects(b + seat("C", 3, 13, 56790, 2), "reject duplicate physical monitors");
    rejects("[hostA]\nconfine_monitor='\\\\.\\display2'\n" + b, "reject console monitor reuse");
    rejects(b + seat("C", 3, 13, 56789, 3), "reject duplicate agent ports");
    rejects(b + seat("C", 3, 13, 57789, 3), "reject collision with another seat's injector port");
    rejects(b + seat("C", 3, 13, 55789, 3), "reject reversed injector collision");
    rejects(b + seat("C", 2, 13, 56790, 3), "reject shared numeric keyboard");
    rejects(b + seat("C", 3, 12, 56790, 3), "reject shared numeric mouse");
    rejects(b + seat("C", 3, 13, 56790, 3, "session='user:SEAT-B'\n"), "reject same explicit user session");
    rejects(b + c + "session='auto'\n", "multiple extra seats cannot guess a shared session");
    rejects(b + c + "session='console'\n", "multiple extra seats cannot target console session");
    rejects(b + "session='user:'\n", "reject empty session username");
    rejects(b + "session='2junk'\n", "reject malformed numeric session");
    rejects(b + "session='2'\n" + c + "session='02'\n", "reject equivalent numeric session IDs");
    rejects(seat("../C", 2, 12, 56789, 2), "reject unsafe IPC seat name");
    rejects(seat("C", 11, 12, 56789, 2), "reject mouse-class index as keyboard");
    rejects(seat("C", 2, 10, 56789, 2), "reject keyboard-class index as mouse");
    rejects(seat("C", 2, 12, 64536, 2), "reserve port space for injector");
    rejects(b + "port=56789junk\n", "reject numeric suffix instead of atoi truncation");
    rejects(b + "port=999999999999999999999999\n", "reject integer overflow");
    rejects(b + "port=-1\n", "reject negative port");
    rejects(b + "port='56789'\n", "reject quoted port");
    rejects(b + "kbd_id='VID_1234'\n", "reject mixing numeric and model-ID keyboard selectors");
    rejects(b + c + d + seat("E", 5, 15, 56792, 5) + seat("F", 6, 16, 56793, 6),
            "reject more seats than the router can accept");
    const std::string ids = "[[seat]]\nname='B'\nkbd_id='VID_1234'\nmouse_id='VID_5678'\n"
                            "port=56789\nmonitor='\\\\.\\DISPLAY2'\nsession='user:seat-b'\n";
    {
        HydraCfg parsed; std::string e;
        CHECK(hydra_parse_config(ids, parsed, e), "ID-based selectors still supported");
        CHECK(hydra_build_router_args(parsed) ==
              L"--seat 56789 --kbd-id \"VID_1234\" --mouse-id \"VID_5678\"",
              "ID arguments remain quoted");
    }
    rejects(ids + "[[seat]]\nname='C'\nkbd_id='vid_1234'\nmouse_id='OTHER'\n"
                  "port=56790\nmonitor='\\\\.\\DISPLAY3'\nsession='user:seat-c'\n", "reject shared model-ID selectors");
    rejects(ids + "kbd_id='A" + std::string(1, '\0') + "B'\n", "reject embedded NUL instead of truncating device ID");
    CHECK(hydra_json_string("a\\b\"c\n") == "\"a\\\\b\\\"c\\u000a\"", "JSON escapes paths, quotes and controls");
    {
        HydraCfg boundary; std::string e;
        CHECK(hydra_parse_config(seat("Z", 10, 20, 64535, 9), boundary, e), "valid numeric upper boundaries");
    }
    CHECK(hydra_auto_session({{1, true, true}}, 1) == 0xFFFFFFFFul,
          "automatic session never falls back to console");
    CHECK(hydra_auto_session({{1, true, true}, {2, true, true}}, 1) == 2,
          "automatic session chooses a single active remote user");
    CHECK(hydra_auto_session({{2, true, true}, {3, true, true}}, 1) == 0xFFFFFFFFul,
          "automatic session refuses multiple active remote users");
    CHECK(hydra_auto_session({{0, true, true}, {2, false, true}, {3, true, false}}, 1) == 0xFFFFFFFFul,
          "service, disconnected and unauthenticated sessions are ignored");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails==1?"":"s");
    return fails ? 1 : 0;
}
