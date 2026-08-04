/* hydra_config.h  --  minimal TOML-subset parser for seats.toml.
 *
 * Deliberately tiny: this config has one [hostA] table and N [[seat]] tables with
 * scalar keys. We do not pull a TOML library for that. Supports:
 *   - [table] and [[array-of-table]] headers
 *   - key = value  with integer, double-quoted (escapes \\ \" \n \t) and
 *     single-quoted literal (no escapes — good for \\.\DISPLAY2) strings
 *   - # comments (whole-line and trailing) and blank lines
 *
 * Header-only and free of Windows types so it is unit-tested natively (tests/).
 */
#ifndef HYDRA_CONFIG_H
#define HYDRA_CONFIG_H

#include <string>
#include <vector>
#include <cstdlib>
#include <cctype>
/* <string> above provides std::wstring/std::to_wstring for hydra_build_router_args */

struct SeatCfg {
    std::string name;
    int         kbd     = 0;     /* Interception keyboard device number (1..10) */
    int         mouse   = 0;     /* Interception mouse device number   (11..20) */
    /* Stable hardware-ID matching (preferred). Interception device NUMBERS drift
     * across reboots/re-plugs; the hardware-ID string does not. If these are set
     * (via kbd_id/mouse_id in seats.toml), seat_router matches on a substring of
     * the device's hardware ID instead of the volatile number. Numbers remain
     * supported for backward compatibility -- set one or the other. */
    std::string kbdId;           /* substring of the keyboard's hardware ID */
    std::string mouseId;         /* substring of the mouse's hardware ID    */
    int         port    = 0;     /* loopback TCP port to the seat's agent       */
    std::string monitor;         /* physical panel device name, e.g. \\.\DISPLAY2 */
    std::string session = "auto";/* "console" | "auto" | "<id>" | "user:NAME"   */
    std::string edid    = "1920x1080@60";
    /* Audio endpoint priming. The seat's Remote Audio endpoint goes bad when
     * idle -- the FIRST app to open it gets silence, a second app works, and
     * then the first one does too. Two ways to avoid being first:
     *   "chime"     (default) play one short sound when the session comes up.
     *               Cheap, no extra process. Sufficient IF the endpoint only
     *               goes bad once per session.
     *   "keepalive" hold a silent stream open for the whole session, so it can
     *               never go idle. Costs one small process per seat.
     *   "off"       do nothing.
     * Restarting Audiosrv is NOT an option here: it was tried from four
     * different contexts and fixes nothing, because a restart leaves the
     * endpoint idle and simply moves the problem to the next opener. */
    std::string audioPrime = "chime";

    std::string audioId;         /* optional: substring of the render-endpoint id
                                  * to route THIS seat's audio to (e.g. the
                                  * monitor's speakers). Empty => no audio agent. */
    std::string audioRoute;      /* optional: like audioId but uses SESSION-BASED
                                  * routing via session_route from session 0 --
                                  * captures ALL of this seat's session audio and
                                  * renders to the named endpoint. The robust path. */
    std::string displayMode;     /* "" or "idd" = iddseat virtual-monitor producer
                                  * (default). "capture" = session_capture (DDA of
                                  * the real session desktop) -> no RDP window. */
};

struct HydraCfg {
    std::string           confineMonitor;   /* [hostA] confine_monitor */
    std::vector<SeatCfg>  seats;
};

namespace hydra_detail {

    inline std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) ++a;
        while (b > a && std::isspace((unsigned char)s[b-1])) --b;
        return s.substr(a, b - a);
    }

    /* Strip a trailing # comment that is not inside a string. */
    inline std::string strip_comment(const std::string& s) {
        bool inS = false, inD = false;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '\'' && !inD) inS = !inS;
            else if (c == '"' && !inS) inD = !inD;
            else if (c == '#' && !inS && !inD) return s.substr(0, i);
        }
        return s;
    }

    /* Parse a value token into a string (quotes removed, escapes handled) or,
     * for bare tokens, leave as-is (caller decides int vs string). */
    inline std::string parse_value_string(const std::string& tok, bool& wasQuoted) {
        wasQuoted = false;
        std::string t = trim(tok);
        if (t.size() >= 2 && t.front() == '\'' && t.back() == '\'') {
            wasQuoted = true;
            return t.substr(1, t.size() - 2);          /* literal: no escapes */
        }
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
            wasQuoted = true;
            std::string out; std::string body = t.substr(1, t.size() - 2);
            for (size_t i = 0; i < body.size(); ++i) {
                if (body[i] == '\\' && i + 1 < body.size()) {
                    char n = body[++i];
                    out.push_back(n == 'n' ? '\n' : n == 't' ? '\t' : n);
                } else out.push_back(body[i]);
            }
            return out;
        }
        return t;                                        /* bare token */
    }
}

/* Returns true on success. On failure, fills `err` with a line-numbered reason. */
inline bool hydra_parse_config(const std::string& text, HydraCfg& out, std::string& err)
{
    using namespace hydra_detail;
    enum { NONE, HOST, SEAT } ctx = NONE;
    int lineno = 0;
    std::string line;
    size_t pos = 0;

    auto nextline = [&](std::string& dst) -> bool {
        if (pos >= text.size()) return false;
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) { dst = text.substr(pos); pos = text.size(); }
        else { dst = text.substr(pos, nl - pos); pos = nl + 1; }
        if (!dst.empty() && dst.back() == '\r') dst.pop_back();
        return true;
    };

    while (nextline(line)) {
        ++lineno;
        std::string s = trim(strip_comment(line));
        if (s.empty()) continue;

        if (s == "[[seat]]") { out.seats.emplace_back(); ctx = SEAT; continue; }
        if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
            std::string name = trim(s.substr(1, s.size() - 2));
            if (name == "hostA") ctx = HOST;
            else if (name == "seat") { err = "line " + std::to_string(lineno) +
                        ": use [[seat]] (array of tables), not [seat]"; return false; }
            else ctx = NONE;                             /* unknown table: ignore */
            continue;
        }

        size_t eq = s.find('=');
        if (eq == std::string::npos) {
            err = "line " + std::to_string(lineno) + ": expected key = value"; return false;
        }
        std::string key = trim(s.substr(0, eq));
        bool quoted = false;
        std::string val = parse_value_string(s.substr(eq + 1), quoted);

        if (ctx == HOST) {
            if (key == "confine_monitor") out.confineMonitor = val;
        } else if (ctx == SEAT) {
            if (out.seats.empty()) { err = "line " + std::to_string(lineno) +
                        ": seat key before any [[seat]]"; return false; }
            SeatCfg& seat = out.seats.back();
            if      (key == "name")     seat.name = val;
            else if (key == "monitor")  seat.monitor = val;
            else if (key == "session")  seat.session = val;
            else if (key == "edid")     seat.edid = val;
            else if (key == "kbd")      seat.kbd = std::atoi(val.c_str());
            else if (key == "mouse")    seat.mouse = std::atoi(val.c_str());
            else if (key == "kbd_id")   seat.kbdId = val;    /* stable hardware-ID match */
            else if (key == "mouse_id") seat.mouseId = val;  /* stable hardware-ID match */
            else if (key == "audio_id") seat.audioId = val;  /* render-endpoint id substring */
            else if (key == "audio_prime") seat.audioPrime = val;  /* chime | keepalive | off */
            else if (key == "audio_route") seat.audioRoute = val;  /* session-based routing */
            else if (key == "display_mode") seat.displayMode = val;  /* idd | capture */
            else if (key == "port")     seat.port = std::atoi(val.c_str());
        }
    }

    /* Validation: each seat needs name/port/monitor, and for BOTH keyboard and
     * mouse it needs EITHER a numeric index OR a hardware-ID string (id preferred
     * -- numbers drift across reboots). */
    for (size_t i = 0; i < out.seats.size(); ++i) {
        const SeatCfg& c = out.seats[i];
        bool haveKbd   = (c.kbd   != 0) || !c.kbdId.empty();
        bool haveMouse = (c.mouse != 0) || !c.mouseId.empty();
        if (c.name.empty() || !haveKbd || !haveMouse || c.port == 0 || c.monitor.empty()) {
            err = "seat #" + std::to_string(i + 1) +
                  " (" + (c.name.empty() ? "?" : c.name) +
                  "): needs name, (kbd or kbd_id), (mouse or mouse_id), port, monitor";
            return false;
        }
    }
    return true;
}

/* Build the seat_router argument string. Each seat becomes a flagged group so
 * numeric indices and hardware-ID strings are unambiguous on the command line:
 *
 *   --seat <port> (--kbd <n> | --kbd-id "<hwid substr>")
 *                 (--mouse <n> | --mouse-id "<hwid substr>")
 *
 * Hardware-ID form is preferred (stable across reboots); numeric form is kept
 * for backward compatibility. IDs are quoted since hardware-ID strings can
 * contain spaces. Returned wide since it becomes a Windows command line; free
 * of Windows types so it stays unit-testable natively alongside the parser. */
inline std::wstring hydra_build_router_args(const HydraCfg& cfg)
{
    auto widen = [](const std::string& s) {
        return std::wstring(s.begin(), s.end());
    };
    std::wstring a;
    for (size_t i = 0; i < cfg.seats.size(); ++i) {
        const SeatCfg& s = cfg.seats[i];
        if (i) a += L" ";
        a += L"--seat " + std::to_wstring(s.port);
        if (!s.kbdId.empty())  a += L" --kbd-id \""  + widen(s.kbdId)  + L"\"";
        else                   a += L" --kbd "       + std::to_wstring(s.kbd);
        if (!s.mouseId.empty()) a += L" --mouse-id \"" + widen(s.mouseId) + L"\"";
        else                    a += L" --mouse "     + std::to_wstring(s.mouse);
    }
    return a;
}

#endif /* HYDRA_CONFIG_H */