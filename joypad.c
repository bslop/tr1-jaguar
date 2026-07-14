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
    return out;
}
