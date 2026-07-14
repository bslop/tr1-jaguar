/*
 * video.c - Jaguar display setup, object list, triple buffering
 *
 * Timing/OP construction follow the canonical Jaguar pattern, proven
 * on BigPEmu and real hardware. Do not "fix" the timing without
 * retesting on hardware:
 *   - CONFIG bit 4 selects NTSC (1) / PAL (0) timing
 *   - HDE = (width/2 - 1) | $400, HDB1/2 = hmid - width/2 + 4
 *   - VDE is programmed $FFFF; the computed vde is only for layout
 *   - VI fires at vdb-4, BEFORE the display, so the ISR can rebuild
 *     the object list (the OP destroys it) ahead of the YPOS line;
 *     VI=15 and VI>=515 never fire on HW (see video_init)
 *   - bitmap HEIGHT field is in lines, YPOS in half-lines (BASE_Y*2)
 *   - OLP is written with its 16-bit halves swapped
 *
 * Display: 320x240 16bpp Jaguar RGB (VMODE $6C7), triple buffered.
 */
#include "jaguar.h"
#include "video.h"
#include "blit.h"

/* NTSC/PAL timing constants (jaguar.inc) */
#define NTSC_WIDTH  1409
#define NTSC_HMID   823
#define NTSC_HEIGHT 241
#define NTSC_VMID   266
#define PAL_WIDTH   1381
#define PAL_HMID    843
#define PAL_HEIGHT  287
#define PAL_VMID    322

#define BASE_X 16
#define BASE_Y 16

/* FB8: 8bpp indexed framebuffer + OP CLUT (for Blitter hardware texturing);
 * else RGB16 direct. Switchable so the RGB16 builds are untouched. */
#ifdef FB8
typedef uint8_t fbpix;
#define SCREEN_PWIDTH ((RENDER_W * 1) / 8)   /* 8bpp: 40 phrases/line */
#define OP_DEPTH      (3u << 12)             /* OBDEPTH 3 = 8bpp        */
#else
typedef uint16_t fbpix;
#define SCREEN_PWIDTH ((RENDER_W * 2) / 8)   /* 16bpp: 80 phrases/line */
#define OP_DEPTH      (4u << 12)             /* OBDEPTH 4 = 16bpp       */
#endif

/* Display buffers are DISPLAY_H tall (240). In HALFRES they hold the line-
 * doubled image; otherwise they are the render buffers directly. */
static fbpix fb0[RENDER_W * DISPLAY_H] __attribute__((aligned(16)));
static fbpix fb1[RENDER_W * DISPLAY_H] __attribute__((aligned(16)));
#ifdef HALFRES
/* HALFRES never triple-buffers the display (render target is rbuf; the
   flip ping-pongs fb0/fb1 — the fb2 fallback in video_flip is unreachable
   because flip waits for pending==0 first). Alias fb2 to fb0: frees 76.8KB
   of BSS, which the 68k STACK needs (sp=0x200000 grows down into BSS;
   cold-boot smash 2026-07-12). */
#define fb2 fb0
#else
static fbpix fb2[RENDER_W * DISPLAY_H] __attribute__((aligned(16)));
#endif
#ifdef HALFRES
/* HALFRES renders here (320x120); video_flip line-doubles it into a display buf. */
static fbpix rbuf[RENDER_W * RENDER_H] __attribute__((aligned(16)));
#endif

/* startup.S's crash beacon paints these on a 68k exception so a
 * hardware crash decodes from a camera capture. */
fbpix *const crash_fbs[3] = { fb0, fb1, fb2 };

static uint32_t op_list[16] __attribute__((aligned(16)));
static uint16_t a_vdb_g, a_vde_g;   /* active vertical window (VC half-lines) */

volatile uint32_t frame_count;

static fbpix *draw_buf;               /* CPU renders here                    */
static uint32_t front_fb;             /* what the OP displays                */
static volatile uint32_t pending_fb;  /* buffer the ISR should show, 0=none  */

/* FAST OP-LIST REPAIR (plain unscaled object only). The OP destroys ONLY
 * phrase 0 of the bitmap object (data ptr / ypos / height) as it draws; the
 * probe showed the full 8-long rebuild chronically finishing at VC 33-35 —
 * PAST the object fetch at 32 — under render-time bus starvation (~10-20%
 * bus service). The ISR now restores just the two destroyed longs from
 * values PRECOMPUTED outside the ISR (flip values prepared in video_flip). */
static volatile uint32_t op_fix0, op_fix1;     /* phrase 0 for front_fb  */
static volatile uint32_t pend_fix0, pend_fix1; /* phrase 0 for pending_fb */
static uint32_t op_link;                       /* link field, set at build */

#if defined(LOWRES) && !defined(HALFRES)
/* LOWRES: the 320x120 framebuffer is displayed at 320x240 by the OP hardware
 * VERTICAL scaler.  The object is a TYPE-1 SCALED BITMAP (3 phrases + STOP):
 *   ph0 [0,1]  DATA | LINK | (HEIGHT-1)<<14 | YPOS<<4 | TYPE=1
 *   ph1 [2,3]  IWIDTH/DWIDTH | PITCH=1 | DEPTH=16bpp | XPOS   (full 320 width)
 *   ph2 [4,5]  SCALE: high long = REMAINDER(0); low long = (VSCALE<<8)|HSCALE
 *   [6,7]      STOP
 * The scale word is 8.5 fixed: 0x20 = 1.0x, 0x40 = 2.0x.
 *   HSCALE = 0x20 (1.0x) - MANDATORY: horizontal OP scaling starves the bus
 *            ~15-20x and blacks the display; the fb is kept full 320 width.
 *   VSCALE = 0x40 (2.0x) - 120 source lines -> 240 displayed lines.
 * QUIRK (HW-verified): nothing may follow a scaled object but STOP
 * (a LINK to any overlay object poisons the display), so LINK points at STOP.
 * HEIGHT field = source lines - 1 (the scaled-object convention).  YPOS and the
 * VDB/VDE window are UNCHANGED from the 240 build, so the 120->240 scaled image
 * fills the exact same vertical window the 240-line bitmap did. */
#define FS_HSCALE 0x20u   /* 1.0x  - no horizontal scaling (full 320 width)      */
#define FS_VSCALE 0x40u   /* 2.0x  - 120 source lines fill the 240-line window   */
/* #define LOWRES_DIAG_PLAIN 1 -- ISOLATION TEST (HW-verified 2026-07-08): with
 * this defined, LOWRES displays the 320x120 fb as a PLAIN bitmap (no scale) and
 * the room renders CORRECTLY in the top 120 lines -> kernel+fb are FINE; the
 * ONLY blocker is the TYPE-1 scaled object below. Re-enable to re-confirm. */
static void build_object_list(uint32_t fb_addr)
{
    /* A scaled (TYPE 1) object must be reached via BRANCH fall-through inside
     * the active window, else the OP blacks out (HW-verified). So:
     * BRANCH(VC>a_vde->STOP), BRANCH(VC<a_vdb->STOP), SCALED BITMAP, STOP. */
#ifdef LOWRES_DIAG_PLAIN
    /* DIAGNOSTIC: display the 320x120 fb as a PLAIN unscaled bitmap (top 120
     * lines). If the room shows here, the kernel/fb are fine and the bug is
     * purely the scaled object. Remove once isolated. */
    uint32_t link = ((uint32_t)&op_list[4]) >> 3;
    op_list[0] = (fb_addr << 8) | (link >> 8);
    op_list[1] = (link << 24) | ((uint32_t)RENDER_H << 14) | ((uint32_t)BASE_Y << 4);
    op_list[2] = SCREEN_PWIDTH >> 4;
    op_list[3] = ((uint32_t)SCREEN_PWIDTH << 28) | ((uint32_t)SCREEN_PWIDTH << 18)
               | (1u << 15) | OP_DEPTH | BASE_X;
    op_list[4] = 0;
    op_list[5] = 4;
#else
    /* FIX (HW-verified 2026-07-08): a BARE scaled object (scaled bitmap ->
     * STOP, NO branch gating) is correct. My earlier BRANCH
     * gating (VC vs a_vdb/a_vde) was masking the ENTIRE display -> black. The
     * scaled object as the first/only object works; the room renders scaled
     * 120->240. (a_vdb_g/a_vde_g now unused by LOWRES; kept harmless.) */
    uint32_t link = ((uint32_t)&op_list[6]) >> 3;   /* STOP at op_list[6] */

    op_list[0] = (fb_addr << 8) | (link >> 8);
    op_list[1] = (link << 24)
               | ((uint32_t)(RENDER_H - 1) << 14)   /* source lines - 1 (scaled) */
               | ((uint32_t)BASE_Y << 4)            /* YPOS                       */
               | 1u;                                /* TYPE 1 = scaled bitmap     */

    op_list[2] = SCREEN_PWIDTH >> 4;
    op_list[3] = ((uint32_t)SCREEN_PWIDTH << 28)
               | ((uint32_t)SCREEN_PWIDTH << 18)
               | (1u << 15)                         /* PITCH 1 */
               | OP_DEPTH                         /* DEPTH 16bpp */
               | BASE_X;

    op_list[4] = 0;                                 /* SCALE: REMAINDER = 0 */
    op_list[5] = ((uint32_t)FS_VSCALE << 8) | (uint32_t)FS_HSCALE;

    op_list[6] = 0;                                 /* STOP */
    op_list[7] = 4;
#endif
}
#else
static void build_object_list(uint32_t fb_addr)
{
    uint32_t link = ((uint32_t)&op_list[4]) >> 3;

    op_list[0] = (fb_addr << 8) | (link >> 8);
    op_list[1] = (link << 24)
               | ((uint32_t)DISPLAY_H << 14)       /* on-screen height (240) */
               | ((uint32_t)BASE_Y << 4);

    op_list[2] = SCREEN_PWIDTH >> 4;
    op_list[3] = ((uint32_t)SCREEN_PWIDTH << 28)
               | ((uint32_t)SCREEN_PWIDTH << 18)
               | (1u << 15)                       /* PITCH 1 */
               | OP_DEPTH                       /* DEPTH 16bpp */
               | BASE_X;

    op_list[4] = 0;                               /* STOP */
    op_list[5] = 4;

    op_link = link;
    op_fix0 = op_list[0];
    op_fix1 = op_list[1];
}
#endif

#ifdef SKUNK_CONSOLE
#include "skunkdbg.h"
void video_dump_oplist(void)
{
    int i;
    dbg_kx("olp", (uint32_t)op_list);
    dbg_kx("vdb", a_vdb_g);
    dbg_kx("vde", a_vde_g);
    for (i = 0; i < 12; i++)
        dbg_kx("op", op_list[i]);
}
#else
void video_dump_oplist(void) {}
#endif

static void point_op_at_list(void)
{
    uint32_t olp = (uint32_t)op_list;
    OLP = (olp >> 16) | (olp << 16);
    OBF = 0;
}

/* Called from the asm stub in startup.S at every vertical interrupt,
 * which fires just before the display field starts. */
void vblank_handler(void)
{
    uint32_t pf = pending_fb;
#if !(defined(LOWRES) && !defined(HALFRES))
    /* fast path: restore ONLY the OP-destroyed phrase — 2 stores, no calls.
       Flip values were precomputed in video_flip, outside the ISR. */
    if (pf) {
        op_list[0] = pend_fix0;
        op_list[1] = pend_fix1;
        op_fix0 = pend_fix0;
        op_fix1 = pend_fix1;
        front_fb = pf;
        pending_fb = 0;
    } else {
        op_list[0] = op_fix0;
        op_list[1] = op_fix1;
    }
    OBF = 0;
#else
    /* scaled object: OP also destroys the scale-remainder phrase — keep the
       full rebuild (fast repair not implemented for this path) */
    if (pf) {
        front_fb = pf;
        pending_fb = 0;
    }
    build_object_list(front_fb);
    point_op_at_list();
#endif
    frame_count++;
}

void video_init(void)
{
    uint32_t i;
    int ntsc = (CONFIG & 0x10) != 0;
    uint16_t width  = ntsc ? NTSC_WIDTH  : PAL_WIDTH;
    uint16_t hmid   = ntsc ? NTSC_HMID   : PAL_HMID;
    uint16_t height = ntsc ? NTSC_HEIGHT : PAL_HEIGHT;
    uint16_t vmid   = ntsc ? NTSC_VMID   : PAL_VMID;
    uint16_t a_vdb;

    for (i = 0; i < RENDER_W * DISPLAY_H; i++) {
        fb0[i] = 0;
        fb1[i] = 0;
        fb2[i] = 0;
    }
#ifdef HALFRES
    for (i = 0; i < RENDER_W * RENDER_H; i++) rbuf[i] = 0;
    draw_buf = rbuf;               /* render into the half-height buffer */
#else
    draw_buf = fb1;
#endif
    front_fb = (uint32_t)fb0;
    pending_fb = 0;
    frame_count = 0;

    /* active vertical window (VC half-lines) - set BEFORE the first build so
     * the LOWRES scaled-object branches gate correctly (also used per-field). */
    a_vdb_g = (uint16_t)(vmid - height);
    a_vde_g = (uint16_t)(vmid + height);

    build_object_list(front_fb);
    point_op_at_list();

    /* horizontal window */
    HDE  = (uint16_t)((width / 2 - 1) | 0x400);
    HDB1 = (uint16_t)(hmid - width / 2 + 4);
    HDB2 = (uint16_t)(hmid - width / 2 + 4);

    /* vertical window: program VDB, leave VDE wide open */
    a_vdb = a_vdb_g;
    VDB = a_vdb;
    VDE = 0xFFFF;

    BG    = 0;
    BORD1 = 0;
    BORD2 = 0;

    /* VI just before display start, ISR vector, unmask */
    {
        extern void vblank_stub(void);
        JAG_AUTOVEC = (uint32_t)vblank_stub;
    }
    /* 2 scanlines before display start. The ISR must rebuild the OP list
     * (the OP destroys it every field) between here and the object's first
     * fetch at VC=2*BASE_Y — a hard ~350us deadline on the 68k. HW-probed
     * limits (2026-07-11): VI=15 AND VI>=515 NEVER FIRE (dead-black screen,
     * ISR dead, flip spins forever) — only this narrow pre-display band
     * works, so the deadline cannot be moved. Bus-storm collisions with
     * this window are instead dodged at the SOURCE (video_wait_safe_vc()
     * before the Jerry room-transform kick in main.c). */
    VI = (uint16_t)(a_vdb - 4);
    INT1 = 0x0003;              /* enable VIDEO + GPU interrupts (STOP-sync) */
    __asm__ volatile ("move.w #0x2000,%sr");

    /* RGB16, CSYNC, BGEN, VIDEN, PWIDTH=4 -> the standard 320-wide mode */
    VMODE = 0x06C7;
}

void *video_backbuffer(void)
{
    return draw_buf;
}

/* Load a 256-entry RGB16 palette into the OP CLUT (for FB8 8bpp mode). */
void video_set_clut(const uint16_t *pal)
{
    volatile uint16_t *clut = (volatile uint16_t *)0xF00400u;
    int i;
    for (i = 0; i < 256; i++)
        clut[i] = pal[i];
}

void video_flip(void)
{
    uint32_t shown;

    while (pending_fb)
        ;
    shown = front_fb;
#ifdef HALFRES
    /* line-double the 320x120 render buffer into a free 320x240 display buffer
     * (neither shown nor queued), then queue that for the next vblank. The
     * render buffer (draw_buf == rbuf) is fixed. */
    {
        fbpix *db = ((uint32_t)fb0 != shown) ? fb0
                  : ((uint32_t)fb1 != shown) ? fb1 : fb2;
        blit_double(rbuf, db, RENDER_H);
        pend_fix0 = ((uint32_t)db << 8) | (op_link >> 8);
        pend_fix1 = (op_link << 24)
                  | ((uint32_t)DISPLAY_H << 14)
                  | ((uint32_t)BASE_Y << 4);
        pending_fb = (uint32_t)db;
    }
#else
    {
        fbpix *done = draw_buf;
        /* rotate to the buffer that is neither queued nor on screen */
        if ((uint32_t)fb0 != (uint32_t)done && (uint32_t)fb0 != shown)
            draw_buf = fb0;
        else if ((uint32_t)fb1 != (uint32_t)done && (uint32_t)fb1 != shown)
            draw_buf = fb1;
        else
            draw_buf = fb2;
        /* precompute the ISR's phrase-0 repair values here (main-loop time)
           so the flip path in the ISR is also just 2 stores */
        pend_fix0 = ((uint32_t)done << 8) | (op_link >> 8);
        pend_fix1 = (op_link << 24)
                  | ((uint32_t)DISPLAY_H << 14)
                  | ((uint32_t)BASE_Y << 4);
        pending_fb = (uint32_t)done;
    }
#endif
}

/* HI-RES path (title screens): paint 320x240 directly into a display
   buffer, bypassing rbuf + the doubling blit. Same ISR flip protocol. */
void *video_backbuffer_hi(void)
{
    uint32_t shown = front_fb, pend = pending_fb;
    if ((uint32_t)fb0 != shown && (uint32_t)fb0 != pend) return fb0;
    if ((uint32_t)fb1 != shown && (uint32_t)fb1 != pend) return fb1;
    return fb2;
}

void video_flip_hi(void *db)
{
    while (pending_fb)
        ;
    pend_fix0 = ((uint32_t)db << 8) | (op_link >> 8);
    pend_fix1 = (op_link << 24)
              | ((uint32_t)DISPLAY_H << 14)
              | ((uint32_t)BASE_Y << 4);
    pending_fb = (uint32_t)db;
}

/* debug: expose all three buffers so heartbeat markers can paint into
   whichever is DISPLAYED (a frozen frame's progress stays visible) */
void *video_fb_n(int i)
{
    return i == 0 ? (void *)fb0 : i == 1 ? (void *)fb1 : (void *)fb2;
}

/* Jerry's room-transform kick unleashes a multi-ms posted-write storm on the
 * DRAM bus with priority over the 68k. If the storm covers the vblank ISR's
 * hard window (VI at VC 21/31 through the object fetch + rebuild, VC ~32-44),
 * the OP walks the consumed list and the top 2-3 scanlines drop for a field —
 * the periodic "bounce" (HW-captured 2026-07-11, flip frames only). Call
 * before the kick: if the beam is near the window, spin until it passes
 * (<=1.1ms, hit on ~7% of frames; Tom is idle here so the poll is cheap). */
void video_wait_safe_vc(void)
{
    /* block starts in [0,48] too close AFTER the wrap, AND in [400,wrap]
       too close BEFORE it — a ~2ms Blitter fill started at VC 5 straddled
       the window (probe caught ISR exits at VC 80) */
    for (;;) {
        uint16_t v = VC;
        if (v > 48 && v < 400)
            break;
    }
}

void video_wait_vblank(void)
{
    uint32_t f = frame_count;
    while (frame_count == f)
        ;
}
