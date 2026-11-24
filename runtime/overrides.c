/* Hand-written replacements for guest functions (see recomp/overrides.txt).
 * Addresses are for Zenonia 1.2.0 (libmerkava.so). */
#include "rt.h"
#include <stdlib.h>

#define G_SCREEN_W  0x100bb908u   /* _screen_width  (game framebuffer) */
#define G_SCREEN_H  0x100bb90cu
#define G_DEVICE_W  0x100bb910u
#define G_DEVICE_H  0x100bb914u

int paragon_screen_w = 320;       /* set by the platform layer before NativeInit */

void HinitPlatformSize(void) {
    WR32(G_SCREEN_W, (uint32_t)paragon_screen_w);
    WR32(G_SCREEN_H, 240);
    WR32(G_DEVICE_W, 480);
    WR32(G_DEVICE_H, 320);
}
