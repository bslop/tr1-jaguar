#ifndef JAG_BLIT_H
#define JAG_BLIT_H

#include <stdint.h>

/* Fill inclusive horizontal span [x0,x1] on line y with colour c.
 * Short spans go on the CPU; longer ones use the Blitter. */
void blit_span(uint16_t *fb, int y, int x0, int x1, uint16_t c);

/* Fill rows [y0,y1) with colour c via the Blitter (frame clear). */
void blit_band(void *fb, int y0, int y1, uint32_t c);

/* full-width 8bpp image copy, src -> dst, h rows (Blitter, not a 68k loop) */
void blit_copy(const void *src, void *dst, int h);
void blit_rect(const void *src, void *dst, int x0, int y0, int w, int h);
/* same move in PHRASE mode (8 bytes per step instead of one pixel); returns
   0 if the Blitter never reported idle */
int  blit_copy_phrase(const void *src, void *dst, int h);

/* linear block move (phrase mode).  Returns the number of bytes the Blitter
   actually moved - 0 if it declined (misalignment) or never idled, so the
   caller must always keep a fallback for the remainder.  Safe for overlapping
   moves DOWNWARD only (src above dst). */
unsigned blit_bytes(const void *src, void *dst, unsigned n);

#ifdef HALFRES
/* Line-double an 8bpp RENDER_W x srch source into a RENDER_W x 2*srch dest. */
void blit_double(const void *src, void *dst, int srch);
#endif

#endif
