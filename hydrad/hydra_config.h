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
#include <utility>
#include <climits>
#include "../common/hydra_seat.h"
/* <string> above provides std::wstring/std::to_wstring for hydra_build_router_args */

struct SeatCfg {
    std::string name;
    int         kbd     = 0;     /* Interception keyboard device number (1..10) */
    int         mouse   = 0;     /* Interception mouse device number   (11..20) */
    /* Model-ID matching. Use only when a substring identifies exactly one
     * attached device. Identical physical devices share hardware IDs. Numbers
     * distinguish them within a boot but drift after reboot/re-plug; the router
     * checks the complete inventory before enabling capture. Set one or the other. */
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

    /* A/V sync: endpoint-id substring for the seat's monitor. When set, audio is
     * carried over shared memory instead of the RDP channel, which is what makes
     * it land at the same time as the video. Empty = leave audio on RDP. */
    std::string audioBridge;

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

    inline std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    /* Strip a trailing # comment that is not inside a string. */
    inline std::string strip_comment(const std::string& s) {
        bool inS = false, inD = false;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (inD && c == '\\' && i + 1 < s.size()) { ++i; continue; }
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
inline bool hydra_parse_config(const std::string& text, HydraCfg& result, std::string& err)
{
    using namespace hydra_detail;
    HydraCfg out;                  /* do not partially replace a live config */
    err.clear();
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
            else if (key == "kbd" || key == "mouse" || key == "port") {
                int n = 0;
                int min = key == "mouse" ? 11 : 1;
                int max = key == "port" ? HYDRA_MAX_AGENT_PORT : key == "kbd" ? 10 : 20;
                if (quoted || !hydra_parse_number(val.c_str(), min, max, &n)) {
                    err = "line " + std::to_string(lineno) + ": " + key +
                          " must be an integer in " + std::to_string(min) + ".." + std::to_string(max);
                    return false;
                }
                if (key == "kbd") seat.kbd = n;
                else if (key == "mouse") seat.mouse = n;
                else seat.port = n;
            }
            else if (key == "kbd_id")   seat.kbdId = val;    /* stable hardware-ID match */
            else if (key == "mouse_id") seat.mouseId = val;  /* stable hardware-ID match */
            else if (key == "audio_id") seat.audioId = val;  /* render-endpoint id substring */
            else if (key == "audio_prime") seat.audioPrime = val;  /* chime | keepalive | off */
            else if (key == "audio_bridge") seat.audioBridge = val; /* monitor endpoint substr */
            else if (key == "audio_route") seat.audioRoute = val;  /* session-based routing */
            else if (key == "display_mode") seat.displayMode = val;  /* idd | capture */
        }
    }

    /* Validation: each seat needs name/port/monitor, and for BOTH keyboard and
     * mouse it needs EITHER a numeric index OR a hardware-ID string (id preferred
     * -- numbers drift across reboots). */
    if (out.seats.size() > HYDRA_MAX_EXTRA_SEATS) {
        err = "seat_router supports at most " + std::to_string(HYDRA_MAX_EXTRA_SEATS) + " extra seats";
        return false;
    }
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
        const std::string prefix = "seat " + c.name + ": ";
        if (!hydra_valid_seat_name(c.name.c_str())) {
            err = prefix + "name must be 1..32 ASCII letters, digits, underscores or hyphens";
            return false;
        }
        int sessionNumber = 0;
        const bool namedSession = c.session.rfind("user:", 0) == 0 && c.session.size() > 5;
        const bool numberedSession = hydra_parse_number(c.session.c_str(), 1, INT_MAX, &sessionNumber) != 0;
        if (c.session != "auto" && c.session != "console" && !namedSession && !numberedSession) {
            err = prefix + "session must be auto, console, user:NAME or a positive numeric ID";
            return false;
        }
        if (out.seats.size() > 1 && !namedSession && !numberedSession) {
            err = prefix + "multiple extra seats require explicit, distinct user:NAME or numeric sessions";
            return false;
        }
        if ((c.kbd && !c.kbdId.empty()) || (c.mouse && !c.mouseId.empty())) {
            err = prefix + "choose a numeric device OR a hardware ID, not both";
            return false;
        }
        for (const std::string* id : {&c.kbdId, &c.mouseId}) {
            bool invalid = id->size() > 255 || (!id->empty() && id->back() == '\\');
            for (unsigned char ch : *id) if (ch < 32 || ch > 126 || ch == '"') invalid = true;
            if (invalid) {
                err = prefix + "hardware ID is too long or contains unsupported command-line characters";
                return false;
            }
        }
        if (!out.confineMonitor.empty() && lower(c.monitor) == lower(out.confineMonitor)) {
            err = prefix + "monitor is already assigned to the console";
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            const SeatCfg& other = out.seats[j];
            std::string conflict;
            if (lower(c.name) == lower(other.name)) conflict = "name";
            else if (lower(c.monitor) == lower(other.monitor)) conflict = "monitor";
            else if (hydra_ports_overlap(c.port, other.port)) conflict = "agent/injector port";
            else if (c.kbd && c.kbd == other.kbd) conflict = "keyboard";
            else if (c.mouse && c.mouse == other.mouse) conflict = "mouse";
            else if (!c.kbdId.empty() && lower(c.kbdId) == lower(other.kbdId)) conflict = "keyboard hardware ID";
            else if (!c.mouseId.empty() && lower(c.mouseId) == lower(other.mouseId)) conflict = "mouse hardware ID";
            else if (c.session != "auto" && !c.session.empty() && lower(c.session) == lower(other.session))
                conflict = "session";
            else {
                int otherSession = 0;
                if (numberedSession && hydra_parse_number(other.session.c_str(), 1, INT_MAX, &otherSession) &&
                    sessionNumber == otherSession) conflict = "session";
            }
            if (!conflict.empty()) {
                err = prefix + conflict + " conflicts with seat " + other.name;
                return false;
            }
        }
    }
    result = std::move(out);
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

/* The final argument also selects the cursor's shared-memory namespace. */
inline std::wstring hydra_build_agent_args(const SeatCfg& seat)
{
    return L"127.0.0.1 " + std::to_wstring(seat.port) + L" " +
           std::wstring(seat.name.begin(), seat.name.end());
}

#endif /* HYDRA_CONFIG_H */
