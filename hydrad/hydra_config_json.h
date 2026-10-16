#ifndef HYDRA_CONFIG_JSON_H
#define HYDRA_CONFIG_JSON_H

#include "hydra_config.h"
#include <sstream>

inline std::string hydra_json_string(const std::string& text)
{
    std::string out = "\"";
    static const char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '\\' || c == '"') { out += '\\'; out += (char)c; }
        else if (c < 32) {
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += (char)c;
    }
    return out + "\"";
}

/* One schema for offline validation and the daemon's actually applied config. */
inline std::string hydra_config_json(const HydraCfg& cfg)
{
    std::ostringstream out;
    out << "{\"console_monitor\":" << hydra_json_string(cfg.confineMonitor) << ",\"seats\":[";
    for (size_t i = 0; i < cfg.seats.size(); ++i) {
        const auto& s = cfg.seats[i];
        if (i) out << ',';
        out << "{\"name\":" << hydra_json_string(s.name)
            << ",\"session\":" << hydra_json_string(s.session)
            << ",\"monitor\":" << hydra_json_string(s.monitor)
            << ",\"port\":" << s.port << ",\"kbd\":" << s.kbd
            << ",\"mouse\":" << s.mouse
            << ",\"kbd_id\":" << hydra_json_string(s.kbdId)
            << ",\"mouse_id\":" << hydra_json_string(s.mouseId)
            << ",\"display_mode\":" << hydra_json_string(s.displayMode)
            << ",\"edid\":" << hydra_json_string(s.edid)
            << ",\"audio_prime\":" << hydra_json_string(s.audioPrime)
            << ",\"audio_bridge\":" << hydra_json_string(s.audioBridge)
            << ",\"audio_id\":" << hydra_json_string(s.audioId)
            << ",\"audio_route\":" << hydra_json_string(s.audioRoute) << '}';
    }
    out << "]}";
    return out.str();
}

#endif
