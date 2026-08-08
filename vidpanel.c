/*
 * vidpanel.c - the SELF-CALIBRATING read-out panel, shared by vidrom and the
 * game's boot video block.
 *
 * WHY THIS EXISTS AS A SHARED FILE (2026-08-08): the game's video block had
 * its own ad-hoc VIDDIAG markers - 8px squares, no border, no calibration
 * word, painted over moving video - and reading them off a capture produced
 * TWO wrong diagnoses in one afternoon ("Tom never starts", then "gpu_ok is
 * 0", the latter contradicted by the game rendering its level at all). The
 * panel below draws its own reference frame, so the host decoder recovers the
 * exact grid instead of guessing, and row 0 is a fixed A5A5A5A5 word: if that
 * does not decode, the decoder reports NOTHING rather than confident noise.
 *
 * Geometry is identical to vidrom's so scratchpad/readout.py reads both
 * without changes:
 *   panel 160x234 at fb (0,0), 2px white border
 *   8 lamps, 16x10 at x=4, y=4+i*12
 *   12 bit rows, 32 bits, 4px per bit, 8px tall, at x=24, y=102+i*11
 */
#include "jaguar.h"
#include "vidpanel.h"

#define MK_OFF 254
#define MK_ON  255
#define PAN_W  160
#define PAN_H  234
#define BITX   24
#define BITW   4
#define BITY0  102
#define BITDY  11
#define BITH   8
#define L_ON   0xFFFFFFFFu
#define L_OFF  ((uint32_t)MK_OFF * 0x01010101u)

/* Monotonic half-line clock (~31.7us). VC wraps every field, so fold in
   frame_count - which the vblank ISR bumps once per field - to get a running
   tick. frame_count alone is far too coarse to attribute a 67ms frame. */
uint32_t vp_tick(void)
{
    extern volatile uint32_t frame_count;
    uint32_t f = frame_count;
    uint32_t v = VC;
    if (frame_count != f) { f = frame_count; v = VC; }
    return f * 525u + (v & 0x3FFu);
}

/* The CLUT belongs to whatever clip is playing, so entries 254/255 are the
   only two we own; re-force them every paint. */
void vp_clut(void)
{
    volatile uint16_t *cl = (volatile uint16_t *)0xF00400u;
    cl[MK_OFF] = 0x0000;
    cl[MK_ON]  = 0xFFFE;
}

static void border(uint8_t *fb)
{
    int y, x;
    for (y = 0; y < PAN_H; y++) {
        uint32_t *r = (uint32_t *)(fb + y * 320);
        if (y < 2 || y >= PAN_H - 2) {
            for (x = 0; x < PAN_W / 4; x++)
                r[x] = L_ON;
        } else {
            r[0] = L_ON;
            r[PAN_W / 4 - 1] = L_ON;
        }
    }
}

/* No background fill: every cell paints BOTH states, so nothing stale
   survives and the panel costs a quarter of what a clear-then-draw does.
   Cells are 4px wide at 4-aligned x precisely so one LONG store covers one
   row of one cell - byte stores are the 68k's worst case under OP load. */
void vp_paint(uint8_t *fb, const uint8_t *lamps, const uint32_t *rows)
{
    int i, b, y, x;
    border(fb);
    for (i = 0; i < 8; i++) {
        uint32_t v = lamps[i] ? L_ON : L_OFF;
        int y0 = 4 + i * 12;
        for (y = y0; y < y0 + 10; y++) {
            uint32_t *r = (uint32_t *)(fb + y * 320 + 4);
            for (x = 0; x < 4; x++)
                r[x] = v;
        }
    }
    for (i = 0; i < 12; i++) {
        uint32_t val = (i == 0) ? 0xA5A5A5A5u : rows[i];
        int y0 = BITY0 + i * BITDY;
        for (y = y0; y < y0 + BITH; y++) {
            uint32_t *r = (uint32_t *)(fb + y * 320 + BITX);
            for (b = 0; b < 32; b++)
                r[b] = ((val >> (31 - b)) & 1) ? L_ON : L_OFF;
        }
    }
}
