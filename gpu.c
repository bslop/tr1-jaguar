/*
 * gpu.c - Tom span-fill driver (68k side).
 *
 * Uploads gpu_spanfill.gas once; each frame the 68k hands Tom a DRAM
 * span list and Tom programs the Blitter for every span from SRAM
 * (far faster than the 68k doing per-span MMIO + idle spins). Launch
 * protocol and DRAM-mailbox handshake are the proven Jaguar idiom.
 */
#include "jaguar.h"
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

int gpu_sync(void)
{
    uint32_t i;
    /* STOP-sync: with SINGLE-DISPATCH there is ONE sync spanning the whole
     * room render — a tight DRAM poll here steals bus cycles from Tom for
     * the entire frame. STOP releases the bus completely; the geotex kernel
     * raises CPUINT at alldone, and vblank (60Hz) bounds the worst-case
     * wake. Supervisor mode throughout, so STOP is legal. */
    for (i = 0; i < 400; i++) {          /* ~6s worst case at vblank wakes */
        if (mailbox[0] == MAGIC_DONE) {
            G_CTRL = 0;
            return 1;
        }
        __asm__ volatile ("stop #0x2000");   /* sleep until any interrupt */
    }
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

/* vertex-cache: kernel pre-pass writes {sx,sy} per room vert here (512 max) */
static uint32_t vtxcache[1024] __attribute__((aligned(16)));

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

void gpu_geotex_kick(const void *room, void *fb, const void *camblk,
                     const void *atlas, uint32_t atlas_width)
{
    G_CTRL = 0;
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
    if (n > 8) n = 8;      /* SRAM cap: 4-long entries {room,clipx,clipy,
                              cacheptr}; caller keeps the current room last */
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
    gpu_geotex_kick(room, fb, camblk, atlas, atlas_width);
    return gpu_sync();
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
