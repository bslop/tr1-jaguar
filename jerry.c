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

/* [0] pose/overlay done magic, [3] published voice count (see jerry_v0_ncnt),
   [4..7] OVERLAY params - see the warning in the JOVL block below. */
/* ☠☠ SIZED 10, NOT 8. The overlay protocol uses slots 0..9: OVL_M_PROBE is
   mailbox[8] and OVL_M_NPROBE is mailbox[9] (see the defines below), and the
   DSP side reads exactly those offsets — dsp_ovl_ent.das:181 `addq #32,r0`
   and :183 `addq #4,r0`. Declared [8], the four C writes at OVL_M_PROBE /
   OVL_M_NPROBE landed PAST THE END of the array and the DSP read them back
   from that same out-of-bounds memory. It worked only because whatever the
   linker placed next in BSS was being used consistently by both sides.
   gcc had been reporting it all along: "array subscript 8 is above array
   bounds of volatile uint32_t[8]", four times per build. */
static volatile uint32_t dsp_mailbox[10] __attribute__((aligned(16)));

extern const uint8_t dsp_kernel[], dsp_kernel_end[];

int jerry_init(void)
{
#if defined(BEACON_AT) && BEACON_AT == 4
    { extern void hang_beacon(uint16_t); hang_beacon(0x07FE); }   /* CYAN */
#endif
#ifdef HANGDIAG
    *(volatile uint16_t *)0xF00058u = (uint16_t)0x07FE;
#endif
    const uint32_t *src = (const uint32_t *)dsp_kernel;
    uint32_t n = (uint32_t)(dsp_kernel_end - dsp_kernel) / 4;
    volatile uint32_t *dst = (volatile uint32_t *)D_SRAM;
    uint32_t i;

    D_CTRL = 0;
    *(volatile uint32_t *)0xF1A100u = 0;   /* D_FLAGS: clear irq state */
    for (i = 0; i < n; i++)
        dst[i] = src[i];
#ifndef NOSOUND
    *(volatile uint32_t *)0xF1A150u = 37;      /* SCLK: 32-BIT reg! ~11kHz */
    *(volatile uint32_t *)0xF1A154u = 0x15;    /* SMODE: 32-BIT reg! I2S   */
    *(volatile uint16_t *)0xF14000u = 0x0100;  /* JOYSTICK: unmute DAC   */
#endif  /* NOSOUND: never start the DAC clock, so no ticks for Jerry to service
         * (the DSP kernel also compiles AUDIO_PUMP out — see dsp_pose.das) */
    /* ☠️ SILENCE THE VOICES BEFORE THE DAC EVER TICKS.
       The voice registers live in Jerry's SRAM and hold POWER-ON GARBAGE.
       jerry_init programs SCLK/SMODE and unmutes the DAC, then starts the
       DSP - and the pump immediately begins mixing from a garbage V0_PTR
       for a garbage V0_CNT bytes. Measured on silicon: 3.7 SECONDS of flat
       ~500-RMS buzz at the head of the first clip (the source audio there
       is RMS 1-80 and rising), which the user heard as the clip "trying to
       start". Zero them here, while the DSP is still stopped and the 68k
       can write his SRAM safely. V0 = 4 longs, V1_CNT stops voice 1, and
       the queued-buffer slot must start empty too.
       ☠️ Do NOT blanket-clear 14 longs from D_VOICES: PUMP_SAVE ($F1C360)
       overlaps voice 1's tail. */
    *(volatile uint32_t *)0xF1C340u = 0;   /* V0_CNT  */
    *(volatile uint32_t *)0xF1C344u = 0;   /* V0_LANE */
    *(volatile uint32_t *)0xF1C348u = 0;   /* V0_PTR  */
    *(volatile uint32_t *)0xF1C34Cu = 0;   /* V0_CVAL */
    *(volatile uint32_t *)0xF1C350u = 0;   /* V1_CNT  */
    *(volatile uint32_t *)0xF1C378u = 0;   /* V0_NCNT */
    *(volatile uint32_t *)0xF1C37Cu = 0;   /* V0_NPTR */
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

/* Jerry's voice state, read from the DRAM MAILBOX he publishes into - never
   from his local SRAM. The 68k cannot reliably read a running JRISC's SRAM
   (proved on Tom, then heard on Jerry as a machine-gun re-trigger), so these
   are the only honest way to ask "is the voice idle / is the queue free". */
/* Mark the published state STALE. Jerry only refreshes it every 64 DAC
   ticks, so for a few ms after the 68k arms or queues a buffer the mailbox
   still holds the PRE-WRITE values - read them and the player concludes the
   voice is idle and re-arms, restarting the buffer. That is the stutter at
   the start of a clip. Callers must ignore the counters until Jerry has
   published over this sentinel. */
#define JERRY_AUD_STALE 0xFFFFFFFFu
void jerry_audio_stale(void)
{
    dsp_mailbox[2] = JERRY_AUD_STALE;
    dsp_mailbox[3] = JERRY_AUD_STALE;
}

uint32_t jerry_v0_cnt(void)  { return dsp_mailbox[2]; }
uint32_t jerry_v0_ncnt(void) { return dsp_mailbox[3]; }

int jerry_pose_sync(void)
{
    uint32_t i;
    for (i = 0; i < 4000000; i++) {
        if (dsp_mailbox[0] == MAGIC_POSE_DONE) return 1;
    }
    return 0;
}

#ifdef JOVL
/* ---- JERRY CODE OVERLAYS -------------------------------------------------
 * Jerry's code window is 4192 bytes and the resident kernel fills most of it,
 * so work moves across one PHASE at a time: the image is copied into the free
 * tail once (jerry_ovl_load), then CALLED per frame (jerry_ovl_run).
 * ☠️ The audio pump does not run while an overlay executes - keep jobs small
 * or the DAC underruns (crackle, not a hang). */
#define MAGIC_OVL_DONE 0x0D5BD0E0u      /* must match dsp_ovl_ent.das */
/* ☠️ Overlay params get their OWN block, NOT D_PARAMS[16..]: jerry_pose_kick
   copies Lara's angle bytes to D_PARAMS+0x40 one LONG per byte (~45 longs), so
   params[16..60] are rewritten by every pose.  Parking the job pointer there
   fed the overlay garbage and drew a spike to infinity on silicon. */
/* ☠️☠️ These live in the DRAM MAILBOX, not in Jerry's SRAM.  Two SRAM homes
   were tried and both were occupied:
     - D_PARAMS[16..] is rewritten by every pose kick (the angle copy);
     - $F1CB00 lands INSIDE SKV_D, whose comment says 1800B but which actually
       holds g_lnv*6 bytes (~3000 for Lara, reaching ~$F1CF78) - writing there
       corrupted her mesh and stretched an arm across the screen on silicon.
   The mailbox address is already handed to Jerry in params[0], so the kernel
   and the overlay both reach it with one indirection and it has no neighbours. */
#define OVL_M_SRC   4                   /* mailbox[4] = overlay image src   */
#define OVL_M_LONGS 5                   /* mailbox[5] = image length, longs */
#define OVL_M_JOBS  6                   /* mailbox[6] = job list address    */
#define OVL_M_NJOB  7                   /* mailbox[7] = job count           */
#define OVL_M_PROBE 8                   /* mailbox[8] = floor-probe list     */
#define OVL_M_NPROBE 9                  /* mailbox[9] = probe count          */

extern const uint8_t dsp_ovl_ent[], dsp_ovl_ent_end[];

void jerry_ovl_load(void)
{
    dsp_mailbox[OVL_M_SRC]   = (uint32_t)dsp_ovl_ent;
    dsp_mailbox[OVL_M_LONGS] = (uint32_t)(dsp_ovl_ent_end - dsp_ovl_ent) / 4;
    D_CMD = 3;
    /* ☠️ Wait until Jerry has CONSUMED the command (it zeroes CMD_D on entry).
       Without this the next kick can overwrite CMD_D before the resident loop
       ever sees the 3, and the overlay is silently never loaded - which shows
       up much later as garbage vertices, not as a failure here. */
    { uint32_t i; for (i = 0; i < 2000000 && D_CMD != 0; i++) ; }
}

/* jobs: n * 8 longs {src, count, cos, sin, rx0, y, rz0, dst} */
void jerry_ovl_run(const void *jobs, uint32_t njobs)
{
    dsp_mailbox[OVL_M_JOBS] = (uint32_t)jobs;
    dsp_mailbox[OVL_M_NJOB] = njobs;
    /* ☠️ The overlay ALWAYS runs its floor-probe phase after the entity phase,
       so this count must be a real number every time.  Left uninitialised it
       is garbage and Jerry loops over it writing results to random DRAM. */
    dsp_mailbox[OVL_M_PROBE]  = 0;
    dsp_mailbox[OVL_M_NPROBE] = 0;
    dsp_mailbox[0] = 0;
    D_CMD = 4;
}

/* Queue floor probes (4 longs each: d, lx, lz, out).  ☠️ Call AFTER
   jerry_ovl_run, which zeroes these slots so a stale count can never be
   executed - run() arms the entity phase, this arms the probe phase, then
   CMD=4 fires both. */
void jerry_ovl_probes(const void *probes, uint32_t n)
{
    dsp_mailbox[OVL_M_PROBE]  = (uint32_t)probes;
    dsp_mailbox[OVL_M_NPROBE] = n;
}

int jerry_ovl_sync(void)
{
    uint32_t i;
    for (i = 0; i < 4000000; i++)
        if (dsp_mailbox[0] == MAGIC_OVL_DONE) return 1;
    return 0;
}
#endif /* JOVL */
