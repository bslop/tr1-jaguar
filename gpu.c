/*
 * gpu.c - Tom span-fill driver (68k side).
 *
 * Uploads gpu_spanfill.gas once; each frame the 68k hands Tom a DRAM
 * span list and Tom programs the Blitter for every span from SRAM
 * (far faster than the 68k doing per-span MMIO + idle spins). Launch
 * protocol and DRAM-mailbox handshake are the proven Jaguar idiom.
 */
#include "jaguar.h"
/* privileged-instruction wrappers (cpu68k.S) — inline asm is outside
   jcc68k's language subset */
void cpu_irq_on(void);
void cpu_stop_sleep(void);
int  cpu_stop_unless(volatile uint32_t *addr, uint32_t val);

#include "gpu.h"


#define G_CTRL   REG32(0xF02114)
#define G_PC     REG32(0xF02110)
#define G_SRAM   0xF03000u
#define G_PARAMS 0xF03F00u

#define MAGIC_DONE 0x0A3DD05Eu

static volatile uint32_t mailbox[4] __attribute__((aligned(16)));

extern const uint8_t gpu_kernel[], gpu_kernel_end[];

int gpu_init(void)
{
#if defined(BEACON_AT) && BEACON_AT == 3
    { extern void hang_beacon(uint16_t); hang_beacon(0xFFC0); }   /* MAGENTA */
#endif
#ifdef HANGDIAG
    *(volatile uint16_t *)0xF00058u = (uint16_t)0xFFC0;
#endif
    const uint32_t *src = (const uint32_t *)gpu_kernel;
    uint32_t n = (uint32_t)(gpu_kernel_end - gpu_kernel) / 4;
    volatile uint32_t *dst = (volatile uint32_t *)G_SRAM;
    uint32_t i;

    G_CTRL = 0;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
    *(volatile uint32_t *)(G_PARAMS + 4) = (uint32_t)mailbox;
    /* zero the geomdirect lara-list params so a count=0 self-test on that
     * kernel can't walk a garbage lara pointer */
    *(volatile uint32_t *)(G_PARAMS + 20) = 0;
    *(volatile uint32_t *)(G_PARAMS + 24) = 0;

    /* self-test: an empty batch must round-trip */
    return gpu_spanfill((const uint32_t *)G_SRAM, 0, (uint16_t *)0x10000);
}

int gpu_spanfill(const uint32_t *list, uint32_t count, uint16_t *fb)
{
    uint32_t i;

    G_CTRL = 0;
    *(volatile uint32_t *)(G_PARAMS + 0) = (uint32_t)list;
    *(volatile uint32_t *)(G_PARAMS + 8) = count;
    *(volatile uint32_t *)(G_PARAMS + 12) = (uint32_t)fb;
    mailbox[0] = 0;
    mailbox[1] = 0;
    G_PC = G_SRAM;
    G_CTRL = 1;

    /* Sparse poll: a tight DRAM poll starves Tom's bus so the GPU can't
     * finish (jaguar-shared porting notes). Check the mailbox only every
     * ~register-delay cycles so Tom keeps the bus and drains fast. */
    for (i = 0; i < 40000; i++) {
        volatile uint32_t d;
        for (d = 0; d < 40; d++)
            ;
        if (mailbox[0] == MAGIC_DONE) {
            G_CTRL = 0;
            return 1;
        }
    }
    G_CTRL = 0;
    return 0;
}

void gpu_geomxform_kick(const uint32_t *list, uint32_t count, uint16_t *fb,
                        const uint32_t *camblock)
{
    G_CTRL = 0;
    *(volatile uint32_t *)(G_PARAMS + 0) = (uint32_t)list;
    *(volatile uint32_t *)(G_PARAMS + 8) = count;
    *(volatile uint32_t *)(G_PARAMS + 12) = (uint32_t)fb;
    *(volatile uint32_t *)(G_PARAMS + 16) = (uint32_t)camblock;   /* params[4] */
    mailbox[0] = 0;
    mailbox[1] = 0;
    G_PC = G_SRAM;
    G_CTRL = 1;                                       /* fire, return */
}

uint32_t g_syncspins = 0, g_synccalls = 0;
int gpu_sync(void)
{
    uint32_t i;
    /* STOP-sync: with SINGLE-DISPATCH there is ONE sync spanning the whole
     * room render — a tight DRAM poll here steals bus cycles from Tom for
     * the entire frame. STOP releases the bus completely; the geotex kernel
     * raises CPUINT at alldone, and vblank (60Hz) bounds the worst-case
     * wake. Supervisor mode throughout, so STOP is legal. */
    g_synccalls++;
    for (i = 0; i < 400; i++) {          /* ~6s worst case at vblank wakes */
#ifdef SYNCPOLL
        /* EXPERIMENT: busy-poll instead of STOP. If the GPU actually finishes
           early and only the wake is late, this collapses the frame time. */
        if (mailbox[0] == MAGIC_DONE) {
            G_CTRL = 0;
            return 1;
        }
        g_syncspins++;                   /* how many interrupt-wakes per sync */
        { volatile int _s; for (_s = 0; _s < 200; _s++) ; }
#else
        /* PACING fix: check+STOP must be ATOMIC. The old open-coded
           check-then-cpu_stop_sleep() had a lost-wakeup window — a CPUINT
           raised between the mailbox read and the STOP was taken+acked
           BEFORE the STOP, which then slept into the next VBL with
           MAGIC_DONE already set. Self-locking: a VBL-quantized wake makes
           the next kick VBL-aligned, so a near-constant render time lands
           the finish in the same window every frame (the M68DIET cadence
           lock: maxvbl pinned, frame time insensitive to kernel speed). */
#ifdef IRQREARM
        /* Do NOT bet the frame on an interrupt that may never come: poll with
           a bound, and re-arm the vertical interrupt each round. */
        { uint32_t s_ = 0;
          while (mailbox[0] != MAGIC_DONE && s_ < 100000u) s_++;
          if (mailbox[0] == MAGIC_DONE) { G_CTRL = 0; return 1; }
          { extern void video_rearm_irq(void); video_rearm_irq(); } }
#else
        if (cpu_stop_unless(&mailbox[0], MAGIC_DONE)) {
            G_CTRL = 0;
            return 1;
        }
#endif
        g_syncspins++;                   /* how many interrupt-wakes per sync */
#endif
    }
#ifdef SKUNK_CONSOLE
    /* WEDGE AUTOPSY (RUNBATCH walking-death hunt): a sync timeout means
       the GPU has been stuck ~6s — its PC names the looping code, B_CMD
       says whether it's spinning on the blitter. Prints only on wedge. */
    {
        extern void dbg_kv(const char *k, long v);
        dbg_kv("wedgepc",  (long)*(volatile uint32_t *)0xF02110u);
        dbg_kv("wedgecmd", (long)*(volatile uint32_t *)0xF02238u);
        dbg_kv("wedgectl", (long)*(volatile uint32_t *)0xF02114u);
    }
#endif
#if defined(BEACON_AT) && BEACON_AT == 10
    { extern void hang_beacon(uint16_t); hang_beacon(0xFFC0); }   /* MAGENTA: gpu_sync TIMED OUT - Tom wedged */
#endif
    G_CTRL = 0;
    return 0;
}

int gpu_geomxform(const uint32_t *list, uint32_t count, uint16_t *fb,
                  const uint32_t *camblock)
{
    gpu_geomxform_kick(list, count, fb, camblock);
    return gpu_sync();
}

void gpu_textured_kick(const uint32_t *list, uint32_t count, void *fb,
                       const void *atlas, uint32_t atlas_width, const void *pal)
{
    G_CTRL = 0;
    *(volatile uint32_t *)(G_PARAMS + 0)  = (uint32_t)list;
    *(volatile uint32_t *)(G_PARAMS + 8)  = count;
    *(volatile uint32_t *)(G_PARAMS + 12) = (uint32_t)fb;
    *(volatile uint32_t *)(G_PARAMS + 16) = (uint32_t)atlas;
    *(volatile uint32_t *)(G_PARAMS + 20) = atlas_width;
    *(volatile uint32_t *)(G_PARAMS + 24) = (uint32_t)pal;
    mailbox[0] = 0;
    mailbox[1] = 0;
    G_PC = G_SRAM;
    G_CTRL = 1;
}

int gpu_textured(const uint32_t *list, uint32_t count, void *fb,
                 const void *atlas, uint32_t atlas_width, const void *pal)
{
    gpu_textured_kick(list, count, fb, atlas, atlas_width, pal);
    return gpu_sync();
}

/* vertex-cache: kernel pre-pass writes {sx,sy} per room vert here (512 max).
   STATICS bins push rooms 13/26 to 666/712 verts -> 768-vert cache under
   -DSTATICS. NOTE (2026-07-22 census): the SHIPPED bins already overflow the
   512 cap (room 8 = 529v, room 26 = 620v) — the pre-pass writes up to 864B
   past this array, straight over jerry.c's dsp_mailbox (nm-verified adjacent)
   and __bss_end. Latent bug, reported; default size kept for byte-identity. */
#ifdef STATICS
static uint32_t vtxcache[1536] __attribute__((aligned(16)));
#else
static uint32_t vtxcache[1024] __attribute__((aligned(16)));
#endif

/* PORTAL-WINDOW CLIP: clamp the kernel's spans/faces to a screen rect.
   Kernel reads $F03F50..5C each face/span; set before EVERY kick (a kick
   with a stale rect clips wrongly). Full screen = 0,319,0,239. */
void gpu_geotex_setclip(int x0, int x1, int y0, int y1)
{
    *(volatile uint32_t *)0xF03F50u = (uint32_t)x0;
    *(volatile uint32_t *)0xF03F54u = (uint32_t)x1;
    *(volatile uint32_t *)0xF03F58u = (uint32_t)y0;
    *(volatile uint32_t *)0xF03F5Cu = (uint32_t)y1;
}

#ifdef MMULTX
/* Precompose yaw*pitch into a flat 3x3 Q12 matrix (row-major) so the kernel's
 * per-vertex rotate becomes 3 hardware MMULTs instead of 8 software imul32.
 * Each coeff is duplicated into BOTH 16-bit halves: silicon MMULT reads the
 * SRAM matrix element from the LOW half, jsim reads the HIGH half — both-halves
 * keeps them identical. Cross-products round-to-nearest (+2048) hold the error
 * to ~1-2px (max 3 camera units) vs the imul32 rotate. Host-verified (300k
 * poses); see MMULT_IMPL.md. The kernel reads g_xform_mtx's pointer from the
 * $F03F20 param scrap under .if MMULTX. */
/* The matrix is APPENDED TO THE CAMERA BLOCK (longs 7..15) rather than published
 * via a scratch SRAM slot. An earlier cut parked the DRAM pointer at $F03F20 on
 * the strength of a stale map comment ("4B, ex-SHADEK") — but SHADEK STILL LIVES
 * THERE and SHADEPASS rewrites it ONCE PER FACE. Result: room 1 transformed
 * correctly, then its face loop clobbered the pointer and every later room/blob
 * (Lara included, she is drawn last) copied garbage as its matrix -> vertices
 * scattered across the screen, scenery popup, and a hang once a wild vertex
 * reached the blitter. The camera block needs no new slot: the 68k writes its
 * pointer to PARAMS[4] ($F03F10) every kick and the kernel never writes PARAMS.
 * We re-publish an extended copy, so main.c's camblk[] stays 8 longs. */
static uint32_t g_camext[16];   /* [0..6] camera as passed, [7..15] the 3x3 */
static uint32_t mtx_pack(int c){ return ((uint32_t)c << 16) | (uint32_t)(c & 0xFFFF); }
static const void *build_xform_mtx(const void *camblk)
{
    const int32_t *cb = (const int32_t *)camblk;   /* cY4,sY4,cP4,sP4 (Q12, signed) */
    const uint32_t *cu = (const uint32_t *)camblk;
    int cY = cb[0], sY = cb[1], cP = cb[2], sP = cb[3];
    int sYsP = (sY*sP + 2048) >> 12, cYsP = (cY*sP + 2048) >> 12;
    int sYcP = (sY*cP + 2048) >> 12, cYcP = (cY*cP + 2048) >> 12;
    int i;
    for (i = 0; i < 7; i++) g_camext[i] = cu[i];
    g_camext[ 7]=mtx_pack(cY);    g_camext[ 8]=mtx_pack(0);   g_camext[ 9]=mtx_pack(-sY);
    g_camext[10]=mtx_pack(-sYsP); g_camext[11]=mtx_pack(cP);  g_camext[12]=mtx_pack(-cYsP);
    g_camext[13]=mtx_pack(sYcP);  g_camext[14]=mtx_pack(sP);  g_camext[15]=mtx_pack(cYcP);
    return g_camext;
}
#endif

void gpu_geotex_kick(const void *room, void *fb, const void *camblk,
                     const void *atlas, uint32_t atlas_width)
{
    G_CTRL = 0;
#ifdef MMULTX
    camblk = build_xform_mtx(camblk);   /* publish camera + appended 3x3 */
#endif
    *(volatile uint32_t *)(G_PARAMS + 0)  = (uint32_t)room;
    *(volatile uint32_t *)(G_PARAMS + 8)  = (uint32_t)vtxcache;
    *(volatile uint32_t *)(G_PARAMS + 12) = (uint32_t)fb;
    *(volatile uint32_t *)(G_PARAMS + 16) = (uint32_t)camblk;
    *(volatile uint32_t *)(G_PARAMS + 20) = (uint32_t)atlas;
    *(volatile uint32_t *)(G_PARAMS + 24) = atlas_width;
    *(volatile uint32_t *)(G_PARAMS + 28) = 0;   /* legacy: no dispatch list */
    mailbox[0] = 0;
    mailbox[1] = 0;
    G_PC = G_SRAM;
    G_CTRL = 1;
}

/* SINGLE-DISPATCH: render a LIST of rooms (with per-room clip rects) in ONE
   kick — one 68k sync for the whole world instead of one per room. list =
   {count, then per room: room_ptr, (clipx0<<16)|clipx1, (clipy0<<16)|clipy1}.
   room/params[0] must still be a valid room (the FIRST one) for the
   self-test guard; the kernel re-reads per-room state from the list. */
void gpu_geotex_dispatch(const uint32_t *list, void *fb, const void *camblk,
                         const void *atlas, uint32_t atlas_width)
{
    /* the list is COPIED into GPU SRAM: the kernel's list loads happen right
       after taken branches, and Tom mis-reads DRAM shortly after branches
       (the documented gpu_fill blocker) — SRAM loads are immune. $F03F70..
       $F03FFF fits count + 11 rooms x 3 longs. */
    volatile uint32_t *sl = (volatile uint32_t *)0xF03F74u;
    uint32_t n = list[0], i;
#ifdef MMULTX
    camblk = build_xform_mtx(camblk);   /* publish camera + appended 3x3 */
#endif
    if (n > 3) n = 3;      /* list ends $F03FA4; tail = kernel vars */      /* SRAM cap SHRUNK 8->5 (2026-07-20): the list now
                              ends at $F03FC8, freeing $F03FC8-FF for kernel
                              scratch (rect-shade + task 6). Callers batch by
                              5; the current room stays last in each batch. */
    G_CTRL = 0;
    sl[0] = n;
    for (i = 0; i < n*4; i++) sl[1+i] = list[1+i];
    *(volatile uint32_t *)(G_PARAMS + 0)  = list[1];         /* first room */
    *(volatile uint32_t *)(G_PARAMS + 8)  = (uint32_t)vtxcache;
    *(volatile uint32_t *)(G_PARAMS + 12) = (uint32_t)fb;
    *(volatile uint32_t *)(G_PARAMS + 16) = (uint32_t)camblk;
    *(volatile uint32_t *)(G_PARAMS + 20) = (uint32_t)atlas;
    *(volatile uint32_t *)(G_PARAMS + 24) = atlas_width;
    *(volatile uint32_t *)(G_PARAMS + 28) = 0xF03F74u;       /* SRAM list */
    mailbox[0] = 0;
    mailbox[1] = 0;
    G_PC = G_SRAM;
    G_CTRL = 1;
}

int gpu_geotex(const void *room, void *fb, const void *camblk,
               const void *atlas, uint32_t atlas_width)
{
#if defined(BEACON_AT) && BEACON_AT == 6
    { extern void hang_beacon(uint16_t); hang_beacon(0xF83E); }   /* YELLOW: reached the Tom kick */
#endif
    gpu_geotex_kick(room, fb, camblk, atlas, atlas_width);
#if defined(BEACON_AT) && BEACON_AT == 12
    /* THE DISCRIMINATOR. Wait for Tom WITHOUT the STOP - a pure spin cannot be
       lost the way an interrupt wake can - then report what Tom actually did:
         GREEN = Tom FINISHED (mailbox is MAGIC_DONE). The render was fine and
                 the fault is the WAKE path: the 68k slept through a completed
                 job, i.e. the interrupt never came.
         RED   = Tom NEVER finished. The GPU is wedged and the STOP is an
                 innocent bystander.
       BLUE = we never even got here. */
    { extern void hang_beacon(uint16_t);
      uint32_t spins = 0;
      while (mailbox[0] != MAGIC_DONE && spins < 40000000u) spins++;
      hang_beacon(mailbox[0] == MAGIC_DONE ? 0x003E : 0xF800); }
#endif
    { int r_ = gpu_sync();
#if defined(BEACON_AT) && BEACON_AT == 9
      { extern void hang_beacon(uint16_t); hang_beacon(0x07FE); } /* CYAN: the first sync RETURNED */
#endif
      return r_; }
}

int gpu_geomdirect(const uint32_t *roomlist, uint32_t roomcount, uint16_t *fb,
                   const uint32_t *camblock, const uint32_t *lara,
                   uint32_t laracount)
{
    G_CTRL = 0;
    *(volatile uint32_t *)(G_PARAMS + 0)  = (uint32_t)roomlist;
    *(volatile uint32_t *)(G_PARAMS + 8)  = roomcount;
    *(volatile uint32_t *)(G_PARAMS + 12) = (uint32_t)fb;
    *(volatile uint32_t *)(G_PARAMS + 16) = (uint32_t)camblock;
    *(volatile uint32_t *)(G_PARAMS + 20) = (uint32_t)lara;
    *(volatile uint32_t *)(G_PARAMS + 24) = laracount;
    mailbox[0] = 0;
    mailbox[1] = 0;
    G_PC = G_SRAM;
    G_CTRL = 1;
    return gpu_sync();
}
