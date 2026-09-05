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
/* privileged-instruction wrappers (cpu68k.S) — inline asm is outside
   jcc68k's language subset */
void cpu_irq_on(void);
void cpu_stop_sleep(void);
int  cpu_stop_unless(volatile uint32_t *addr, uint32_t val);

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

/* XPOS is an OP LINE-BUFFER index, so it counts PIXEL CLOCKS, not framebuffer
   pixels.  Widening the pixel clock halves that index space. */
#if defined(HRESN) && (HRESN == 160)
#define BASE_X 8
#else
#define BASE_X 16
#endif

/* ☠☠ VMODE IS RE-ASSERTED IN TWO PLACES (video_init and video_rearm_irq, the
   latter reached from gpu_sync via gpu.c).  Hard-coding it in one and not the
   other makes the picture silently double in width mid-session.  One symbol. */
#if defined(HRESN) && (HRESN == 160)
#define JAG_VMODE 0x0EC7      /* PWIDTH 7 - [HW] jag_quake ships this */
#else
#define JAG_VMODE 0x06C7      /* PWIDTH 3 - the standard 320 mode      */
#endif
#define BASE_Y 16

/* FB8: 8bpp indexed framebuffer + OP CLUT (for Blitter hardware texturing);
 * else RGB16 direct. Switchable so the RGB16 builds are untouched. */
#ifdef FB8
typedef uint8_t fbpix;
#define SCREEN_PWIDTH ((RENDER_W * 1) / 8)   /* 8bpp: 40 phrases/line (STRIDE) */
#define SCREEN_IWIDTH ((VIEW_W   * 1) / 8)   /* 8bpp: what the OP FETCHES     */
#define OP_DEPTH      (3u << 12)             /* OBDEPTH 3 = 8bpp        */
#else
typedef uint16_t fbpix;
#define SCREEN_PWIDTH ((RENDER_W * 2) / 8)   /* 16bpp: 80 phrases/line (STRIDE)*/
#define SCREEN_IWIDTH ((VIEW_W   * 2) / 8)   /* 16bpp: what the OP FETCHES     */
#define OP_DEPTH      (4u << 12)             /* OBDEPTH 4 = 16bpp       */
#endif

/* Display buffers are DISPLAY_H tall (240). In HALFRES they hold the line-
 * doubled image; otherwise they are the render buffers directly. */
/* FLIPASM (2026-07-27): the publish half of the flip protocol moves to
 * cpu68k.S so its ORDER is fixed in the instruction stream rather than left
 * to a compiler.  The ISR half is already assembler (startup.S) for the same
 * reason.  These few symbols must be visible to it; every other build keeps
 * them file-local exactly as before. */
#ifdef FLIPASM
#define FLIPSTATIC
#else
#define FLIPSTATIC static
#endif

/* Buffers are 240-tall even in LOWRES (task #4): the TITLE phase displays
 * them as plain 320x240 (video_set_disp240), the game uses rows 0..119 via
 * the scaled object. +172KB BSS, verified against the 0x200000 stack. */
#if defined(LOWRES) && !defined(HALFRES)
#define FB_ALLOC_H 240
#else
#define FB_ALLOC_H DISPLAY_H
#endif
FLIPSTATIC fbpix fb0[RENDER_W * FB_ALLOC_H] __attribute__((aligned(16)));
FLIPSTATIC fbpix fb1[RENDER_W * FB_ALLOC_H] __attribute__((aligned(16)));
#ifdef HALFRES
/* HALFRES never triple-buffers the display (render target is rbuf; the
   flip ping-pongs fb0/fb1 — the fb2 fallback in video_flip is unreachable
   because flip waits for pending==0 first). Alias fb2 to fb0: frees 76.8KB
   of BSS, which the 68k STACK needs (sp=0x200000 grows down into BSS;
   cold-boot smash 2026-07-12). */
#define fb2 fb0
#else
FLIPSTATIC fbpix fb2[RENDER_W * FB_ALLOC_H] __attribute__((aligned(16)));
#endif
#ifdef HALFRES
/* HALFRES renders here (320x120); video_flip line-doubles it into a display buf. */
static fbpix rbuf[RENDER_W * RENDER_H] __attribute__((aligned(16)));
#endif

/* startup.S's crash beacon paints these on a 68k exception so a
 * hardware crash decodes from a camera capture. */
fbpix *const crash_fbs[3] = { fb0, fb1, fb2 };

/* The scaled (LOWRES) path repairs the OP list from ASSEMBLY in startup.S's
 * vblank stub — video.c is compiled by jcc68k, which emits ~10 instructions per
 * store, far too slow to beat the OP's object fetch at VC 32. Those builds must
 * therefore export the state the stub touches; every other build keeps it
 * file-local exactly as before. */
/* 2026-08-02: startup.S's exc_catch references op_list UNCONDITIONALLY, so the
 * old "file-local except in LOWRES" rule no longer holds - a full-height
 * (non-LOWRES, 320x240) build failed to link with 8 undefined references to
 * op_list.  Export it in every configuration; dropping `static` costs nothing. */
#define OPSTATIC          /* exported to startup.S in all builds */


/* DEFINED BY THE LINKER SCRIPT (jaguar.ld), 32-byte aligned by construction. */
extern uint32_t op_list[16];
/* ☠️☠️☠️ 32, NOT 16 (2026-08-26). The OP fetches a SCALED (TYPE-1) object as ONE
   4-phrase = 32-BYTE burst and DIES if it straddles a 32-byte boundary: black
   first field, wedged machine. aligned(16) let the linker put us at 16 mod 32
   half the time - and THAT is the whole A10 / PADTEXT boot lottery. Hardware
   result from jag_quake 2026-08-22; cobweb 905188f models it, and jsim scores
   our own ship pads 0/408/544 dead (16 mod 32) against 136/272/816 clean.
   With OPDBL, list B is at op_list[8] = +32, so both lists share the residue.
   ☠️ Do not "tidy" this back to 16. */
/* A10 (2026-07-30): CONTIGUOUS shadow of the 6 longs the vblank ISR restores.
   The ISR must rebuild the scaled object before the OP re-fetches it at VC 32.
   The old repair was six `move.l abs,abs` - each 5 words of INSTRUCTION FETCH
   before any data moves, ~30 fetch words total - and video.c:145 records it
   chronically finishing at VC 33-35 once render-time bus starvation cuts the
   68k to ~10-20% bus service. Under starvation the fetches are the expensive
   part, so a movem block copy (2 instructions) is the shortest path to the
   deadline. Keep these SIX longs adjacent and in OP-list order. */
uint32_t op_shadow[6] __attribute__((aligned(16)));
static uint16_t a_vdb_g, a_vde_g;   /* active vertical window (VC half-lines) */

volatile uint32_t frame_count;

FLIPSTATIC fbpix *draw_buf;               /* CPU renders here                    */
OPSTATIC uint32_t front_fb;           /* what the OP displays                */
OPSTATIC volatile uint32_t pending_fb;/* buffer the ISR should show, 0=none  */
/* VIDEO PLAYER PRESENTATION SCHEDULE (2026-08-06): the ISR performs a
   pending flip only once frame_count reaches this field. Zero = always
   due (every existing caller). The FMV player sets it per frame so the
   cadence is HARDWARE-timed and immune to 68k read stalls; it must be
   reset to 0 when the player exits. */
OPSTATIC volatile uint32_t video_pend_at;
/* FMV 2-BUFFER PIN (2026-08-07): delta video patches the back buffer with
   the PREVIOUS frame's tokens, which covers exactly 2-deep staleness. The
   demo's TRIPLE rotation leaves a third buffer un-patched -> the title
   screen GHOSTED through the clip. While set, video_flip ping-pongs
   fb0/fb1 only; the player waits for the pending flip BEFORE decoding so
   the displayed buffer is never written mid-scan. Reset on player exit. */
OPSTATIC volatile uint32_t video_two_buf;

/* FAST OP-LIST REPAIR (plain unscaled object only). The OP destroys ONLY
 * phrase 0 of the bitmap object (data ptr / ypos / height) as it draws; the
 * probe showed the full 8-long rebuild chronically finishing at VC 33-35 —
 * PAST the object fetch at 32 — under render-time bus starvation (~10-20%
 * bus service). The ISR now restores just the two destroyed longs from
 * values PRECOMPUTED outside the ISR (flip values prepared in video_flip). */
OPSTATIC volatile uint32_t op_fix0;            /* phrase 0 for front_fb  */
static volatile uint32_t op_fix1;
OPSTATIC volatile uint32_t pend_fix0;          /* phrase 0 for pending_fb */
static volatile uint32_t pend_fix1;
FLIPSTATIC uint32_t op_link;                       /* link field, set at build */

#ifdef OPDBL
/* ---- OP-LIST DOUBLE BUFFER (2026-08-02) -------------------------------
 * The OP DESTROYS phrase 0 of the object as it draws it, so this ISR has always
 * had to REPAIR the list before the OP re-fetches it at VC 32. video.c's own
 * probe measured the old 8-long rebuild landing at VC 33-35 under bus
 * starvation - i.e. the repair was already losing that race, which is why it
 * was cut down to precomputed stores. On an ACTIVE cartridge (the GameDrive,
 * with its SD/USB traffic on the cart bus) the extra bus load pushes it over
 * again and the OP fetches a HALF-REPAIRED object: the field then displays the
 * wrong buffer. Measured as ~1.86 full-screen changes per rendered frame vs a
 * clean 1.00 on the Skunkboard - the ghosting.
 * So stop racing the deadline and DELETE it: keep TWO complete lists and
 * alternate. The only deadline-bound work becomes ONE store (OLP <- the clean
 * list); the 6-store repair of the list destroyed LAST field then has a whole
 * field of slack instead of ~32 half-lines.
 * Storage is free: op_list is already uint32_t[16] and the scaled object uses
 * only [0..7], so list B lives at [8..15]. Each list needs its OWN link, since
 * the link points at that list's own STOP. */
static volatile uint32_t olp_sw[2];    /* pre-byte-swapped OLP per list      */
static volatile uint32_t olp_next;     /* OLP for the NEXT field (scalar, so
                                          the critical store needs no index) */
static volatile uint32_t link_hi[2];   /* link>>8, the ph0 half              */
static volatile uint32_t fs_ph1_l[2];  /* ph1 carries the link too           */
static volatile int op_cur;            /* list the OP is using this field    */
#endif

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
#ifdef VRESN
/* VSCALE is 3.5 fixed point (0x20 = 1.0x), so the scale that makes N source
   lines fill the 240-line window is 240/N and its register value is 7680/N.
   Exact only for N in {120,96,80,64,60} - the Makefile enforces that. */
#define FS_VSCALE (7680u / (unsigned)VRESN)
#elif defined(VRES60)
#define FS_VSCALE 0x80u   /* 4.0x  -  60 source lines fill the 240-line window   */
#else
#define FS_VSCALE 0x40u   /* 2.0x  - 120 source lines fill the 240-line window   */
#endif
#define FS_SCALE  (((uint32_t)FS_VSCALE << 8) | (uint32_t)FS_HSCALE)

/* FAST OP-LIST REPAIR FOR THE SCALED OBJECT (2026-07-24).
 * The scaled path used to rebuild all 8 longs AND reprogram OLP from inside the
 * vblank ISR every field. The plain path's probe (see above) already showed that
 * the full rebuild chronically finishes at VC 33-35 — PAST the OP's object fetch
 * at VC 32 — once render-time bus starvation cuts the 68k to ~10-20% bus service.
 * That is the leading explanation for LOWRES blacking out on MULTIROOM+FB8 while
 * it was HW-verified fine on the older, lightly-loaded single-room RGB16 path:
 * the OP walks a half-written list.
 * Fix: precompute every long outside the ISR and let the ISR do plain stores, no
 * call, no arithmetic, no OLP write (OLP is not destroyed — the plain path has
 * never rewritten it per field). Only phrase 0's data pointer varies per flip;
 * phrases 1 and 2 are build-time constants. */
uint32_t fs_ph1, fs_ph2, fs_ph3;          /* constant longs of the scaled object */
/* op_list[5] (VSCALE|HSCALE) for the ISR's fast repair. It used to be a literal
   #0x4020 in startup.S, which overrode whatever C put there - see the note in
   the ISR. Owned here so the two paths cannot disagree. */
uint32_t fs_ph5 = ((uint32_t)FS_VSCALE << 8) | (uint32_t)FS_HSCALE;
/* #define LOWRES_DIAG_PLAIN 1 -- ISOLATION TEST (HW-verified 2026-07-08): with
 * this defined, LOWRES displays the 320x120 fb as a PLAIN bitmap (no scale) and
 * the room renders CORRECTLY in the top 120 lines -> kernel+fb are FINE; the
 * ONLY blocker is the TYPE-1 scaled object below. Re-enable to re-confirm. */
extern int g_disp240;               /* task #4: 1 = title plain-240 mode */

/* ☠☠ UNDER HRESN, IWIDTH AND VMODE ARE PHASE-DEPENDENT, NOT BUILD-CONSTANT.
   The GAME renders VIEW_W columns and wants the narrow fetch + wide pixel
   clock.  The TITLE, the FMV clips and the loading art are full-width 320
   assets and want neither.  Narrowing all four op_list sites unconditionally
   shipped a build whose intro FMV was CUT OFF at the right edge on silicon
   (job #2545) while rendering plausibly in jagemu -- which cannot see PWIDTH
   and takes its screenshot from the framebuffer, so a wrong OP object is
   invisible offline.  g_disp240 is the phase, so it is the switch.
   ☠ gpu.c:423-431: a VMODE write disturbs the video timing generator, so it
   must happen ONLY on the title<->game transition, never per frame. */
#ifdef HRESN
#define OP_IWIDTH   ((uint32_t)(g_disp240 ? SCREEN_PWIDTH : SCREEN_IWIDTH))
#define VMODE_NOW   ((uint32_t)(g_disp240 ? 0x06C7 : JAG_VMODE))
#else
/* ☠ Without HRESN these MUST collapse to the old constants, not to a runtime
   conditional that happens to pick the same value.  The first cut left the
   ternary in unconditionally: JAG_VMODE is 0x06C7 at 320 so the BEHAVIOUR was
   identical, but the codegen was not and the 320 ROM stopped being
   byte-identical -- which is this project's standard proof that a new flag is
   inert when off.  It also added a VMODE write to the 320 phase transition
   that was never there (gpu.c:423-431: VMODE writes disturb the video timing
   generator). Caught by the no-op check, which is what it is for. */
#define OP_IWIDTH   ((uint32_t)SCREEN_PWIDTH)
#define VMODE_NOW   ((uint32_t)JAG_VMODE)
#endif
/* ☠️☠️ THIS DECLARATION MUST STAY ABOVE build_object_list (run 11).
 * It was below it, and video.c is compiled by **jcc68k, which accepted the
 * undeclared identifier SILENTLY** where gcc errors out. The result was not a
 * build failure but a WRONG OBJECT: `if (g_op_plain240 && g_disp240)` evaluated
 * true, so every build from run 8 on shipped the PLAIN TYPE-0 probe shape
 * (fs_ph1 TYPE=0, fs_ph5=4) instead of the scaled TYPE-1 object
 * (fs_ph1 TYPE=1, fs_ph5=0x2020) - with the probe switched OFF.
 * Caught by rebuilding the last known-good commit and dumping both values,
 * which is jaguar-shared's `1da82fd` rule, read the same day and applied three
 * runs late. Every hardware result from run 8 to run 10 is contaminated.
 * run-8 probe: 1 = build a TYPE-0 PLAIN bitmap for the 240 display instead of
 * the 1.0x-scaled TYPE-1 object. Set BEFORE video_set_disp240() rebuilds. */
int g_op_plain240 = 0;
/* run-14 bandwidth probe: 1 = ask the OP for 120 lines instead of 240. Same
   placement rule as g_op_plain240 above - ABOVE build_object_list. */
int g_op_half240 = 0;

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
    op_list[3] = (OP_IWIDTH << 28) | ((uint32_t)SCREEN_PWIDTH << 18)
               | (1u << 15) | OP_DEPTH | BASE_X;
    op_list[4] = 0;
    op_list[5] = 4;
#elif defined(OPPLAIN)
    /* SCALER-BISECT PROBE (2026-07-25). Silicon shows the LOWRES display
     * breaking up mid-frame (a stationary white band + garbled rows over the
     * lower ~45%, with Lara drawn COMPLETE on top of it) while the SAME .cof
     * renders clean in jagemu — so the corruption is on the DISPLAY side, and
     * jagemu cannot arbitrate it (its OP never writes objects back and is
     * never bus-starved). Two candidates remain and they need opposite fixes:
     *   (a) the TYPE-1 scaled object cannot survive render-time bus pressure
     *       (it re-fetches 3 phrases/line and writes back the remainder — ~50%
     *       more OP bus traffic per line than the plain object, and the
     *       Makefile already records the scaler "blacking out under heavy
     *       multiroom fill" while the unscaled path survives);
     *   (b) the renderer and the scan-out genuinely share a buffer.
     * OPPLAIN=1 shows the SAME 320x120 framebuffer through a PLAIN (TYPE-0)
     * unscaled object — top 120 lines, black below — changing NOTHING else:
     * same render, same flip protocol, same ISR repair, same buffers.
     *   bands GONE  -> the scaler is the culprit (a), LOWRES-via-scaler is dead
     *   bands STAY  -> a real draw/scan race (b), fix the barrier
     * One flash, one photo. */
    uint32_t link = ((uint32_t)&op_list[4]) >> 3;   /* STOP at op_list[4] */

    op_list[0] = (fb_addr << 8) | (link >> 8);
    op_list[1] = (link << 24)
               | ((uint32_t)RENDER_H << 14)         /* on-screen height (120) */
               | ((uint32_t)BASE_Y << 4);           /* TYPE 0 = plain bitmap  */

    op_list[2] = SCREEN_PWIDTH >> 4;
    op_list[3] = (OP_IWIDTH << 28)
               | ((uint32_t)SCREEN_PWIDTH << 18)
               | (1u << 15)                         /* PITCH 1 */
               | OP_DEPTH
               | BASE_X;

    op_list[4] = 0;                                 /* STOP */
    op_list[5] = 4;

    op_link = link;
    op_fix0 = op_list[0];
    fs_ph1  = op_list[1];
    fs_ph2  = op_list[2];
    fs_ph3  = op_list[3];
    op_shadow[0]=op_list[0]; op_shadow[1]=op_list[1];
    op_shadow[2]=op_list[2]; op_shadow[3]=op_list[3];
    op_shadow[4]=0;          /* REMAINDER, exactly as the ISR writes it   */
    op_shadow[5]=fs_ph5;     /* VSCALE|HSCALE - C owns the 2.0x/4.0x value */
#else
    /* task #4 TITLE 240: SAME scaled TYPE-1 machinery, but scale 1.0x over the
       240-tall fb (a 1:1 "scaled" object IS a plain display, and the entire
       proven repair/OPDBL structure stays byte-identical in layout). */
    /* ☠️ HEIGHT IS A COUNT, NOT AN INDEX (2026-08-08, user: "at the bottom
       there are lines that extend across").  The OP's bitmap HEIGHT field is
       the number of lines to display - it counts DOWN to zero - and for a
       scaled object it counts SOURCE lines.  This branch wrote one less than
       that ("source lines - 1"), while the plain-bitmap branch a few lines up
       writes RENDER_H.  One of the two is wrong, and it is this one: at the
       2.0x vertical scale the game uses, one missing source line is TWO TV
       lines with no data at the bottom of the frame - which is why the band
       shows in the loading screen and in the game (both scaled 120->240) but
       never on the title or the FMVs (plain 240, 1.0x). */
    { uint32_t t_srcl  = g_disp240 ? 240u : (uint32_t)RENDER_H;
      /* ☠️ BANDWIDTH PROBE (run 14). The FMV object asks the Object Processor
         for 320x240 of 8bpp every field; the GAME's object asks for 320x120
         and lets the OP scale it 2x. **The clips make the OP fetch TWICE what
         the game does**, through the same scaler, which is the cleanest
         account yet of why the game and the title look right and the clips do
         not. Halving the object's HEIGHT halves that fetch and shows only the
         top half of the picture - if that half comes back CLEAN, the fault is
         OP fetch bandwidth and the fix is to stop asking for 240 lines.
         Set from main.c's JVDECMASK bit 11 (2048). */
      if (g_op_half240 && g_disp240) t_srcl = 120u;
      /* ☠️ PLAIN-OBJECT PROBE (run 8, 2026-08-18, the user's lead). The clips
         are displayed through this SCALED TYPE-1 object with the scale set to
         1.0x - the comment above says so outright - purely so nothing has to
         be switched before the title. But a 1:1 scaled object still runs the
         OP's SCALER, and jaguar-shared records the scaler starving the bus
         15-20x on real Tom. Fault 2 behaves exactly like a lost fetch: a flat
         field renders 0.00% and ANY detailed data comes back ~50% wrong, from
         any writer. So build a real TYPE-0 PLAIN bitmap instead and see.
         The shape stays 6 longs so the ISR's repair (d[0..5]) is untouched:
         the object is 4 longs, the STOP moves to [4]/[5], and fs_ph5 - which
         the ISR stores into d[5] - carries the STOP's TYPE instead of the
         scale word. */
      uint32_t t_type = 1u;
      uint32_t stop_i = 6u, stopB_i = 14u;
      if (g_op_plain240 && g_disp240) { t_type = 0u; stop_i = 4u; stopB_i = 12u; }
      fs_ph5 = (t_type == 0u) ? 4u
             : g_disp240 ? 0x2020u                       /* 1.0x V, 1.0x H */
                         : (((uint32_t)FS_VSCALE << 8) | (uint32_t)FS_HSCALE);
    /* FIX (HW-verified 2026-07-08): a BARE scaled object (scaled bitmap ->
     * STOP, NO branch gating) is correct. My earlier BRANCH
     * gating (VC vs a_vdb/a_vde) was masking the ENTIRE display -> black. The
     * scaled object as the first/only object works; the room renders scaled
     * 120->240. (a_vdb_g/a_vde_g now unused by LOWRES; kept harmless.) */
    uint32_t link = ((uint32_t)&op_list[stop_i]) >> 3;

    op_list[0] = (fb_addr << 8) | (link >> 8);
    op_list[1] = (link << 24)
               | (t_srcl << 14)                     /* source lines - 1 (scaled) */
               | ((uint32_t)BASE_Y << 4)            /* YPOS                       */
               | t_type;                            /* 1 = scaled, 0 = plain      */

    op_list[2] = SCREEN_PWIDTH >> 4;
    op_list[3] = (OP_IWIDTH << 28)
               | ((uint32_t)SCREEN_PWIDTH << 18)
               | (1u << 15)                         /* PITCH 1 */
               | OP_DEPTH                         /* DEPTH 16bpp */
               | BASE_X;

    op_list[4] = 0;                                 /* SCALE: REMAINDER = 0,
                                                       or the STOP's high long */
    op_list[5] = fs_ph5;                            /* scale word, or STOP TYPE */

    op_list[6] = 0;                                 /* STOP (scaled shape only) */
    op_list[7] = 4;

    /* cache everything the ISR must restore, so the ISR itself only stores */
    op_link = link;
    op_fix0 = op_list[0];
    fs_ph1  = op_list[1];
    fs_ph2  = op_list[2];
    fs_ph3  = op_list[3];
#ifdef OPDBL
    /* Build list B at op_list[8..15] as a twin of A, with its OWN link (its
       STOP is at [14], not [6]). Everything the ISR needs is precomputed here,
       outside the deadline. */
    {
        uint32_t linkB = ((uint32_t)&op_list[8 + (stopB_i - 8)]) >> 3;
        uint32_t a, b;
        op_list[8]  = (fb_addr << 8) | (linkB >> 8);
        op_list[9]  = (linkB << 24)
                    | (t_srcl << 14)
                    | ((uint32_t)BASE_Y << 4)
                    | t_type;
        op_list[10] = op_list[2];
        op_list[11] = op_list[3];
        op_list[12] = 0;
        op_list[13] = fs_ph5;
        op_list[14] = 0;                 /* STOP */
        op_list[15] = 4;
        link_hi[0]  = link  >> 8;
        link_hi[1]  = linkB >> 8;
        fs_ph1_l[0] = op_list[1];
        fs_ph1_l[1] = op_list[9];
        a = (uint32_t)&op_list[0];
        b = (uint32_t)&op_list[8];
        olp_sw[0] = (a >> 16) | (a << 16);   /* OLP wants its halves swapped */
        olp_sw[1] = (b >> 16) | (b << 16);
        op_cur   = 0;
        olp_next = olp_sw[1];
    }
#endif
    op_shadow[0]=op_list[0]; op_shadow[1]=op_list[1];
    op_shadow[2]=op_list[2]; op_shadow[3]=op_list[3];
    op_shadow[4]=0;          /* REMAINDER, exactly as the ISR writes it   */
    op_shadow[5]=fs_ph5;     /* VSCALE|HSCALE - C owns the 2.0x/4.0x value */
    }
#endif
}

/* TITLE 240 switch (task #4): call with Tom idle + a just-flipped display.
 * Rebuilds both OP lists + every precomputed repair value for the new mode. */
int g_disp240 = 1;   /* PROBE: boot in title-240 (scaled-1x) */
void video_set_disp240(int on)
{
    g_disp240 = on;
#ifdef HRESN
    /* ★ THE ONLY PLACE THE PIXEL CLOCK MAY CHANGE.  This is the title<->game
       transition, which is exactly the boundary gpu.c:423-431 says a VMODE
       write must be confined to -- it disturbs the video timing generator, so
       doing it per-frame costs half the frame throughput.  Written BEFORE the
       rebuild so the OP never scans one field with the new IWIDTH against the
       old pixel clock. */
    VMODE = VMODE_NOW;
#endif
    build_object_list(front_fb);
}
#else
/* 2026-08-02: these live in the LOWRES branch above, but startup.S's ISR and
 * exc_catch reference them UNCONDITIONALLY, so a full-height (320x240) build
 * failed to link.  The plain object is UNSCALED, so VSCALE=HSCALE=0x20 (1.0x)
 * rather than the LOWRES 2.0x/4.0x.  Values are filled in by
 * build_object_list below, exactly as the scaled path does. */
uint32_t fs_ph1, fs_ph2, fs_ph3;
uint32_t fs_ph5 = 0x2020u;      /* 1.0x vertical, 1.0x horizontal */

static void build_object_list(uint32_t fb_addr)
{
    uint32_t link = ((uint32_t)&op_list[4]) >> 3;

    op_list[0] = (fb_addr << 8) | (link >> 8);
    op_list[1] = (link << 24)
#ifdef SLITDISPLAY
               /* BUS PROBE (2026-07-20): present only the top 64 lines —
                  OP framebuffer fetch drops ~73% while ALL render work is
                  unchanged. If fps rises, the OP's constant bus load is
                  throttling Tom (the "bus bottleneck" theory); the fps bar
                  (rows 21-55) stays visible for the measurement. */
               | (64u << 14)
#else
               | ((uint32_t)DISPLAY_H << 14)       /* on-screen height (240) */
#endif
               | ((uint32_t)BASE_Y << 4);

    op_list[2] = SCREEN_PWIDTH >> 4;
    op_list[3] = (OP_IWIDTH << 28)
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
#ifdef PADVID
#define PV_STR2(x) #x
#define PV_STR(x) PV_STR2(x)
/* PADVID=N (2026-08-07): shift the ISR handler WITHIN video.o - the
   HANGDIAG-boots clue suggests the A10-critical position is internal
   to video.o, which whole-object pads (PADMAIN) cannot move. */
__attribute__((used, noinline)) void pad_video_probe(void)
{
    __asm__ volatile(".space " PV_STR(PADVID));
}
#endif

void vblank_handler(void)
{
#if defined(BEACON_AT) && BEACON_AT == 11
    /* Fires on the FIRST vblank interrupt ever taken. The beacon is validated
       (it goes solid on a booting build), so BLACK here means the interrupt
       genuinely never arrives - which is the documented VI failure mode:
       "dead-black screen, ISR dead, flip spins forever". */
    { extern void hang_beacon(uint16_t); hang_beacon(0x003E); }   /* GREEN */
#endif
    uint32_t pf = pending_fb;
#ifdef HANGDIAG
    /* GREEN border on the FIRST vblank: proves interrupts are alive at all.
       Any later HANGDIAG halt overwrites it, so the final colour is the most
       specific fact available - and a border still BLACK after 45 s means the
       ISR never ran even once. */
    { static int hd_first = 0;
      if (!hd_first) { hd_first = 1; *(volatile uint16_t *)0xF00058u = 0x03E0; } }
#endif
#if !(defined(LOWRES) && !defined(HALFRES))
    /* fast path: restore ONLY the OP-destroyed phrase — 2 stores, no calls.
       Flip values were precomputed in video_flip, outside the ISR. */
    if (pf && (int32_t)(frame_count - video_pend_at) >= 0) {
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
    /* scaled object: the OP destroys phrase 0 (data ptr / ypos / height) AND the
       scale phrase's REMAINDER. Restore from precomputed values — stores only,
       no call, no arithmetic, no OLP reprogram (see FAST OP-LIST REPAIR above).
       Phrase 1 is restored too: it costs 2 stores and removes a failure class. */
    /* NOTE: video.c is compiled by jcc68k (NOT gcc -O2 — see the Makefile).
       jcc68k folds no constant offsets and keeps every local in memory, so an
       indexed store costs ~10 instructions and a walking pointer ~13 (measured
       by disassembly). Indexed is the cheaper phrasing here; ~60 instructions
       total is still ~6x below the rebuild this replaces, and the probe showed
       that rebuild overshooting the VC-32 fetch by only 1-3 half-lines. */
    if (pf && (int32_t)(frame_count - video_pend_at) >= 0) {
        op_fix0 = pend_fix0;
        front_fb = pf;
        pending_fb = 0;
    }
#ifdef OPDBL
    /* ONE deadline-bound store: point the OP at the list repaired last field.
       Everything below runs with a full field of slack. */
    OLP = olp_next;
    if (pf) { op_fix0 = pend_fix0; front_fb = pf; pending_fb = 0; }
    op_cur ^= 1;                       /* the store above switched lists     */
    {
        int dead = op_cur ^ 1;         /* destroyed during the LAST field    */
        uint32_t *d = &op_list[dead * 8];
        d[0] = ((front_fb) << 8) | link_hi[dead];
        d[1] = fs_ph1_l[dead];
        d[2] = fs_ph2;
        d[3] = fs_ph3;
        d[4] = 0;
        d[5] = fs_ph5;                 /* scale (game) or STOP-low 4 (title) */
        olp_next = olp_sw[dead];       /* next field uses what we just fixed */
    }
    OBF = 0;
#else
#ifdef OPMIN
    /* OPMIN (2026-08-02): DISCRIMINATOR, not a shipping option.
       The ghosting on the GameDrive is the OP fetching a HALF-REPAIRED object:
       this repair must land before the OP re-fetches at VC 32, and video.c's own
       probe measured the old 8-long rebuild finishing at VC 33-35 under bus
       starvation. But that could be the ISR being too SLOW *or* the ISR ENTRY
       being too LATE, and the fixes differ completely (double-buffering the OP
       list cures the former and does nothing for the latter).
       So write ONLY what the OP actually destroys — phrase 0 ([0],[1]) and the
       scale REMAINDER ([4]) — and leave [2],[3],[5] alone. Halves the store
       count. If the ghosting improves, ISR LENGTH is the problem and
       double-buffering is the fix. If it does not move, it is interrupt-entry
       latency and shortening the ISR can never help. */
    op_list[0] = op_fix0;
    op_list[1] = fs_ph1;
    op_list[4] = 0;
#else
    op_list[0] = op_fix0;          /* [0] data ptr | link                   */
    op_list[1] = fs_ph1;           /* [1] height | ypos | TYPE=1            */
    op_list[2] = fs_ph2;           /* [2] iwidth                            */
    op_list[3] = fs_ph3;           /* [3] dwidth | pitch | depth | xpos     */
    op_list[4] = 0;                /* [4] REMAINDER — destroyed every field */
    op_list[5] = FS_SCALE;         /* [5] vscale | hscale                   */
#endif
    OBF = 0;
#endif /* OPDBL */
#endif
    frame_count++;
}

void video_init(void)
{
#ifdef EARLYCON
    { extern void dbg_kv(const char *, long); dbg_kv("vi_enter", 1); }
#endif
#if defined(BEACON_AT) && BEACON_AT == 1
    { extern void hang_beacon(uint16_t); hang_beacon(0x07C0); }   /* BLUE */
#endif
#ifdef HANGDIAG
    *(volatile uint16_t *)0xF00058u = (uint16_t)0x07C0;
#endif
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

#ifdef EARLYCON
    { extern void dbg_kv(const char *, long); dbg_kv("vi_cleared", 1); }
#endif
    build_object_list(front_fb);
    point_op_at_list();
#ifdef EARLYCON
    { extern void dbg_kv(const char *, long); dbg_kv("vi_oplist", 1); }
#endif

    /* horizontal window */
    HDE  = (uint16_t)((width / 2 - 1) | 0x400);
    HDB1 = (uint16_t)(hmid - width / 2 + 4);
    HDB2 = (uint16_t)(hmid - width / 2 + 4);

    /* vertical window: program VDB, leave VDE wide open */
    a_vdb = a_vdb_g;
    VDB = a_vdb;
    VDE = 0xFFFF;

#ifdef A10BG
    /* A10 THREE-WAY PROBE (2026-07-30). BGEN is on, so BG fills every active
       line that no object paints. On a BOOTING build the object covers the
       screen and BG stays hidden; on a FAILING build nothing paints, which is
       exactly why it reads black (BG=0). So paint BG BLUE here and RED from
       the ISR, and one flash distinguishes:
         RED   -> the ISR ran at least once  (=> OP/display or deadline bug)
         BLUE  -> video_init finished, ISR NEVER ran (=> interrupt never fires)
         BLACK -> never even got here / no signal
       Self-validating: the BLUE arm proves BG is visible in the failing state,
       which is the positive control hangbeacon.c demands before trusting a
       negative. Jaguar RGB16 = R<<11 | B<<6 | G<<1. */
    BG    = 0x07C0;            /* BLUE: video_init reached the end */
#else
    BG    = 0;
#endif
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
    /* ☠️ REFUTED 2026-07-29: writing VMODE (VIDEN) BEFORE arming VI - so the
       comparator is armed while the counter is running - does NOT fix A10.
       Tried with IRQREARM off so the reorder was the only change, main.o
       byte-identical to the failing build: still black. Do not re-run it. */
#ifdef EARLYCON
    { extern void dbg_kv(const char *, long); dbg_kv("vi_window", (long)a_vdb); }
#endif
    VI = (uint16_t)(a_vdb - 4);
    INT1 = 0x0003;              /* enable VIDEO + GPU interrupts (STOP-sync) */
    cpu_irq_on();
#ifdef EARLYCON
    { extern void dbg_kv(const char *, long); dbg_kv("vi_irqon", 1); }
#endif

    /* RGB16, CSYNC, BGEN, VIDEN, PWIDTH=4 -> the standard 320-wide mode */
    VMODE = VMODE_NOW;
#ifdef EARLYCON
    { extern void dbg_kv(const char *, long); dbg_kv("vi_done", 1); }
#endif
#ifdef A10BG
    /* GREEN = VI armed, INT1 enabled, cpu_irq_on() returned and VMODE written,
       i.e. video_init ran to completion. The BLUE above sits BEFORE the arming
       block, so blue alone would not have proved the arming was reached.
         GREEN -> armed, and the ISR still never fired
         BLUE  -> died inside the arming block itself
         RED   -> ISR ran */
    BG = 0x003E;
#endif
#if defined(BEACON_AT) && BEACON_AT == 2
    { extern void hang_beacon(uint16_t); hang_beacon(0x003E); }   /* GREEN */
#endif
}

/* Re-arm the vertical interrupt from its stored window.  A10 (2026-07-29):
   on a failing build the video interrupt NEVER FIRES - proven with a validated
   beacon - so every interrupt-dependent wait sleeps forever (gpu_sync's STOP
   first, then the flip's pending_fb wait). video.o is byte-identical between a
   booting and a failing build, so nothing here is being overwritten; the arming
   is simply not taking. This re-asserts it. */
void video_rearm_irq(void);
void video_rearm_irq(void)
{
    VI   = (uint16_t)(a_vdb_g - 4);
    INT1 = 0x0003;
    VMODE = VMODE_NOW;
    cpu_irq_on();
}

/* Do the vblank ISR's work FROM THE MAIN LOOP.
   A10: when the vertical interrupt stops firing, the ISR never runs, and both
   waits that depend on it hang forever - gpu_sync's STOP and the flip's
   pending_fb wait. Re-arming the interrupt (video_rearm_irq) recovers many
   builds but not all. This is the belt to that pair of braces: it retires the
   pending flip and rebuilds the phrases the OP consumes, so the picture keeps
   advancing even with a dead ISR. Called only from the flip wait after it has
   already spun for a long time, so it costs nothing when the ISR is healthy.
   It can tear - it runs at an arbitrary raster position rather than in the
   pre-display window - but a torn frame beats a dead console. */
void video_flip_force(void);
void video_flip_force(void)
{
    uint32_t pf = pending_fb;
#ifdef VECDIAG
    /* A10 probe: this function only runs once the vblank ISR has already
       failed three re-arms, so it is the exact moment of failure. Report
       whether 68k VECTOR 64 ($100) still points at vblank_stub.
         GREEN  = vector intact  -> the interrupt is armed and vectored, and
                  something else stops it being delivered
         RED    = vector CLOBBERED -> a wild write hit low memory, which would
                  be codegen-dependent and explains everything
       The image loads at $4000, so nothing of ours should ever write $100. */
    { extern void vblank_stub(void);
      extern void hang_beacon(uint16_t);
      uint32_t v = *(volatile uint32_t *)0x100u;
      hang_beacon(v == (uint32_t)&vblank_stub ? 0x003E : 0xF800); }
#endif
    if (pf && (int32_t)(frame_count - video_pend_at) >= 0) {
        op_fix0 = pend_fix0;
        front_fb = pf;
        pending_fb = 0;
    }
#if defined(LOWRES) && !defined(HALFRES) && !defined(OPPLAIN)
    /* SCALED object: 6 longs, STOP at [6]. NOT valid under OPPLAIN, where the
       object is 4 longs and the STOP lives at [4]/[5] - writing the scale
       phrase there corrupts the STOP and the OP runs off into memory. */
    op_list[0] = op_fix0;
    op_list[1] = fs_ph1;
    op_list[2] = fs_ph2;
    op_list[3] = fs_ph3;
    op_list[4] = 0;
    op_list[5] = fs_ph5;
#elif defined(OPPLAIN)
    op_list[0] = op_fix0;
    op_list[1] = fs_ph1;
    op_list[2] = fs_ph2;
    op_list[3] = fs_ph3;
#else
    op_list[0] = op_fix0;
    op_list[1] = op_fix1;
#endif
    OBF = 0;
}

void *video_backbuffer(void)
{
    return draw_buf;
}

/* FMV pin start: enter 2-buffer mode with a DETERMINISTIC first target.
   Without this the first (key)frame lands in whatever buffer the menu left
   as draw target - often fb2, which the pinned rotation never revisits, so
   one of fb0/fb1 misses the keyframe and flickers stale-title ghost every
   other frame until a later keyframe heals it (user 2026-08-07). Seeding
   the ping-pong on the non-shown of fb0/fb1 guarantees frame 0 (keyframe)
   and frame 1 (prev = that keyframe) cover both buffers. */
void video_pin_start(void)
{
    while (pending_fb)
        ;
    draw_buf = ((uint32_t)fb0 != front_fb) ? fb0 : fb1;
    video_two_buf = 1;
}

/* Load a 256-entry RGB16 palette into the OP CLUT (for FB8 8bpp mode). */
void video_set_clut(const uint16_t *pal)
{
#if defined(BEACON_AT) && BEACON_AT == 5
    { extern void hang_beacon(uint16_t); hang_beacon(0xF800); }   /* RED */
#endif
    volatile uint16_t *clut = (volatile uint16_t *)0xF00400u;
    int i;
    for (i = 0; i < 256; i++)
        clut[i] = pal[i];
}

#ifdef FLIPASM
extern void video_flip_asm(void);
void video_flip(void)
{
#ifdef A10BG
    /* A10 LIVENESS probe: cycle BG on every flip attempt. The GREEN left by
       video_init persists whether the 68k is running or stopped, so it cannot
       tell those apart. This can:
         changing colour -> the 68k is ALIVE and reaching video_flip; the
                            interrupt simply never arrives
         steady GREEN    -> never reached the first flip (stuck earlier - level
                            load, or the first gpu_sync STOP sleeping forever) */
    { static uint16_t t; t++; BG = (uint16_t)((t & 31) << 11); }   /* red ramp */
#endif
    video_flip_asm();
}
#else
void video_flip(void)
{
#if defined(BEACON_AT) && BEACON_AT == 7
    { extern void hang_beacon(uint16_t); hang_beacon(0xFFFE); }   /* WHITE */
#endif
    uint32_t shown;

    /* Was a tight DRAM poll on a volatile global — the anti-pattern gpu_sync's
       own comment warns about ("a tight DRAM poll steals bus cycles from Tom for
       the entire frame"). Tom is RENDERING during this wait, so the spin slows
       the very thing it waits for. STOP releases the bus; the vblank ISR clears
       pending_fb and vblank bounds the wake. Supervisor mode throughout.
       Safe at init: if the ISR were not running, pending_fb would never clear
       and the old spin would have hung here too. */
#ifdef HANGDIAG
    /* POLL, never STOP: a STOP that is never woken cannot time itself out -
       the counter below would never advance - and "asleep forever" is exactly
       the failure mode still on the table. */
    { uint32_t g = 0;
      while (pending_fb) {
          g++;
          if (g > 8000000u) {
              *(volatile uint16_t *)0xF00058u = 0x07FF;  /* CYAN: flip wait */
              for (;;) ;
          }
      } }
#else
    while (pending_fb)
#ifdef FLIPSPIN
        ;
#else
        /* atomic check+STOP (see gpu_sync PACING note). Here the waker is
           the VBL ISR itself so the old race self-healed in one field, but
           the closed window costs nothing and one lost field is exactly
           what this campaign hunts. */
        cpu_stop_unless(&pending_fb, 0);
#endif
#endif
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
        if (video_two_buf)
            draw_buf = ((uint32_t)fb0 != (uint32_t)done) ? fb0 : fb1;
        else if ((uint32_t)fb0 != (uint32_t)done && (uint32_t)fb0 != shown)
            draw_buf = fb0;
        else if ((uint32_t)fb1 != (uint32_t)done && (uint32_t)fb1 != shown)
            draw_buf = fb1;
        else
            draw_buf = fb2;
        /* precompute the ISR's phrase-0 repair values here (main-loop time)
           so the flip path in the ISR is also just 2 stores */
        pend_fix0 = ((uint32_t)done << 8) | (op_link >> 8);
#if !(defined(LOWRES) && !defined(HALFRES))
        pend_fix1 = (op_link << 24)
                  | ((uint32_t)DISPLAY_H << 14)
                  | ((uint32_t)BASE_Y << 4);
#else
        /* COMPLETION BARRIER (2026-07-25). gpu_sync() waits for Tom's GPU
         * PROGRAM, not for the Blitter: the kernel launches a span and moves
         * on, so the last span(s) can still be transferring into `done` when
         * we publish it. HALFRES never had this hole — blit_double() ends
         * with blit_wait() (blit.c), so by the time IT publishes, the Blitter
         * is idle and the display buffer is whole. That trailing wait is half
         * of what "blit_double was also the frame barrier" meant; the other
         * half (render off-screen) is already satisfied here, and MEASURED:
         * 400 consecutive fields sampled in jagemu show draw_buf is never
         * front_fb nor pending_fb (0 violations). So this is the one piece
         * the direct-to-display path was actually missing.
         * Cost is ~nil when the Blitter is already idle, which is the normal
         * case after gpu_sync; it is an I/O-register poll, not a DRAM poll. */
        while (!(B_CMD & BLIT_IDLE))
            ;
#endif  /* scaled path: phrase 0's second long is constant (fs_ph1) */
        pending_fb = (uint32_t)done;
    }
#endif
}

#endif /* FLIPASM */

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
    /* ☠️ MASK THE COUNTER.  VC carries a flag above the half-line count, so a
       RAW read is >= 2048 for a whole field and the window test below can
       never be true - the loop then spins out the ENTIRE field.  Measured on
       silicon 2026-08-09: 12-22ms a frame in here, 8-15% of the frame, for a
       blit that takes 2-4ms.  vp_tick() in vidpanel.c always masked; this one
       never did.  Masked, the wait is bounded by the window itself (~4ms). */
    for (;;) {
        uint16_t v = (uint16_t)(VC & 0x7FFu);
        if (v > 48 && v < 400)
            break;
    }
}

void video_wait_vblank(void)
{
#if defined(BEACON_AT) && BEACON_AT == 8
    { extern void hang_beacon(uint16_t); hang_beacon(0x07C0); }   /* BLUE */
#endif
    uint32_t f = frame_count;
#ifdef HANGDIAG
    uint32_t g = 0;
    while (frame_count == f) {
        g++;
        if (g > 8000000u) {               /* the VBL ISR is not running */
            *(volatile uint16_t *)0xF00058u = 0xF81F;   /* MAGENTA: no vblank */
            for (;;) ;
        }
    }
#elif defined(IRQREARM)
    /* A10, the LAST unbounded ISR-dependent wait in the title loop: with
       gpu_sync and the flip both bounded, this is where a dead vblank ISR
       parks the machine - frame_count is ONLY ever incremented by the ISR, so
       "wait for the next field" becomes "wait forever" and nothing is ever
       drawn. Bound it, re-arm the interrupt a few times, then stop waiting: a
       frame that runs unpaced beats a console that never draws at all. */
    { uint32_t g = 0, tries = 0;
      while (frame_count == f) {
          g++;
          if (g > 200000u) {
              g = 0;
              tries++;
              if (tries > 3) return;
              video_rearm_irq();
          }
      } }
#else
    while (frame_count == f)
        ;
#endif
}
