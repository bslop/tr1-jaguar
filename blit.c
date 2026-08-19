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

/* ☠️ FALSE-IDLE SETTLE ARM (user, 2026-08-18: "is there a flush needed before
 * the video starts?").  The Blitter does not assert BUSY the instant B_CMD is
 * written, so the FIRST B_CMD read after the write can return IDLE while the
 * copy has not started.  blit_copy_phrase believes that read and returns 1;
 * the caller then flips a buffer the Blitter is still filling and the Object
 * Processor displays it mid-copy.  cobweb b8dd333 added a BUSY settle window
 * to jagemu for exactly this hazard, and run 3 named it as one of the two
 * surviving candidates for the FMV grain.
 *   BLITSET = 0   ship behaviour: believe the first IDLE
 *   BLITSET = N   require N CONSECUTIVE IDLE reads before believing it
 * A `volatile const` so an arm is ONE IMMEDIATE and the layout never moves -
 * the same trick JVDECMASK uses, so one A10 pad roll covers both arms. */
#ifndef BLITSET
#define BLITSET 0
#endif
volatile const uint32_t g_blitset = BLITSET;


/* HANGDIAG: every wait in the 68k boot path is unbounded, so a hang anywhere
   looks identical from the outside - a black screen.  Under HANGDIAG each one
   gets a budget and, on timeout, paints a DISTINCT border colour and halts, so
   a single photograph names which wait never completed.  Deliberately touches
   no file that main.c compiles into: main.o stays byte-identical to the build
   under test, which is essential because this fault is codegen-sensitive. */
#ifdef HANGDIAG
#define HANGDIAG_HALT(col) do { \
    *(volatile uint16_t *)0xF00058u = (uint16_t)(col); \
    for (;;) ; } while (0)
#endif

static void blit_wait(void)
{
#ifdef HANGDIAG
    uint32_t g = 0;
    while (!(B_CMD & BLIT_IDLE)) {
        g++;
        if (g > 8000000u) HANGDIAG_HALT(0xFFE0);   /* YELLOW: blit_wait */
    }
#else
    while (!(B_CMD & BLIT_IDLE))
        ;
#endif
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
#if defined(PHRASECLEAR) && defined(FB8)
    /* ★★★ PHRASE-MODE CLEAR. The per-frame in-game clear is the single most
       expensive Blitter shape in the level: `jagemu --blit-histogram
       --pc-histogram --fidelity silicon` at f1500 puts 320x80 srcen=no at
       21.2% of ALL transfer ticks, 16,916 ticks/frame - more than the next
       seven shapes combined. It ran with BLIT_XPIX (pixel X-add), i.e. ONE
       BYTE PER TICK, while the machine can fill a PHRASE (8 bytes) per tick.
       Phrase addressing is XADDCTRL 0 - simply DROPPING BLIT_XPIX - which is
       exactly what blit_copy_phrase does, and that path is silicon-verified
       (it was the 95ms -> 5ms video lever). B_COUNT stays in PIXELS either
       way, so only the flag changes.
       ☠️ DECLINES RATHER THAN GUESSES, like blit_bytes: phrase mode needs an
       8-aligned row start. Rows are RENDER_W (320) apart and 320 % 8 == 0, so
       every row inherits the base's alignment - but if the base is not phrase
       aligned we fall through to the byte path instead of running the Blitter
       away over DRAM (the first phrase build came back black and looked
       exactly like an A10 miss). */
    if ((((uint32_t)fb) & 7u) == 0) {
        A1_FLAGS = pixflag | BLIT_WID320;        /* XADDCTRL 0 = XADDPHR */
    } else {
        A1_FLAGS = pixflag | BLIT_WID320 | BLIT_XPIX;
    }
#else
    A1_FLAGS = pixflag | BLIT_WID320 | BLIT_XPIX;
#endif
    A1_PIXEL = ((uint32_t)y0 << 16);
    A1_STEP  = (1u << 16) | ((uint32_t)(-RENDER_W) & 0xFFFFu);
    B_SRCD   = cc;
    B_SRCD1  = cc;
    B_COUNT  = ((uint32_t)(y1 - y0) << 16) | RENDER_W;
    B_CMD    = BLIT_UPDA1 | BLIT_LFU_REP;
}

/* blit_fill_rect: solid rectangle, x0..x0+w-1 by y0..y0+h-1.
 *
 * blit_band fills WHOLE rows, so anything narrower than the screen was being
 * painted with 68000 byte stores - both loading bars were written that way
 * (user, 2026-08-09: "what did I say about the 68000?").  Same A1 setup as
 * blit_band with the x origin and the inner count bounded, so it inherits a
 * path already proven on silicon rather than inventing a new one.
 */
void blit_fill_rect(void *fb, int x0, int y0, int w, int h, uint32_t c)
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
    if (w <= 0 || h <= 0)
        return;
    blit_wait();
    A1_BASE  = (uint32_t)fb;
    A1_FLAGS = pixflag | BLIT_WID320 | BLIT_XPIX;
    A1_PIXEL = ((uint32_t)y0 << 16) | (uint32_t)x0;
    A1_STEP  = (1u << 16) | ((uint32_t)(-w) & 0xFFFFu);
    B_SRCD   = cc;
    B_SRCD1  = cc;
    B_COUNT  = ((uint32_t)h << 16) | (uint32_t)w;
    B_CMD    = BLIT_UPDA1 | BLIT_LFU_REP;
}

/* blit_copy: full-width Blitter image copy, src -> dst, `h` rows.
 * Replaces the 68k byte loop that repainted the title art every frame
 * (76800 x `moveb (a0)+,(a1)+`; 21.9% of all awake 68k cycles in a boot
 * trace). Same A1/A2 setup as blit_double, without the row doubling.
 * This is the "Blitter composite" the title-screen TODO asked for. */
void blit_copy(const void *src, void *dst, int h)
{
    uint32_t xreset = ((uint32_t)(-RENDER_W)) & 0xFFFFu;
    blit_wait();
    A1_BASE  = (uint32_t)src;
    A1_FLAGS = BLIT_PIX8 | BLIT_WID320 | BLIT_XPIX;
    A1_PIXEL = 0;
    A1_STEP  = (1u << 16) | xreset;
    A2_BASE  = (uint32_t)dst;
    A2_FLAGS = BLIT_PIX8 | BLIT_WID320 | BLIT_XPIX;
    A2_PIXEL = 0;
    A2_STEP  = (1u << 16) | xreset;
    B_COUNT  = ((uint32_t)h << 16) | RENDER_W;
    B_CMD    = BLIT_CMD_COPY | BLIT_UPDA1 | BLIT_UPDA2;
    blit_wait();
}

/* PHRASE-mode full-width 8bpp copy.  blit_copy above addresses ONE PIXEL per
 * inner step (XADDPIX), so a whole 320x240 frame is 76800 read/write pairs
 * ping-ponging between two DRAM regions - measured at 40ms/frame in the video
 * player, the largest cost left after the 68k loop it replaced.  XADDCTRL
 * value 0 is XADDPHR: eight bytes per step, an eighth of the DRAM page churn.
 * 320 bytes is exactly 40 phrases per row and both buffers are 16-aligned, so
 * the alignment preconditions hold exactly.
 * Returns 0 if the Blitter never went idle - the wait is BOUNDED here because
 * a wrong phrase-mode setup must report itself, not wedge the console behind
 * the unbounded spin every other entry point uses. */
int blit_copy_phrase(const void *src, void *dst, int h)
{
    uint32_t xreset = ((uint32_t)(-RENDER_W)) & 0xFFFFu;
    uint32_t g;
    blit_wait();
    A1_BASE  = (uint32_t)src;
    A1_FLAGS = BLIT_PIX8 | BLIT_WID320;          /* XADDCTRL 0 = XADDPHR */
    A1_PIXEL = 0;
    A1_STEP  = (1u << 16) | xreset;
    A2_BASE  = (uint32_t)dst;
    A2_FLAGS = BLIT_PIX8 | BLIT_WID320;
    A2_PIXEL = 0;
    A2_STEP  = (1u << 16) | xreset;
    B_COUNT  = ((uint32_t)h << 16) | RENDER_W;
    B_CMD    = BLIT_CMD_COPY | BLIT_UPDA1 | BLIT_UPDA2;
    if (g_blitset) {
        uint32_t run = 0;
        for (g = 0; g < 4000000u; g++) {
            if (B_CMD & BLIT_IDLE) {
                if (++run >= g_blitset)
                    return 1;
            } else {
                run = 0;                 /* a BUSY read resets the window */
            }
        }
        return 0;
    }
    for (g = 0; g < 4000000u; g++)
        if (B_CMD & BLIT_IDLE)
            return 1;
    return 0;
}

/* blit_bytes: LINEAR block move, src -> dst, in PHRASE mode.
 *
 * WHY (user, 2026-08-08): "the 68000 is the enemy - boot code runs on it,
 * everything else is Tom and Jerry."  The video player was still carrying two
 * bulk moves on the 68k every frame - the stream-buffer compaction and the
 * audio chunk copy into the DSP ring - and the 68k is the slowest thing on the
 * machine, an order of magnitude worse than the hardware that exists to do
 * exactly this.
 *
 * It reuses the PROVEN 320-wide geometry rather than inventing a new Blitter
 * setup: a contiguous block is just N rows of 320 bytes, and the row step in
 * blit_copy_phrase already advances exactly one row, so consecutive rows are
 * contiguous.  A mis-set Blitter runs away over DRAM (the first phrase-mode
 * build came back black and looked identical to an A10 miss), so this
 * DECLINES rather than guesses: it returns 0 unless both ends are 8-aligned,
 * and the caller keeps its own tail path for the last <320 bytes.
 *
 * Overlap: the Blitter copies in INCREASING address order, so a move DOWN
 * (src above dst) is safe - which is the only direction compaction uses.
 */
unsigned blit_bytes(const void *src, void *dst, unsigned n)
{
    unsigned rows = n / RENDER_W;
    if (rows == 0)
        return 0;
    if ((((uint32_t)src | (uint32_t)dst) & 7u) != 0)
        return 0;                       /* phrase mode needs 8-alignment */
    if (!blit_copy_phrase(src, dst, (int)rows))
        return 0;                       /* Blitter never idled - caller falls
                                           back; never silently half-move */
    return rows * RENDER_W;
}

/* blit_rect: sub-rectangle Blitter copy between two RENDER_W-wide 8bpp
 * images (same layout both sides). The title's dirty-rect repaint uses
 * this to restore the art behind a moving ring item without erasing the
 * statically-cached items around it (2026-08-06). Offsets ride in the
 * PIXEL registers so the phrase-aligned buffer bases stay legal. */
void blit_rect(const void *src, void *dst, int x0, int y0, int w, int h)
{
    uint32_t xreset = ((uint32_t)(-w)) & 0xFFFFu;
    if (w <= 0 || h <= 0)
        return;
    blit_wait();
    A1_BASE  = (uint32_t)src;
    A1_FLAGS = BLIT_PIX8 | BLIT_WID320 | BLIT_XPIX;
    A1_PIXEL = ((uint32_t)y0 << 16) | (uint32_t)(x0 & 0xFFFF);
    A1_STEP  = (1u << 16) | xreset;
    A2_BASE  = (uint32_t)dst;
    A2_FLAGS = BLIT_PIX8 | BLIT_WID320 | BLIT_XPIX;
    A2_PIXEL = ((uint32_t)y0 << 16) | (uint32_t)(x0 & 0xFFFF);
    A2_STEP  = (1u << 16) | xreset;
    B_COUNT  = ((uint32_t)h << 16) | (uint32_t)w;
    B_CMD    = BLIT_CMD_COPY | BLIT_UPDA1 | BLIT_UPDA2;
    blit_wait();
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
