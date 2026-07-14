#ifndef JAG_BLIT_H
#define JAG_BLIT_H

#include <stdint.h>

/* Fill inclusive horizontal span [x0,x1] on line y with colour c.
 * Short spans go on the CPU; longer ones use the Blitter. */
void blit_span(uint16_t *fb, int y, int x0, int x1, uint16_t c);

/* Fill rows [y0,y1) with colour c via the Blitter (frame clear). */
void blit_band(void *fb, int y0, int y1, uint32_t c);

#ifdef HALFRES
/* Line-double an 8bpp RENDER_W x srch source into a RENDER_W x 2*srch dest. */
void blit_double(const void *src, void *dst, int srch);
#endif

#endif
