#ifndef HYDRA_SESSION_H
#define HYDRA_SESSION_H

#include <vector>

struct HydraSessionCandidate {
    unsigned long id;
    bool active;
    bool hasUser;
};

/* An automatic seat cannot guess between users or fall back to the console. */
inline unsigned long hydra_auto_session(const std::vector<HydraSessionCandidate>& sessions,
                                       unsigned long console)
{
    unsigned long found = 0xFFFFFFFFul;
    for (const auto& s : sessions) {
        if (!s.active || !s.hasUser || s.id == 0 || s.id == console) continue;
        if (found != 0xFFFFFFFFul) return 0xFFFFFFFFul;
        found = s.id;
    }
    return found;
}

#endif
