/* Shared limits and argument validation for the daemon and input helpers.
 * This header deliberately has no Windows dependencies. */
#ifndef HYDRA_SEAT_H
#define HYDRA_SEAT_H

#include <stddef.h>

#define HYDRA_MAX_EXTRA_SEATS 4
#define HYDRA_INJECT_PORT_OFFSET 1000
#define HYDRA_MAX_AGENT_PORT (65535 - HYDRA_INJECT_PORT_OFFSET)
#define HYDRA_MAX_SEAT_NAME 32

static inline int hydra_valid_seat_name(const char *name)
{
    size_t n = 0;
    if (!name || !*name) return 0;
    for (; name[n]; ++n) {
        unsigned char c = (unsigned char)name[n];
        if (n >= HYDRA_MAX_SEAT_NAME ||
            !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* Unlike atoi, reject suffixes, negatives and overflow before narrowing. */
static inline int hydra_parse_number(const char *text, int min, int max, int *out)
{
    int value = 0;
    if (!text || !*text || !out || min < 0 || max < min) return 0;
    for (; *text; ++text) {
        int digit = *text - '0';
        if (digit < 0 || digit > 9 || value > max / 10 ||
            (value == max / 10 && digit > max % 10)) return 0;
        value = value * 10 + digit;
    }
    if (value < min) return 0;
    *out = value;
    return 1;
}

static inline int hydra_ports_overlap(int a, int b)
{
    return a == b || a + HYDRA_INJECT_PORT_OFFSET == b ||
           b + HYDRA_INJECT_PORT_OFFSET == a;
}

#endif
