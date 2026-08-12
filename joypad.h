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

/* NUMERIC KEYPAD (2026-08-11). EVERY Jaguar controller has these twelve keys -
   the 3-button pad included - so they are the way an action stays reachable
   when the Pro pad's X/Y/Z are not there. The matrix already scans them: each
   strobe returns FOUR row bits, and only strobe 0's four are used (the dpad).
   Strobes 1/2/3 carry the keypad columns and were being thrown away.
   ☠️ WHICH row is which KEY is the standard layout, NOT something this project
   has confirmed - build PADPROBE=1 and press them. Same caution as X/Y/Z. */
#define PAD_K1     0x00001000u
#define PAD_K2     0x00002000u
#define PAD_K3     0x00004000u
#define PAD_K4     0x00008000u
#define PAD_K5     0x00010000u
#define PAD_K6     0x00020000u
#define PAD_K7     0x00040000u
#define PAD_K8     0x00080000u
#define PAD_K9     0x00100000u
#define PAD_KSTAR  0x00200000u
#define PAD_K0     0x00400000u
#define PAD_KHASH  0x00800000u

/* PADPROBE=1: joypad_probe() returns the RAW accumulated matrix word so a
   build can show which bits a real key actually asserts. Nothing else may
   assume the mappings above until that has been done on hardware. */
uint32_t joypad_probe(void);

/* ---- ACTION MAP (2026-08-11) -------------------------------------------
 * User: "base the actions Lara does around the Jaguar 6 button controller;
 * this should also map to the number matrix on the 3 button controller."
 * So every action has a Pro-pad face button AND a keypad key, and the keypad
 * half is what keeps the game fully playable on a standard 3-button pad.
 *
 *   JUMP    A   / 2      ROLL    Y   / 5
 *   ACTION  B   / 1      LOOK    Z   / 6
 *   WALK    C   / 3      DRAW    X   / 4
 *
 * ★ TR1 HAS NO SEPARATE FIRE BUTTON: ACTION fires when the pistols are out
 * and grabs/uses when they are not. Keeping that means one less binding and
 * it is what the original does. */
#define ACT_JUMP   (PAD_A | PAD_K2)
#define ACT_ACTION (PAD_B | PAD_K1)
#define ACT_WALK   (PAD_C | PAD_K3)
#define ACT_DRAW   (PAD_X | PAD_K4)
#define ACT_ROLL   (PAD_Y | PAD_K5 | PAD_PAUSE)
#define ACT_LOOK   (PAD_Z | PAD_K6)

uint32_t joypad_read(void);

#endif
