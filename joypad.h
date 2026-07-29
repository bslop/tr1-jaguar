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
/* JAGUAR PRO (6-BUTTON) PAD — X/Y/Z (2026-07-28).
   Each of the four strobes returns 4 row bits + 2 FIRE bits, so the matrix
   carries EIGHT fire bits: 29,28 / 25,24 / 13,12 / 9,8. The 3-button pad uses
   only five (A=29 Pause=28 B=25 C=13 Option=9); 24, 12 and 8 are unused, and
   the Pro pad adds exactly three face buttons. Decoding them costs nothing on
   a 3-button pad (the bits simply never assert).
   *** BIT ASSIGNMENT IS INFERRED, NOT YET CONFIRMED ON HARDWARE — verify with
   the PADPROBE build before relying on which of X/Y/Z is which. *** */
#define PAD_X      0x0200
#define PAD_Y      0x0400
#define PAD_Z      0x0800

uint32_t joypad_read(void);

#endif
