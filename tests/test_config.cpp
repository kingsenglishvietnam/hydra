/* test_config.c(pp) -- native unit test for hydra_config.h. */
#include "../hydrad/hydra_config.h"
#include <cstdio>
#include <cassert>

static int fails = 0;
#define CHECK(c, m) do{ if(c){printf("  ok   %s\n",m);} else {printf("  FAIL %s\n",m);++fails;} }while(0)

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
        "session = \"auto\"\n"
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
        CHECK(b.session == "auto", "seat B session=auto");
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

    /* router args: flat "<kbd> <mouse> <port>" triples, seat_router's contract */
    {
        std::wstring ra = hydra_build_router_args(out);
        bool okArgs = (ra == L"2 12 56789 3 13 56790");
        CHECK(okArgs, "hydra_build_router_args -> '2 12 56789 3 13 56790'");
        if (!okArgs) wprintf(L"       got: [%ls]\n", ra.c_str());
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails==1?"":"s");
    return fails ? 1 : 0;
}
