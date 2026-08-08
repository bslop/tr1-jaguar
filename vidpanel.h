#ifndef JAG_VIDPANEL_H
#define JAG_VIDPANEL_H
#include <stdint.h>
/* self-calibrating read-out panel; see vidpanel.c */
uint32_t vp_tick(void);                 /* VC half-line clock, ~31.7us */
void     vp_clut(void);                 /* force CLUT 254=black 255=white */
void     vp_paint(uint8_t *fb, const uint8_t *lamps, const uint32_t *rows);
#endif
