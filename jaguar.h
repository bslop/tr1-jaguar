/*
 * jaguar.h - Atari Jaguar hardware registers
 *
 * Register addresses follow the standard jaguar.inc naming from the
 * Atari developer kit. Tom (video) at $F00000+, Jerry (audio/DSP) at
 * $F10000+, joystick at $F14000. Pixel format and resolution live in
 * core/render.h - this header is hardware only.
 */
#ifndef JAGUAR_H
#define JAGUAR_H

#include <stdint.h>

#define REG16(addr) (*(volatile uint16_t *)(addr))
#define REG32(addr) (*(volatile uint32_t *)(addr))

/* --- Tom: memory / video --- */
#define MEMCON1 REG16(0xF00000)
#define MEMCON2 REG16(0xF00002)
#define HC      REG16(0xF00004)   /* horizontal count            */
#define VC      REG16(0xF00006)   /* vertical count (half-lines) */

#define OLP     REG32(0xF00020)   /* object list pointer (word-swapped!) */
#define OBF     REG16(0xF00026)   /* object processor flag       */
#define VMODE   REG16(0xF00028)   /* video mode                  */
#define BORD1   REG16(0xF0002A)   /* border red/green            */
#define BORD2   REG16(0xF0002C)   /* border blue                 */
#define HP      REG16(0xF0002E)   /* horizontal period           */
#define HBB     REG16(0xF00030)   /* horizontal blank begin      */
#define HBE     REG16(0xF00032)   /* horizontal blank end        */
#define HS      REG16(0xF00034)   /* horizontal sync             */
#define HVS     REG16(0xF00036)   /* horizontal vertical sync    */
#define CONFIG  REG16(0xF00036)   /* read: config, bit 4 1=NTSC  */
#define HDB1    REG16(0xF00038)   /* horizontal display begin 1  */
#define HDB2    REG16(0xF0003A)   /* horizontal display begin 2  */
#define HDE     REG16(0xF0003C)   /* horizontal display end      */
#define VP      REG16(0xF0003E)   /* vertical period             */
#define VBB     REG16(0xF00040)   /* vertical blank begin        */
#define VBE     REG16(0xF00042)   /* vertical blank end          */
#define VS      REG16(0xF00044)   /* vertical sync               */
#define VDB     REG16(0xF00046)   /* vertical display begin      */
#define VDE     REG16(0xF00048)   /* vertical display end        */
#define VEB     REG16(0xF0004A)   /* vertical equalization begin */
#define VEE     REG16(0xF0004C)   /* vertical equalization end   */
#define VI      REG16(0xF0004E)   /* vertical interrupt line     */
#define BG      REG16(0xF00058)   /* background color            */

#define INT1    REG16(0xF000E0)   /* CPU interrupt control       */
#define INT2    REG16(0xF000E2)   /* CPU interrupt resume        */

/* --- Blitter (register set per the official SDK JAGUAR.INC) --- */
#define A1_BASE  REG32(0xF02200)  /* dest base, phrase aligned   */
#define A1_FLAGS REG32(0xF02204)
#define A1_PIXEL REG32(0xF0220C)  /* y << 16 | x                 */
#define A1_STEP  REG32(0xF02210)  /* per-line pointer step       */
#define A2_BASE  REG32(0xF02224)  /* source base, phrase aligned */
#define A2_FLAGS REG32(0xF02228)
#define A2_PIXEL REG32(0xF02230)  /* y << 16 | x                 */
#define A2_STEP  REG32(0xF02234)  /* per-line pointer step       */
#define B_CMD    REG32(0xF02238)  /* write: start; read: status  */
#define B_COUNT  REG32(0xF0223C)  /* lines << 16 | pixels        */
#define B_SRCD   REG32(0xF02240)  /* source data (fill pattern)  */
#define B_SRCD1  REG32(0xF02244)

/* Bit values from the official SDK JAGUAR.INC. Do not copy them from
 * third-party platform headers: a common one has UPDA1 as UPDA2 (bit 10 vs
 * the real bit 9) and its "LFU_B" is LFU_N_SXORD - NOT(src XOR dst) -
 * which inverts fills depending on stale destination-register state.
 * That combination sprayed 0xFFFF over low RAM here before diagnosis. */
#define BLIT_IDLE    0x00000001u  /* B_CMD read: blitter idle      */
#define BLIT_UPDA1   0x00000200u  /* d09: step A1 by A1_STEP/line  */
#define BLIT_UPDA2   0x00000400u  /* d10: step A2 by A2_STEP/line  */
#define BLIT_LFU_REP 0x01800000u  /* LFU_SAND|LFU_SAD: output = src */
/* mem->mem copy: SRCEN|LFU_REPLACE|DSTA2 (A1 read as source, A2 written as
 * dest) — the SAME command the textured kernel uses (gpu_geotex BCMD_TEX). */
#define BLIT_CMD_COPY 0x01800801u
#define BLIT_PIX16   0x00000020u  /* A1_FLAGS: PIXEL16             */
#define BLIT_PIX8    0x00000018u  /* A1_FLAGS: PIXEL8 (3<<3)       */
#define BLIT_XPIX    0x00010000u  /* A1_FLAGS: XADDPIX             */
#define BLIT_WID320  0x00004200u  /* A1_FLAGS: official WID320     */

/* --- Joystick --- */
#define JOYSTICK REG16(0xF14000)
#define JOYBUTS  REG16(0xF14002)
/* JOYSTICK bit 8 = audio un-mute. The joypad scan strobes (0x81Fx)
 * all keep bit 8 set, so scanning never re-mutes the DAC (verified
 * pattern: this scan coexists with streaming audio). */
#define JOY_UNMUTE 0x0100u

/* --- Jerry: DSP + I2S DAC (registers per the SDK JAGUAR.INC) --- */
#define D_FLAGS  REG32(0xF1A100)  /* DSP flags: IMASK + irq enable/clear */
#define D_PC     REG32(0xF1A110)  /* DSP program counter (entry)   */
#define D_CTRL   REG32(0xF1A114)  /* DSP control: bit0 DSPGO = run */
#define LTXD     REG32(0xF1A148)  /* I2S left  DAC sample (write)  */
#define RTXD     REG32(0xF1A14C)  /* I2S right DAC sample (write)  */
#define SCLK     REG32(0xF1A150)  /* serial clock divider -> rate  */
#define SMODE    REG32(0xF1A154)  /* serial mode (I2S framing)     */

#define DC_DSPGO    0x00000001u   /* D_CTRL d00: run the DSP       */
#define SM_INTERNAL 0x00000001u   /* SMODE d00: internal serial clock */
#define SM_WSEN     0x00000004u   /* SMODE d02: word-strobe enable */
#define SM_RISING   0x00000010u   /* SMODE d04: clock on rising edge */

/* 68k vector 64 ($100) - all Jaguar interrupts arrive here */
#define JAG_AUTOVEC (*(volatile uint32_t *)0x100)

#endif /* JAGUAR_H */
