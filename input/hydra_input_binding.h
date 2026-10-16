/* Resolve physical input ownership before enabling Interception filters.
 * A hardware ID identifies a MODEL, not necessarily an individual device.
 * Never resolve ambiguity by choosing the first seat or device in a list. */
#ifndef HYDRA_INPUT_BINDING_H
#define HYDRA_INPUT_BINDING_H

#include <wchar.h>
#include <wctype.h>
#include "../common/hydra_seat.h"

typedef struct {
    int number;                 /* explicit Interception device, or zero */
    const wchar_t *id;          /* hardware-ID substring, or empty */
} HydraInputSelector;

typedef struct {
    int number;
    const wchar_t *id;          /* nonempty only for an enumerated device */
} HydraInputDevice;

enum HydraBindingResult {
    HYDRA_BIND_OK,
    HYDRA_BIND_INVALID,
    HYDRA_BIND_MISSING,
    HYDRA_BIND_AMBIGUOUS,
    HYDRA_BIND_SHARED
};

static inline int hydra_id_contains(const wchar_t *hay, const wchar_t *needle)
{
    if (!hay || !needle || !*needle) return 0;
    for (; *hay; ++hay) {
        const wchar_t *a = hay, *b = needle;
        while (*a && *b && towlower(*a) == towlower(*b)) { ++a; ++b; }
        if (!*b) return 1;
    }
    return 0;
}

/* On failure, bindings remain zero; badSeat identifies the offending selector.
 * Numeric selectors deliberately remain boot-specific. They must be learned
 * again after device enumeration changes; they are not persistent identities. */
static inline int hydra_resolve_inputs(
    const HydraInputSelector *selectors, int count,
    const HydraInputDevice *devices, int deviceCount,
    int first, int last, int *bindings, int *badSeat)
{
    int resolved[HYDRA_MAX_EXTRA_SEATS] = {0};
    int i, j;
    if (badSeat) *badSeat = -1;
    if (!selectors || !devices || !bindings || count < 1 ||
        count > HYDRA_MAX_EXTRA_SEATS || deviceCount < 0) return HYDRA_BIND_INVALID;
    for (i = 0; i < count; ++i) bindings[i] = 0;
    for (i = 0; i < count; ++i) {
        const HydraInputSelector *s = &selectors[i];
        int byId = s->id && s->id[0];
        int matches = 0;
        if (badSeat) *badSeat = i;
        if ((byId && s->number) || (!byId && (s->number < first || s->number > last)))
            return HYDRA_BIND_INVALID;
        for (j = 0; j < deviceCount; ++j) {
            const HydraInputDevice *d = &devices[j];
            if (d->number < first || d->number > last || !d->id || !d->id[0]) continue;
            if (byId ? hydra_id_contains(d->id, s->id) : d->number == s->number) {
                resolved[i] = d->number;
                ++matches;
            }
        }
        if (!matches) return HYDRA_BIND_MISSING;
        if (matches != 1) return HYDRA_BIND_AMBIGUOUS;
        for (j = 0; j < i; ++j)
            if (resolved[j] == resolved[i]) return HYDRA_BIND_SHARED;
    }
    for (i = 0; i < count; ++i) bindings[i] = resolved[i];
    if (badSeat) *badSeat = -1;
    return HYDRA_BIND_OK;
}

#endif
