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
#ifdef MULTIROOM
/* TITLE-HQ kernel (task #4): same kernel assembled with LOWRES=0 (240-line
 * projection constants).  gpu_kernel_select() swaps which blob is resident in
 * GPU SRAM; call ONLY with Tom idle (after gpu_sync / before the next kick).
 * The title phase runs HQ; entering the game re-selects the normal kernel. */
extern const uint8_t gpu_kernel_hq[], gpu_kernel_hq_end[];
static int g_kernel_cur = 0;             /* 0 = game kernel (boot default) */
void gpu_kernel_select(int hq)
{
    const uint32_t *src; uint32_t n, i; volatile uint32_t *dst;
    if (hq == g_kernel_cur) return;
    src = hq ? (const uint32_t *)gpu_kernel_hq : (const uint32_t *)gpu_kernel;
    n   = hq ? (uint32_t)(gpu_kernel_hq_end - gpu_kernel_hq) / 4
             : (uint32_t)(gpu_kernel_end - gpu_kernel) / 4;
    dst = (volatile uint32_t *)G_SRAM;
    for (i = 0; i < n; i++) dst[i] = src[i];
    g_kernel_cur = hq;
}

void gpu_kernel_dirty(void)
{
    g_kernel_cur = -1;
}
#endif

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

/* ---- BOOT-FMV JV02 decoder (gpu_jvdec.gas) ------------------------------
 * Loads its own kernel (swapping out whatever is resident - BOOTVID runs
 * before the title selects the renderer). Per frame: decode payload into
 * the 160x120 stage, horizontally double the union band into the fb.
 * Returns 1 + this frame's written band in *lo/*hi; 0 on timeout. */
void gpu_jvdec_load(void)
{
    extern const uint8_t gpu_jvdec_kernel[], gpu_jvdec_kernel_end[];
    const uint32_t *src = (const uint32_t *)gpu_jvdec_kernel;
    uint32_t n = (uint32_t)(gpu_jvdec_kernel_end - gpu_jvdec_kernel) / 4;
    volatile uint32_t *dst = (volatile uint32_t *)G_SRAM;
    uint32_t i;
    G_CTRL = 0;
    /* STOP-SETTLE (2026-08-07, the load-corruption root): G_CTRL=0 posts
       but Tom keeps executing for a while - SRAM writes issued before he
       actually halts get mangled. Loads issued long after a stop (game
       entry, behind two heavy functions) verified clean; loads issued
       immediately after activity (video entry, post-drain boot) failed
       every probe. Read back until the GPUGO bit drops, then let the
       pipeline drain before touching his RAM. */
    for (i = 0; i < 10000; i++)
        if ((G_CTRL & 1u) == 0) break;
    { volatile uint32_t d; for (d = 0; d < 4000; d++) ; }
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

/* Restore the boot-time param block after the clips: gpu_init wrote the
 * mailbox pointer (+4) and zeroed the geomdirect lara fields (+20/+24)
 * ONCE; the video params clobber all three, and a geotex kick after that
 * stores its DONE flag through garbage - the 68k times out on every kick
 * and the ring items draw desynced (the post-video 'menu doesn't work',
 * 2026-08-05). */
void gpu_jvdec_done(void)
{
    G_CTRL = 0;
    *(volatile uint32_t *)(G_PARAMS + 4)  = (uint32_t)mailbox;
    *(volatile uint32_t *)(G_PARAMS + 20) = 0;
    *(volatile uint32_t *)(G_PARAMS + 24) = 0;
}

/* ASYNC split (2026-08-06, user: "make the video player less jumpy"):
 * kick returns immediately so the 68k can run GD top-up reads inside the
 * frame's pace window instead of serialising read -> decode -> wait. The
 * caller must not touch the token/codebook buffers between kick and wait. */
void gpu_jvdec_kick(const void *prevTok, uint32_t prevLen,
                    const void *curTok, uint32_t curLen,
                    const void *cb, void *fb)
{
    G_CTRL = 0;
    /* DONE/HELLO handshake moved to the DRAM mailbox (2026-08-07): the 68k
       cannot reliably read GPU SRAM while the GPU is running, so polling
       PARAMS+32 timed out on EVERY frame on silicon and all video was
       silently 68k-fallback painted — the root of the delta-clip ghosting
       and the entire v8 pacing fight. Same pattern as every other kernel. */
    mailbox[0] = 0;
    mailbox[1] = 0;
    *(volatile uint32_t *)(G_PARAMS + 24) = (uint32_t)mailbox;
    *(volatile uint32_t *)(G_PARAMS + 0)  = (uint32_t)prevTok;
    *(volatile uint32_t *)(G_PARAMS + 4)  = prevLen;
    *(volatile uint32_t *)(G_PARAMS + 8)  = (uint32_t)curTok;
    *(volatile uint32_t *)(G_PARAMS + 12) = curLen;
    *(volatile uint32_t *)(G_PARAMS + 16) = (uint32_t)cb;
    *(volatile uint32_t *)(G_PARAMS + 20) = (uint32_t)fb;
    *(volatile uint32_t *)(G_PARAMS + 32) = 0;
    G_PC = G_SRAM;
    G_CTRL = 1;
}

int gpu_jvdec_wait(void)
{
    uint32_t i;
    /* sparse poll of the DRAM mailbox (NOT GPU SRAM - see gpu_jvdec_kick) */
    for (i = 0; i < 80000; i++) {
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

/* VIDDIAG: did the kernel start? (hello magic in the DRAM mailbox) */
uint32_t gpu_jvdec_hello(void)
{
    return mailbox[1];
}

/* VIDDIAG: does GPU SRAM actually hold the jvdec kernel? (call with the
   GPU stopped, right after gpu_jvdec_load). Returns mismatch count. */
uint32_t gpu_jvdec_verify(void)
{
    extern const uint8_t gpu_jvdec_kernel[], gpu_jvdec_kernel_end[];
    const uint32_t *src = (const uint32_t *)gpu_jvdec_kernel;
    volatile uint32_t *dst = (volatile uint32_t *)G_SRAM;
    uint32_t n = (uint32_t)(gpu_jvdec_kernel_end - gpu_jvdec_kernel) / 4;
    uint32_t i, bad = 0;
    for (i = 0; i < n; i++)
        if (dst[i] != src[i]) bad++;
    return bad;
}

/* VIDDIAG: where is Tom? (sample after wait; PC holds its last value) */
uint32_t gpu_pc_read(void)
{
    return *(volatile uint32_t *)0xF02110u;
}

/* VIDDIAG: GPU-SRAM read/write reliability probe. Writes a 32-long pattern
   to an unused SRAM corner (F03800), reads it back 64 times, returns the
   total mismatch count (0 = both directions clean HERE AND NOW). Run at
   several moments of one boot to map WHICH contexts poison the 68k<->GPU
   SRAM path and in which direction. GPU must be stopped. */
uint32_t gpu_sram_test(void)
{
    /* ☠️ F03800 was INSIDE the resident geotex kernel (3572B ends F03DF4)
       - the first version of this test corrupted the renderer and wedged
       boot on a pale screen (which incidentally PROVED the writes land).
       F03E00..F03E60 is the only truly free gap (kernel end to SRAM vars). */
    volatile uint32_t *p = (volatile uint32_t *)0xF03E00u;
    uint32_t i, r, bad = 0;
    G_CTRL = 0;
    for (i = 0; i < 16; i++)
        p[i] = 0xA5000000u | (i * 0x01010101u);
    for (r = 0; r < 64; r++)
        for (i = 0; i < 16; i++)
            if (p[i] != (0xA5000000u | (i * 0x01010101u))) bad++;
    return bad;
}

#ifdef MULTIROOM
/* VIDDIAG: invalidate the resident-kernel tracker so the next
   gpu_kernel_select RELOADS unconditionally - a probe that loaded jvdec
   left g_kernel_cur stale and kernel_select(0) no-op'd, sending the GAME
   into the micro-kernel (the reboot-loop of 2026-08-07). */
void gpu_kernel_dirty(void);
#endif

int gpu_jvdec_frame(const void *prevTok, uint32_t prevLen,
                    const void *curTok, uint32_t curLen,
                    const void *cb, void *fb)
{
    gpu_jvdec_kick(prevTok, prevLen, curTok, curLen, cb, fb);
    return gpu_jvdec_wait();
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
        /* A10-resistant WITHOUT busy-polling DRAM.
           The first version of this spun on mailbox[0] with a bound. That is
           measurably wrong: the mailbox lives in DRAM, so the spin steals bus
           from Tom for the whole frame - exactly what this function's own
           comment warns about. Measured over a matched window, the spin cost
           +276% 68k instructions, +276% GPU contention (10.6M -> 39.8M) and
           -5% Tom throughput.
           So: STOP (which releases the bus completely) is the wait, and the
           A10 protection is to make sure a WAKER EXISTS rather than to avoid
           sleeping. Re-arm the vertical interrupt once before the first sleep,
           and again periodically; vblank then bounds every STOP at one field. */
        /* ONLY re-arm once the wait is already abnormally long. Re-arming
           writes VMODE, and doing that on the fast path (every frame) disturbs
           the video timing generator - measured at HALF the frame throughput.
           Tom normally completes within a wake or two, so i>=8 never fires in
           the common case and this costs nothing. */
        /* MEASURED: gpu_sync takes ~8 STOP wakes per frame in normal operation
           (Tom needs ~8 fields to render a frame and the 68k sleeps through
           them). So any threshold near 8 fires EVERY frame, and re-arming
           writes VMODE, which disturbs the video timing generator - that cost
           HALF the rendered frames. The re-arm is for a DEAD ISR, which means
           hundreds of wakes, so put it far out of the normal range. */
        if (i >= 200 && (i & 63) == 63) { extern void video_rearm_irq(void);
                                          video_rearm_irq(); }
        if (cpu_stop_unless(&mailbox[0], MAGIC_DONE)) {
            G_CTRL = 0;
            return 1;
        }
#else
#if defined(BEACON_AT) && BEACON_AT == 13
        /* Sample the interrupt state right before the FIRST sleep. */
        if (i == 0) { extern void hang_beacon_irqstate(void); hang_beacon_irqstate(); }
#endif
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
#ifdef VCBIG
/* VCBIG (2026-07-31): the overflow described above is REAL and still shipping.
   768 verts (STATICS) is not enough: rooms alone reach 712, and under
   single-dispatch the SAME cache also takes Lara's 300, so the pre-pass runs
   off the end and scribbles over dsp_mailbox. Symptoms that fit: half of
   Lara's head never drawn, and world faces appearing/disappearing as the room
   mix changes. 4096 longs = 2048 verts, comfortably clear of any room+Lara
   combination. Costs ~10KB of .bss, which is free here. */
static uint32_t vtxcache[4096] __attribute__((aligned(16)));
#else
#ifdef STATICS
static uint32_t vtxcache[1536] __attribute__((aligned(16)));
#else
static uint32_t vtxcache[1024] __attribute__((aligned(16)));
#endif
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

/* ---- KSWAP: PRICE A KERNEL OVERLAY BEFORE BUILDING ONE ------------------
 * The sector renderer needs TWO kernels (flats + the polygon path for walls,
 * Lara and the 6.3% fallback) and GPU SRAM holds only one: gpu_geotex is
 * 3572 B of the 3680 usable. The plan is an overlay -- keep both in DRAM and
 * DMA the right one in per pass -- which costs N kernel copies per frame.
 *
 * This flag pays that cost WITHOUT writing the overlay: reload the SAME kernel
 * KSWAP times before each dispatch. Behaviour is identical (it copies the
 * bytes that are already there), so any fps delta is PURELY the swap cost.
 * KSWAP=2 is what a flats+polygon overlay would actually pay.
 *
 * ☠️ This is the gate TRAPEZOID never took: measure the quantity the
 * IMPLEMENTATION will actually pay, not an idealised one. If two swaps cost a
 * vsync rung, the overlay route is dead and the campaign must go big-bang.
 * Measure in ROOM 26, never at the spawn. */
#ifdef KSWAP
static void kernel_reload(void)
{
    const uint32_t *src = (const uint32_t *)gpu_kernel;
    uint32_t n = (uint32_t)(gpu_kernel_end - gpu_kernel) / 4;
    volatile uint32_t *dst = (volatile uint32_t *)G_SRAM;
    uint32_t i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}
#endif

void gpu_geotex_kick(const void *room, void *fb, const void *camblk,
                     const void *atlas, uint32_t atlas_width)
{
    G_CTRL = 0;
#ifdef KSWAP
    { int _k; for (_k = 0; _k < KSWAP; _k++) kernel_reload(); }
#endif
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
