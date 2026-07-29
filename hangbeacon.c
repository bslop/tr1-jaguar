/* hangbeacon.c — a boot beacon that CANNOT be invisible.
 *
 * 2026-07-29. The previous attempt at this painted the BG register and read
 * black every time — including on a build that demonstrably executed the
 * markers and booted. BG is not visible on this display path: the OP's object
 * covers the whole active area, and the pillarbox in a capture is blanking,
 * not background. Three conclusions were drawn off that dead instrument and
 * had to be withdrawn.
 *
 * This one takes the display over instead of writing into it:
 *   - points the OP at a list containing NOTHING BUT A STOP object, so the
 *     object processor draws no pixels at all, and
 *   - turns on BGEN with the wanted colour, so the background fills every
 *     active line.
 * That works whether or not video_init has run — it does not depend on the
 * framebuffers existing, on the CLUT being loaded, or on the vblank ISR.
 *
 * It then SPINS. The colour on screen is the last milestone reached.
 *
 * *** VALIDATE IT ON A BUILD THAT BOOTS BEFORE TRUSTING A NEGATIVE. ***
 * BEACON_AT=1 does exactly that: it fires at the top of video_init, which any
 * booting build reaches. If that build does not go solid, the beacon is broken
 * and every "no colour" reading below is meaningless.
 */
#include <stdint.h>
#include "jaguar.h"

/* 16-byte aligned: the OP fetches object phrases, and OLP is a phrase address */
static uint32_t bcn_list[8] __attribute__((aligned(16)));

void hang_beacon(uint16_t colour);

void hang_beacon(uint16_t colour)
{
    uint32_t olp;

    /* A STOP object (type 4) and nothing else. */
    bcn_list[0] = 0;
    bcn_list[1] = 4;

    olp = (uint32_t)bcn_list;
    OLP = (olp >> 16) | (olp << 16);   /* OLP is word-swapped */
    OBF = 0;

    BG    = colour;
    BORD1 = colour;
    BORD2 = colour;

    /* RGB16, CSYNC, BGEN, VIDEN, PWIDTH=4 — same mode video_init sets, so the
       raster stays valid whether or not video_init has already run. */
    VMODE = 0x06C7;

    for (;;)
        ;
}
