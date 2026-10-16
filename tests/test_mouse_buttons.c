#include "../input/hydra_mouse_buttons.h"
#include <stdio.h>

int main(void)
{
    if (hydra_mouse_button_state(0x001, 0) != 0x001 ||
        hydra_mouse_button_state(0x002, 0) != 0x002 ||
        hydra_mouse_button_state(0x004, 0) != 0x004 ||
        hydra_mouse_button_state(0x008, 0) != 0x008) return 1;
    if (hydra_mouse_button_state(0x001, 1) != 0x004 ||
        hydra_mouse_button_state(0x002, 1) != 0x008 ||
        hydra_mouse_button_state(0x004, 1) != 0x001 ||
        hydra_mouse_button_state(0x008, 1) != 0x002) return 2;
    /* Exhaust all wire states: swapping is reversible and affects only four bits. */
    for (unsigned int state = 0; state <= 0xFFFFu; ++state) {
        unsigned short swapped = hydra_mouse_button_state((unsigned short)state, 1);
        if ((swapped & ~0xFu) != (state & ~0xFu) ||
            hydra_mouse_button_state(swapped, 1) != state) return 3;
    }
    puts("PASS: primary buttons follow session preference; other wire bits preserved");
    return 0;
}
