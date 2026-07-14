/*
 * blit.c - Tom Blitter raster backend (68k never touches pixels).
 *
 * Proven on real hardware: XPIX
 * pixel-addressing, LFU set to pass B_SRCD through, wait-for-idle
 * BEFORE touching registers (never after start). Blitter B_CMD/flag
 * values come from jaguar.h, validated against the official SDK
 * JAGUAR.INC (UPDA1=$200, LFU_REPLACE=$01800000).
 */
#include "jaguar.h"
#include "video.h"
#include "blit.h"

#define SPAN_CPU_LIMIT 12

static void blit_wait(void)
{
    while (!(B_CMD & BLIT_IDLE))
        ;
}

void blit_span(uint16_t *fb, int y, int x0, int x1, uint16_t c)
{
    int n = x1 - x0 + 1;
    uint32_t cc;

    if (n <= 0)
        return;

    if (n <= SPAN_CPU_LIMIT) {
        uint16_t *p = fb + y * RENDER_W + x0;
        blit_wait();                 /* the Blitter may still own this row */
        while (n--)
            *p++ = c;
        return;
    }

    cc = ((uint32_t)c << 16) | c;
    blit_wait();
    A1_BASE  = (uint32_t)fb;
    A1_FLAGS = BLIT_PIX16 | BLIT_WID320 | BLIT_XPIX;
    A1_PIXEL = ((uint32_t)y << 16) | (uint32_t)x0;
    B_SRCD   = cc;
    B_SRCD1  = cc;
    B_COUNT  = (1u << 16) | (uint32_t)n;
    B_CMD    = BLIT_LFU_REP;
}

void blit_band(void *fb, int y0, int y1, uint32_t c)
{
#ifdef FB8
    uint8_t  idx = (uint8_t)c;
    uint32_t cc  = idx | (idx << 8) | (idx << 16) | (idx << 24);
    uint32_t pixflag = BLIT_PIX8;
#else
    uint16_t c16 = (uint16_t)c;
    uint32_t cc  = ((uint32_t)c16 << 16) | c16;
    uint32_t pixflag = BLIT_PIX16;
#endif
    if (y1 <= y0)
        return;

    blit_wait();
    A1_BASE  = (uint32_t)fb;
    A1_FLAGS = pixflag | BLIT_WID320 | BLIT_XPIX;
    A1_PIXEL = ((uint32_t)y0 << 16);
    A1_STEP  = (1u << 16) | ((uint32_t)(-RENDER_W) & 0xFFFFu);
    B_SRCD   = cc;
    B_SRCD1  = cc;
    B_COUNT  = ((uint32_t)(y1 - y0) << 16) | RENDER_W;
    B_CMD    = BLIT_UPDA1 | BLIT_LFU_REP;
}

#ifdef HALFRES
/* Line-double copy: src (RENDER_W x srch, 8bpp) -> dst (RENDER_W x 2*srch).
 * Each source row k is written to dst rows 2k and 2k+1 (two Blitter copy
 * passes: even lines, then odd).  Uses the SAME mem->mem copy the textured
 * kernel uses (SRCEN|LFU_REPLACE|DSTA2): A1 = source (read), A2 = dest.
 * This lets us render at half height (half the fill) but display through the
 * proven unscaled 240-line path (the OP scaler can't survive heavy fill). */
void blit_double(const void *src, void *dst, int srch)
{
    uint32_t xreset = ((uint32_t)(-RENDER_W)) & 0xFFFFu;
    int pass;
    for (pass = 0; pass < 2; pass++) {
        blit_wait();
        A1_BASE  = (uint32_t)src;                         /* source (read)     */
        A1_FLAGS = BLIT_PIX8 | BLIT_WID320 | BLIT_XPIX;
        A1_PIXEL = 0;
        A1_STEP  = (1u << 16) | xreset;                   /* src +1 line/row   */
        A2_BASE  = (uint32_t)dst;                         /* dest (written)    */
        A2_FLAGS = BLIT_PIX8 | BLIT_WID320 | BLIT_XPIX;
        A2_PIXEL = (uint32_t)pass << 16;                  /* dst y = 0 or 1    */
        A2_STEP  = (2u << 16) | xreset;                   /* dst +2 lines/row  */
        B_COUNT  = ((uint32_t)srch << 16) | RENDER_W;
        B_CMD    = BLIT_CMD_COPY | BLIT_UPDA1 | BLIT_UPDA2;
    }
    blit_wait();
}
#endif
