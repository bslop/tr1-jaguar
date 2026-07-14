#ifndef JAG_JOYPAD_H
#define JAG_JOYPAD_H

#include <stdint.h>

/* Button bits returned by joypad_read (controller port 1). */
#define PAD_UP     0x0001
#define PAD_DOWN   0x0002
#define PAD_LEFT   0x0004
#define PAD_RIGHT  0x0008
#define PAD_A      0x0010
#define PAD_B      0x0020
#define PAD_C      0x0040
#define PAD_PAUSE  0x0080
#define PAD_OPTION 0x0100

uint32_t joypad_read(void);

#endif
