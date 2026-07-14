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
#ifdef LOWRES
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

void video_dump_oplist(void);   /* dump OP list to skunk console (debug) */

#ifdef __cplusplus
}
#endif

#endif
