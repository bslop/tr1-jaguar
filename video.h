#ifndef JAG_VIDEO_H
#define JAG_VIDEO_H

#include <stdint.h>

/* Display: 320x240, 16bpp Jaguar RGB, triple buffered.
 *
 * LOWRES build (make ... LOWRES=1): the framebuffer is HALF HEIGHT (320x120)
 * and the Object Processor's hardware VERTICAL scaler (2.0x) stretches it to
 * the full 240-line window during scanout - halving the Blitter fill (the
 * measured bottleneck) with no per-frame cost.  RENDER_W stays 320: horizontal
 * OP scaling starves the Jaguar bus ~15-20x and blacks the display, so ONLY the
 * vertical axis is scaled (see video.c build_object_list). */
#define RENDER_W 320
/* HRESN=N (2026-09-05) - THE HORIZONTAL DIAL, and it is NOT the OP scaler.
 * Render only the leftmost N columns of each RENDER_W-byte row and widen the
 * PIXEL CLOCK (VMODE PWIDTH) so those N pixels fill the same active line.
 *
 * ☠☠ DO NOT reach for HSCALE here.  The comment in build_object_list is right
 * and is about a DIFFERENT mechanism: the OP's horizontal SCALER holds the bus
 * for most of the active display (~15-20x) and blacks the screen.  PWIDTH is
 * the opposite - fewer, wider pixels per line means FEWER OP FETCHES, so it is
 * a bus WIN.  [HW] jag_quake has shipped VMODE=$0EC7 (PWIDTH field 7) since
 * 2026-08-12 across 3,273 capture grabs: full active width, hard edges, no
 * stretch artifact.
 *
 * ★ The STRIDE stays RENDER_W.  Only IWIDTH (what the OP fetches per line)
 * shrinks; DWIDTH (the row step) does not.  That keeps the title, ring menu,
 * loading art and the FMV decoder - all of which assume a 320-byte row - and
 * leaves BSS byte-identical, so this does NOT re-roll the A10 boot lottery.
 * ★ 160x120 with the OP's 2.0x VERTICAL scale is exactly ASPECT-NEUTRAL:
 * PWIDTH's 2x horizontal and the OP's 2x vertical cancel, so FOCAL and FOCAL_Y
 * come out EQUAL (95) and no projection fudge is needed.  It is LESS
 * anisotropic than the 320x60 build it replaces. */
#ifdef HRESN
#define VIEW_W HRESN
#else
#define VIEW_W RENDER_W
#endif
/* VRES60 (2026-07-29): render 60 lines and let the OP scaler stretch 4.0x to
   the 240-line window, exactly as LOWRES does at 2.0x.  Spans are emitted PER
   SCANLINE, so halving the scanlines halves the span count - and every
   measurement this project has (Blitter busy on 92% of polls, ~19.5 cyc per
   launch, launch count unchanged by every per-pixel trick tried) says the
   launch count IS the frame time.  LOWRES 240->120 bought 6.00->7.50 fps;
   120->60 targets 7.50 -> 15.00 (8 vsync fields -> 4). */
/* VRESN=N (2026-08-11) generalises VRES60 into a RESOLUTION DIAL: render N
 * lines and let the OP scaler stretch them over the same 240-line window.
 * ☠️ ONLY 120/96/80/64/60 ARE AVAILABLE.  The OP's VSCALE is 3.5 fixed point
 * (0x20 = 1.0x), so the scale that fills the window, 240/N, must land on a
 * multiple of 1/32 -> VSCALE = 7680/N must be a whole number.  72 lines (the
 * band height that measured +7.8%) is NOT expressible: 3.333x would leave the
 * bottom of the frame short.  The Makefile refuses anything off the ladder. */
#ifdef VRESN
#define RENDER_H VRESN
#elif defined(VRES60)
#define RENDER_H 60
#elif defined(LOWRES)
#define RENDER_H 120
#else
#define RENDER_H 240
#endif

/* HALFRES build (make ... HALFRES=1): render at RENDER_H=120 (LOWRES render
 * path, half the fill) but DISPLAY at 240 by Blitter LINE-DOUBLING the 120-line
 * render into a normal 320x240 buffer shown through the UNSCALED OP path (the OP
 * scaler goes black under heavy multiroom fill; the unscaled path survives it).
 * DISPLAY_H is the on-screen height; RENDER_H stays the render-buffer height. */
#ifdef HALFRES
#define DISPLAY_H 240
#else
#define DISPLAY_H RENDER_H
#endif

/* VIEWH=N (make ... VIEWH=90): DOOM-STYLE SHORT BAND.  Render the world into
 * only the TOP N lines of the RENDER_H-line buffer and leave the rest black -
 * that is where a status bar would go.  The buffer, the OP object and the
 * display window are UNCHANGED; only the extent the game draws into moves.
 *
 * This is a CROP, not a squash: CENTER_Y follows VIEW_H but FOCAL_Y does not,
 * so the vertical field of view shrinks with the band and faces outside it are
 * rejected by the edge walk instead of rasterized.  Scaling FOCAL_Y too would
 * keep the FOV and squash the world - same pixels, no front-end saving, wrong
 * aspect.  ☠️ The kernel carries its OWN copy of these constants
 * (gpu_geotex.gas); VIEWH is passed to rmac as well and both must agree. */
#ifndef VIEW_H
#define VIEW_H RENDER_H
#endif

#ifdef __cplusplus
extern "C" {
#endif

void      video_init(void);
void     *video_backbuffer(void);   /* buffer the CPU renders into        */
void      video_flip(void);         /* queue back buffer for next vblank  */
void     *video_backbuffer_hi(void);/* 320x240 direct target (titles)     */
void      video_flip_hi(void *db);  /* queue a hi buffer, no doubling     */
void      video_set_clut(const uint16_t *pal);  /* FB8: load OP CLUT      */
void      video_wait_vblank(void);  /* block until the next vblank        */

extern volatile uint32_t frame_count;
extern volatile uint32_t video_pend_at;
extern volatile uint32_t pending_fb;

void video_dump_oplist(void);   /* dump OP list to skunk console (debug) */

#ifdef __cplusplus
}
#endif

#endif
