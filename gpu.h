#ifndef JAG_GPU_H
#define JAG_GPU_H

#include <stdint.h>

/* Upload the span-fill kernel into Tom's SRAM. Returns 1 if the GPU
 * round-trips an empty batch, 0 if absent (caller keeps CPU fills). */
int gpu_init(void);

/* Drain a 68k-built span list on Tom (each record = 3 longs:
 * A1_PIXEL, B_SRCD, B_COUNT). Returns 1 if the GPU signalled done.
 * Also used for the geomwalk kernel (list = poly packets). */
int gpu_spanfill(const uint32_t *list, uint32_t count, uint16_t *fb);

/* geomxform kernel: Tom transforms+projects+culls+edge-walks world-space
 * poly packets; camblock = {cY4,sY4,cP4,sP4,camx,camy,camz,pad}. */
int gpu_geomxform(const uint32_t *list, uint32_t count, uint16_t *fb,
                  const uint32_t *camblock);

/* Async: fire the geomxform kernel and return immediately; gpu_sync
 * waits for it. Lets the 68k build the next frame while Tom draws. */
void gpu_geomxform_kick(const uint32_t *list, uint32_t count, uint16_t *fb,
                        const uint32_t *camblock);
int  gpu_sync(void);

/* geomdirect kernel: Tom reads the room geometry tables straight from
 * DRAM (68k passes only per-room base pointers, far-first ordered) and
 * renders them, then the Lara world-packet list, last. */
int gpu_geomdirect(const uint32_t *roomlist, uint32_t roomcount, uint16_t *fb,
                   const uint32_t *camblock, const uint32_t *lara,
                   uint32_t laracount);

/* textured kernel: affine texture-mapped convex-poly fill. list = polys of
 * {n, then n*(sx,sy,u,v)}; atlas = 8bpp index atlas, pal = 256*RGB16. */
int gpu_textured(const uint32_t *list, uint32_t count, void *fb,
                 const void *atlas, uint32_t atlas_width, const void *pal);
/* async: fire the textured/bltex kernel and return; gpu_sync waits. */
void gpu_textured_kick(const uint32_t *list, uint32_t count, void *fb,
                       const void *atlas, uint32_t atlas_width, const void *pal);

/* geotex kernel: Tom reads textured room geometry from DRAM, transforms/
 * projects/culls per face, and Blitter-textures the spans. 68k passes only
 * the room blob + camera block + atlas. room = room0_tex.bin. */
void gpu_geotex_setclip(int x0, int x1, int y0, int y1);
void gpu_geotex_dispatch(const uint32_t *list, void *fb, const void *camblk,
                         const void *atlas, uint32_t atlas_width);
void gpu_geotex_kick(const void *room, void *fb, const void *camblk,
                     const void *atlas, uint32_t atlas_width);
int  gpu_geotex(const void *room, void *fb, const void *camblk,
                const void *atlas, uint32_t atlas_width);

#endif
