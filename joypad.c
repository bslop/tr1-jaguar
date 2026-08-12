/*
 * joypad.c - Jaguar controller scan, port 1.
 *
 * The only scan verified on
 * real hardware. A 32-bit read of $F14000 puts JOYSTICK in the high
 * word, JOYBUTS in the low word; row data is active-low in bits
 * 27:24, fire buttons in bits 1:0. Four strobes, each nibble rotated
 * to its own range and AND-accumulated, then inverted to active-high.
 */
#include "jaguar.h"
#include "joypad.h"

#define JOY32 (*(volatile uint32_t *)0xF14000)

#define RAW_UP     (1u << 20)
#define RAW_DOWN   (1u << 21)
#define RAW_LEFT   (1u << 22)
#define RAW_RIGHT  (1u << 23)
#define RAW_PAUSE  (1u << 28)
#define RAW_A      (1u << 29)
#define RAW_B      (1u << 25)
#define RAW_C      (1u << 13)
#define RAW_OPTION (1u << 9)
/* Pro-pad face buttons: the three fire bits the 3-button pad never asserts.
   ☠️ INFERRED from the matrix shape, not confirmed - see joypad.h. */
#define RAW_X      (1u << 24)
#define RAW_Y      (1u << 12)
#define RAW_Z      (1u << 8)
/* Keypad: strobes 1/2/3 each contribute four ROW bits that were discarded.
   Column 1 -> 19:16, column 2 -> 7:4, column 3 -> 3:0. Row order within a
   column is the standard layout and is UNVERIFIED HERE. */
#define RAW_K1     (1u << 16)
#define RAW_K4     (1u << 17)
#define RAW_K7     (1u << 18)
#define RAW_KSTAR  (1u << 19)
#define RAW_K2     (1u << 4)
#define RAW_K5     (1u << 5)
#define RAW_K8     (1u << 6)
#define RAW_K0     (1u << 7)
#define RAW_K3     (1u << 0)
#define RAW_K6     (1u << 1)
#define RAW_K9     (1u << 2)
#define RAW_KHASH  (1u << 3)

static inline uint32_t ror32(uint32_t v, int n)
{
    return (v >> n) | (v << (32 - n));
}

static inline uint32_t strobe(uint16_t sel)
{
    JOYSTICK = sel | 0x0100u;   /* bit 8 = audio unmute: EVERY write must
                                   carry it or the pad scan re-mutes the DAC */
    return JOY32 | 0xF0FFFFFCu;
}

uint32_t joypad_read(void)
{
    uint32_t acc = 0xFFFFFFFFu;
    uint32_t raw, out = 0;

    acc &= ror32(strobe(0x81FE), 4);
    acc &= ror32(strobe(0x81FD), 8);
    acc &= ror32(strobe(0x81FB), 32 - 12);
    acc &= ror32(strobe(0x81F7), 32 - 8);
    raw = ~acc;

    if (raw & RAW_UP)     out |= PAD_UP;
    if (raw & RAW_DOWN)   out |= PAD_DOWN;
    if (raw & RAW_LEFT)   out |= PAD_LEFT;
    if (raw & RAW_RIGHT)  out |= PAD_RIGHT;
    if (raw & RAW_A)      out |= PAD_A;
    if (raw & RAW_B)      out |= PAD_B;
    if (raw & RAW_C)      out |= PAD_C;
    if (raw & RAW_PAUSE)  out |= PAD_PAUSE;
    if (raw & RAW_OPTION) out |= PAD_OPTION;
    if (raw & RAW_X)      out |= PAD_X;
    if (raw & RAW_Y)      out |= PAD_Y;
    if (raw & RAW_Z)      out |= PAD_Z;
    if (raw & RAW_K1)     out |= PAD_K1;
    if (raw & RAW_K2)     out |= PAD_K2;
    if (raw & RAW_K3)     out |= PAD_K3;
    if (raw & RAW_K4)     out |= PAD_K4;
    if (raw & RAW_K5)     out |= PAD_K5;
    if (raw & RAW_K6)     out |= PAD_K6;
    if (raw & RAW_K7)     out |= PAD_K7;
    if (raw & RAW_K8)     out |= PAD_K8;
    if (raw & RAW_K9)     out |= PAD_K9;
    if (raw & RAW_K0)     out |= PAD_K0;
    if (raw & RAW_KSTAR)  out |= PAD_KSTAR;
    if (raw & RAW_KHASH)  out |= PAD_KHASH;
    return out;
}

/* RAW matrix word, for PADPROBE. ☠️ X/Y/Z and every keypad bit above are
   INFERRED. Read them off hardware with this before trusting the table. */
uint32_t joypad_probe(void)
{
    uint32_t acc = 0xFFFFFFFFu;
    acc &= ror32(strobe(0x81FE), 4);
    acc &= ror32(strobe(0x81FD), 8);
    acc &= ror32(strobe(0x81FB), 32 - 12);
    acc &= ror32(strobe(0x81F7), 32 - 8);
    return ~acc;
}
