/* jerry.c - Jerry DSP driver (68k side): load kernel, kick, sync.
 * Mirrors gpu.c's proven protocol on the DSP register set. */
#include "jaguar.h"

#define D_FLAGS  REG32(0xF1A100)
#define D_PC     REG32(0xF1A110)
#define D_CTRL   REG32(0xF1A114)
#define D_SRAM   0xF1B000u
#define D_PARAMS 0xF1C240u

#define MAGIC_DSP 0x0D5B0001u
#define D_CMD    (*(volatile uint32_t *)0xF1C338u)  /* resident: 1 pose, 2 roomx */
#define D_VOICES 0xF1C340u                          /* 2 x 7 longs */

static volatile uint32_t dsp_mailbox[4] __attribute__((aligned(16)));

extern const uint8_t dsp_kernel[], dsp_kernel_end[];

int jerry_init(void)
{
    const uint32_t *src = (const uint32_t *)dsp_kernel;
    uint32_t n = (uint32_t)(dsp_kernel_end - dsp_kernel) / 4;
    volatile uint32_t *dst = (volatile uint32_t *)D_SRAM;
    uint32_t i;

    D_CTRL = 0;
    *(volatile uint32_t *)0xF1A100u = 0;   /* D_FLAGS: clear irq state */
    for (i = 0; i < n; i++)
        dst[i] = src[i];
    *(volatile uint32_t *)0xF1A150u = 37;      /* SCLK: 32-BIT reg! ~11kHz */
    *(volatile uint32_t *)0xF1A154u = 0x15;    /* SMODE: 32-BIT reg! I2S   */
    *(volatile uint16_t *)0xF14000u = 0x0100;  /* JOYSTICK: unmute DAC   */
    *(volatile uint32_t *)(D_PARAMS + 0) = (uint32_t)dsp_mailbox;
    *(volatile uint32_t *)(D_PARAMS + 60) = 0;   /* mcount=0 -> hello mode */
    dsp_mailbox[0] = 0;

    D_PC = D_SRAM;
    D_CTRL = 1;
    /* RESIDENT: Jerry runs forever from here (audio ISR + command loop).
       NEVER write D_CTRL/D_PC again — commands go through D_CMD. */
    for (i = 0; i < 400000; i++) {
        if (dsp_mailbox[0] == MAGIC_DSP) return 1;
    }
    return 0;
}

/* one-time upload of the pose constants into Jerry's LOCAL SRAM (his DRAM
 * link is 16-bit + arbitrated: local data = the whole point of the DSP). */
void jerry_pose_setup(const int16_t *skv_c, int nverts,
                      const int16_t *sknode_c, int nnodes,
                      const int32_t *sintab_c)
{
    /* JRISC internal SRAM allows 32-bit access ONLY. skv stays packed s16
       (kernel extracts from longs); nodes expanded to s32; SINTAB copied.
       Map (2026-07-11 relayout for the room-transform mode): kernel $F1B000
       (max 1792) | OUT $F1B700 | MBLK $F1C060 | CAMB $F1C090 | MSTACK
       $F1C0C0 | PARAMS $F1C240 | SKV16 $F1C3C0 | SINTAB $F1CB00 |
       NODES32 $F1CF00. */
    volatile uint32_t *d;
    volatile int32_t  *e;
    const uint32_t *s;
    int i;
    /* resident Jerry idles in his command loop; these tables are not read
       until the first pose command, so live writes are safe */
    d = (volatile uint32_t *)0xF1C3C0u; s = (const uint32_t *)skv_c;
    for (i = 0; i < (nverts*3+1)/2; i++) d[i] = s[i];
    e = (volatile int32_t *)0xF1CF00u;
    for (i = 0; i < nnodes*4; i++) e[i] = sknode_c[i];
    d = (volatile uint32_t *)0xF1CB00u; s = (const uint32_t *)sintab_c;
    for (i = 0; i < 256; i++) d[i] = s[i];
}

/* copy Jerry's posed verts (OUT_D, SRAM) into the draw blob (~600 longs). */
void jerry_pose_read(void *dst, int longs)
{
    const volatile uint32_t *s = (const volatile uint32_t *)0xF1B700u; /* OUT_D */
    uint32_t *d = (uint32_t *)dst;
    int i;
    for (i = 0; i < longs; i++) d[i] = s[i];
}

#define MAGIC_POSE_DONE 0x0D5BD05Eu

void jerry_pose_kick(const void *skv, const void *skvl, const void *sknode,
                     const void *angles, const void *sintab, void *out,
                     int32_t rootx, int32_t rooty, int32_t rootz,
                     int32_t laC, int32_t laS, int32_t rx0, int32_t rz0,
                     int32_t base_y, uint32_t mcount)
{
    volatile uint32_t *p = (volatile uint32_t *)D_PARAMS;
    p[0]  = (uint32_t)dsp_mailbox;
    p[1]  = (uint32_t)skv;
    p[2]  = (uint32_t)skvl;
    p[3]  = (uint32_t)sknode;
    p[4]  = (uint32_t)angles;
    p[5]  = (uint32_t)sintab;
    p[6]  = (uint32_t)out;
    p[7]  = (uint32_t)rootx;
    p[8]  = (uint32_t)rooty;
    p[9]  = (uint32_t)rootz;
    p[10] = (uint32_t)laC;
    p[11] = (uint32_t)laS;
    p[12] = (uint32_t)rx0;
    p[13] = (uint32_t)rz0;
    p[14] = (uint32_t)base_y;
    p[15] = mcount;
    /* angles: DSP byte-reads of DRAM are unreliable (Jaguar quirk) — copy
       the mcount*3 angle bytes into DSP-local params area instead. */
    if (mcount <= 32) {   /* guard: sentinel/test mcounts must NOT overrun
                             the angle window into SKV/SINTAB */
      volatile uint32_t *ab = (volatile uint32_t *)(D_PARAMS + 0x40);
      const uint8_t *s = (const uint8_t *)angles;
      uint32_t i2;   /* one LONG per angle byte: JRISC SRAM = 32-bit only */
      for (i2 = 0; i2 < mcount*3; i2++) ab[i2] = s[i2]; }
    dsp_mailbox[0] = 0;
    D_CMD = 1;                       /* resident: request pose */
}

/* debug: copy arbitrary DSP SRAM (32-bit reads) back to the 68k */
void jerry_read_at(uint32_t src, void *dst, int longs)
{
    const volatile uint32_t *s = (const volatile uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;
    int i;
    for (i = 0; i < longs; i++) d[i] = s[i];
}

/* ROOM-TRANSFORM mode: Jerry transforms room verts into per-room caches
   (list: count, then per room {vertptr, vcount, offX<<8, offZ<<8, cacheptr};
   flag long at cacheptr-4 is set to 2 when that room completes). mcount
   sentinel 0xFFFE selects the mode. */
void jerry_roomx_kick(const void *list, const void *camblk)
{
    volatile uint32_t *p = (volatile uint32_t *)D_PARAMS;
    p[0]  = (uint32_t)dsp_mailbox;
    p[1]  = (uint32_t)list;
    p[2]  = (uint32_t)camblk;
    p[15] = 0xFFFEu;
    dsp_mailbox[0] = 0;
    D_CMD = 2;                       /* resident: request room transform */
}

/* fire a one-shot PCM sample on a mixer voice (0..1). step = 16.16 pitch
   (11025Hz source at the ~16kHz mixer: 45211). Write order matters: end=0
   deactivates while we rewrite, then the final end write arms the voice. */
void jerry_sfx(int voice, const void *pcm, uint32_t bytes, uint32_t step)
{
    volatile uint32_t *v = (volatile uint32_t *)(D_VOICES + voice*16);
    (void)step;                      /* native-rate mixer: 1 byte per tick */
    v[0] = 0;                        /* CNT=0: deactivate while rewriting  */
    v[1] = 0;                        /* LANE */
    v[2] = (uint32_t)pcm;            /* PTR (long-aligned by the extractor) */
    v[3] = 0;                        /* CVAL */
    v[0] = bytes;                    /* CNT: arms the voice */
}

/* queue the NEXT buffer for gapless voice-0 chaining: the DSP pump
   promotes it the instant the current buffer drains (zero-sample gap).
   PTR first; the NCNT write arms the slot. */
void jerry_sfx_queue(const void *pcm, uint32_t bytes)
{
    *(volatile uint32_t *)0xF1C37Cu = (uint32_t)pcm;
    *(volatile uint32_t *)0xF1C378u = bytes;
}

int jerry_pose_sync(void)
{
    uint32_t i;
    for (i = 0; i < 4000000; i++) {
        if (dsp_mailbox[0] == MAGIC_POSE_DONE) return 1;
    }
    return 0;
}
