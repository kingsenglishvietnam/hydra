#ifndef HYDRA_MOUSE_BUTTONS_H
#define HYDRA_MOUSE_BUTTONS_H

/* Wire button bits are physical left/right. Apply the destination session's
 * preference, preserving wheel, absolute-motion and auxiliary-button bits. */
static inline unsigned short hydra_mouse_button_state(unsigned short state, int swapped)
{
    if (!swapped) return state;
    return (unsigned short)((state & ~0x00Fu) |
                            ((state & 0x003u) << 2) |
                            ((state & 0x00Cu) >> 2));
}

#endif
