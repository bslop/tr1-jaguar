/*
 * main.c - OpenLara Jaguar port, milestone M2 (real TR1 room)
 *
 * Renders one real Tomb Raider room extracted from OpenLara's level
 * data (see tools/tr2jag.cpp -> room.bin, embedded by roomdata.S).
 * You fly a free camera through it with the D-pad. OpenLara is used
 * only as a DATA source; the geometry pipeline is ours.
 *
 * 68000 work (still "slow but correct", to be moved onto Tom next):
 * transform+project every vertex, near-cull + painter-sort faces.
 * Tom Blitter fills every span - the 68k never touches a pixel.
 *
 * room.bin format (big-endian; the 68k is big-endian so we read it
 * in place): header{u16 magic,vcount,qcount,tcount; s16 ox,oy,oz},
 * then vcount RVert{s16 x,y,z; u16 shade}, qcount RQuad{u16 v0..v3,
 * color, flags}, tcount RTri{u16 v0..v2, color, flags}.
 */
#include "jaguar.h"
#include "video.h"
#include "blit.h"
#include "vidpanel.h"
#include "joypad.h"
#include "gd_input.h"
#include "gdbios.h"

/* PADMAIN=N (2026-08-06): intra-main.o layout roll. PADTEXT pads AFTER
   the object, so a size delta INSIDE main.c shifts the A10-critical code
   identically across every PADTEXT offset - 14 straight black boots
   proved a ~100B block could not be compensated from outside. This
   early dummy absorbs/extends the delta from BEFORE the critical code. */
#ifdef PADMAIN
#define PM_STR2(x) #x
#define PM_STR(x) PM_STR2(x)
__attribute__((used, noinline)) void pad_main_probe(void)
{
    __asm__ volatile(".space " PM_STR(PADMAIN));
}
#endif
#include "gpu.h"
#include "skunkdbg.h"
#include "sintab.h"
#ifdef GEOTEX
#include "room0_tex.h"      /* ROOM0_ATLAS_H + Lara swatch cell defines */
#endif
#ifdef MULTIROOM
#include "mrt.h"            /* MRT_ATLAS_H + Lara swatch cell defines */
#if defined(ENTITIES) && defined(DOORTEX)
#include "mrt_door.h"       /* door/lever atlas UV rects (MRT_DOORPATCH) */
#endif
#if defined(ENTITIES) && defined(ENEMIES)
#include "mrt_bat.h"        /* bat model: posed fly-cycle frames + faces */
#include "mrt_wolf.h"       /* wolf model: run-cycle frames + faces       */
#include "mrt_bear.h"       /* bear model: reared pose + faces            */
#endif
#include "mrt_lara.h"       /* Lara anim frame indices (run/stand/jump) */
#include "mrt_spawn.h"      /* Lara entity from LEVEL1.PSX (room/pos/yaw) */
#ifdef ROOMTOUR
#include "roomtour_tab.h"   /* per-room centre table for the auto room audit */
#endif
#include "gym_spawn.h"      /* Lara entity from GYM.PSX (room/pos/yaw) */
#endif
/* Lara swatch/atlas constants resolve per build (room0 vs shared multiroom) */
#ifdef MULTIROOM
#define LARA_CELL_ MRT_LARA_CELL
#define LARA_SWY_  MRT_LARA_SW_Y
#define LARA_ATH_  MRT_ATLAS_H
#elif defined(GEOTEX)
#define LARA_CELL_ ROOM0_LARA_CELL
#define LARA_SWY_  ROOM0_LARA_SW_Y
#define LARA_ATH_  ROOM0_ATLAS_H
#endif

typedef int32_t fix;
#define CENTER_X  (RENDER_W / 2)
#define CENTER_Y  (RENDER_H / 2)
#define FOCAL     (RENDER_W / 2)
/* In LOWRES only the vertical axis is 2x compressed (the OP scaler stretches
 * the 120-line fb back to 240), so the Y focal length is halved.  This affects
 * only the 68k SOFTWARE projection path (fill_convex); the GEOMDIRECT kernel
 * carries its own FOCAL_Y.  In non-LOWRES FOCAL_Y == FOCAL (no change). */
#ifdef LOWRES
#define FOCAL_Y   (FOCAL / 2)
#else
#define FOCAL_Y   FOCAL
#endif
#define NEAR      32

/* FB8: 8bpp indexed framebuffer (Blitter hardware texturing). CLEAR/BLINK
 * are palette indices; I reserve CLUT[254]=black, [255]=white for the blink. */
#ifdef FB8
typedef uint8_t fbpix;
#define CLEAR_IDX 0     /* was 254: the rect-shade pass ORs k into cleared
                           void pixels, and 254|k hit 255 = the white blink
                           reserve (white cave mouths). Palette base 0 is
                           now sorted darkest-first, so 0|k stays black. */
#define BLINK_ON  255
#define BLINK_OFF 254
#else
typedef uint16_t fbpix;
#define CLEAR_IDX 0x0004
#define BLINK_ON  0xFFFF
#define BLINK_OFF 0x0000
#endif

#define SIN(a)    SINTAB[(a) & 255]
#define COS(a)    SINTAB[((a) + 64) & 255]

typedef struct { uint16_t magic, vcount, qcount, tcount; int16_t ox, oy, oz; } RHdr;
typedef struct { int16_t x, y, z; uint16_t shade; } RVert;
typedef struct { uint16_t v[4]; uint16_t color, flags; } RQuad;
typedef struct { uint16_t v[3]; uint16_t color, flags; } RTri;
typedef struct { int16_t floorY; uint16_t walkable; } RSector;

/* Multi-room (rooms.bin): several portal-connected rooms in one common
 * coordinate space. World pos of a room vertex = (local + off). */
typedef struct { uint16_t magic, roomCount; int16_t laraX, laraY, laraZ; uint16_t laraAngle; } MRHdr;
typedef struct { uint16_t vcount, qcount, tcount, xSec, zSec; int16_t offX, offZ, yTopD, pad; } MRBlock;
typedef struct {
    const RVert *verts; const RQuad *quads; const RTri *tris; const RSector *sect;
    const int16_t *norms;   /* (nq+nt) world face normals, s16 x3, /64 scale */
    int nv, nq, nt, xSec, zSec, offX, offZ, yTopD;
} RoomDesc;
extern const uint8_t rooms_data[];
static RoomDesc g_rooms[64];
static int g_nrooms;

/* Global floor/walkable lookup across all rooms (common coords). */
static int global_walkable(int wx, int wz, int *floorCommonY)
{
    int r;
    for (r = 0; r < g_nrooms; r++) {
        RoomDesc *rm = &g_rooms[r];
        int lx = wx - rm->offX, lz = wz - rm->offZ;
        if (lx >= 0 && lx < rm->xSec * 1024 && lz >= 0 && lz < rm->zSec * 1024) {
            const RSector *s = &rm->sect[(lx >> 10) * rm->zSec + (lz >> 10)];
            if (s->walkable) {
                if (floorCommonY) *floorCommonY = s->floorY + rm->yTopD;
                return 1;
            }
            /* wall in THIS room, but rooms overlap at portals - a
             * neighbour may be walkable here, so keep checking. */
        }
    }
    return 0;                         /* no room walkable here -> blocked */
}

#ifdef GEOTEX
/* Room-0 floor lookup for the textured free-cam collision. room0_sect.bin:
 *   u16 xSectors, zSectors ; s32 info_x, info_z ; xS*zS*{s16 floorY, ceilY}
 * floorY = floor*256 (world Y, +down); 0x7FFF = solid wall. Returns 1 and sets
 * *floorY if the sector under (wx,wz) has a floor; 0 (blocked) for wall/OOB. */
extern const uint8_t room0_sect[];
static int room0_floor(int wx, int wz, int *floorY)
{
    const uint8_t *sp = room0_sect;
    int xS = (sp[0] << 8) | sp[1];
    int zS = (sp[2] << 8) | sp[3];
    int32_t infx = (int32_t)(((uint32_t)sp[4]<<24)|((uint32_t)sp[5]<<16)|((uint32_t)sp[6]<<8)|sp[7]);
    int32_t infz = (int32_t)(((uint32_t)sp[8]<<24)|((uint32_t)sp[9]<<16)|((uint32_t)sp[10]<<8)|sp[11]);
    int lx = wx - infx, lz = wz - infz;
    const uint8_t *e; int fy;
    if (lx < 0 || lx >= xS * 1024 || lz < 0 || lz >= zS * 1024) return 0;
    e = room0_sect + 12 + ((lx >> 10) * zS + (lz >> 10)) * 4;
    fy = (int16_t)(((uint16_t)e[0] << 8) | e[1]);
    if (fy == 0x7FFF) return 0;       /* wall */
    if (floorY) *floorY = fy;
    return 1;
}
#endif

#ifdef MULTIROOM
static int mr_iabs(int v) { return v < 0 ? -v : v; }
/* Multi-room floor lookup across a set of room0_sect blobs. Rooms overlap at
 * portals, so a wall in one room may be floor in a neighbour -> keep checking.
 * Returns 1 + *floorY for the first room with a real floor under (wx,wz). */
static int g_floorroom;   /* which room supplied the last room_floor_mr floor */
static int g_flr_slx, g_flr_slz;  /* TR FLOOR slantX/slantZ of that same cell —
                                     TR1 slides when |slant| > 2 on either axis */
static int g_floorwater;  /* 1 if that floor is a WATER-SURFACE cell. Read from
   bit0 of the STORED floorY (the extractor's mark) BEFORE the slope interpolation
   runs -- slope math dirties bit0, which made sloped SNOW read as water. The
   returned *floorY is always kept even, so callers must use THIS flag, never
   (floorY & 1), to detect water. */
static int room_reachable(int a, int b);   /* fwd (defined after S_adj) */
static int room_within3(int a, int b);     /* fwd: b within 3 portal hops of a */
static int g_flr_limit;   /* 1 = only search current room + portal neighbours
   (gameplay walking; stops stair-casing up stacked rooms' floors at region
   boundaries — the "launched to the top crossing onto the rug" bug).
   0 = AIRBORNE: was a full all-rooms scan; now limited to 3 portal hops
   (room_within3) — a fall can only cross portal-connected rooms, and the
   long-drop "fell through the bottom" case is 2 hops. The all-rooms scan's
   sector-table DRAM traffic doubled 68k logic time during descents on
   silicon (hl_logic 31->63ms, 2026-07-21) while jsim saw nothing — the
   bus-price class of cost. */
static int g_curroom_fwd(void);
/* ceiling of the CURRENT room's sector at (wx,wz): grab targets ABOVE this
   are through solid geometry (the courtyard->roof "fly to the top" chain). */
/* 16x16 hardware multiply (muls.w) — see pose-loop note below: both operands
 * must fit signed 16 bits. Hoisted above the collision helpers 2026-07-20:
 * the sector-index (lx>>10)*zS compiled to __mulsi3 (~246 cycles vs ~70) at
 * ~440 calls/render — the 68k pc-histogram's top game-code entry. */
static inline int32_t mul16(int32_t a, int32_t b){
    __asm__("muls.w %1,%0" : "+d"(a) : "d"(b));
    return a;
}

/* M68DIET AUTOPSY SUB-FLAGS (2026-07-22, see PERFHUNT_CAMPAIGN.md):
 * silicon convicted the combined M68DIET at -62% wall while every one of
 * its own 68k segments measured equal-or-faster (hl telemetry) — the
 * mechanism is indirect.  The three pieces now key off SEPARATE defines
 * so silicon can bisect them:
 *   M68D_A1 = portal_rect split-multiply    (PMUL / mul32x16)
 *   M68D_A2 = painter sort on admitted subset (order[] prefilter)
 *   M68D_A3 = lara_finish word-read centroids + reciprocal divides
 * -DM68DIET still means all three (the convicted combination, byte-
 * identical repro).  mul32x16 itself is needed by A1 and A3. */
#ifdef M68DIET
#ifndef M68D_A1
#define M68D_A1 1
#endif
#ifndef M68D_A2
#define M68D_A2 1
#endif
#ifndef M68D_A3
#define M68D_A3 1
#endif
#endif

#ifdef M68D_A2
static int g_m68d_nord;        /* rooms in order[] (A2 candidate subset) */
#define M68D_ORD_N g_m68d_nord
#else
#define M68D_ORD_N roomCount   /* legacy token — codegen identical */
#endif

#if defined(M68D_A1) || defined(M68D_A3)
/* PERFHUNT A1 (see PERFHUNT_CAMPAIGN.md): exact 32x16 multiply from two
 * hardware muls (muls.w high half + mulu.w low half + sign fix), for
 * |b| <= 32767 and |a*b| < 2^31.  ~110 cycles vs ~270+ for the gcc
 * __mulsi3 shift-add loop.  Used by portal_rect (the visibility rect
 * chain was 94% of all steady-state __mulsi3 calls) and lara_finish. */
static inline int32_t mul32x16(int32_t a, int32_t b){
    uint32_t lo = (uint16_t)a;
    int32_t  hi = (int32_t)(int16_t)((uint32_t)a >> 16);
    __asm__("mulu.w %1,%0" : "+d"(lo) : "d"(b));
    __asm__("muls.w %1,%0" : "+d"(hi) : "d"(b));
    if (b < 0) lo -= (uint32_t)(uint16_t)a << 16;
    return (int32_t)(((uint32_t)hi << 16) + lo);
}
#endif
#ifdef M68D_A1
#define PMUL(a,b) mul32x16((a),(b))
#else
#define PMUL(a,b) ((a)*(b))
#endif

static int room_ceil_at(const uint8_t *sp, int wx, int wz, int *ceilY)
{
    int xS = (sp[0]<<8)|sp[1], zS = (sp[2]<<8)|sp[3];
    int ix = (int)(((uint32_t)sp[4]<<24)|((uint32_t)sp[5]<<16)|((uint32_t)sp[6]<<8)|sp[7]);
    int iz = (int)(((uint32_t)sp[8]<<24)|((uint32_t)sp[9]<<16)|((uint32_t)sp[10]<<8)|sp[11]);
    int lx = wx - ix, lz = wz - iz;
    const uint8_t *e;
    if (lx < 0 || lx >= xS*1024 || lz < 0 || lz >= zS*1024) return 0;
    e = sp + 12 + (mul16(lx>>10, zS) + (lz>>10))*6;
    *ceilY = (int16_t)(((uint16_t)e[2]<<8)|e[3]);
    return 1;
}
/* ---- PORTAL-WINDOW CLIPPING: project a portal's 4 verts to a screen rect
   with EXACTLY the kernel's transform (yaw-rot -> pitch-rot -> NEAR cull ->
   FOCAL project). Neighbour rooms then render clipped to the doorway rect
   they're seen through; an empty/behind rect skips the room draw entirely.
   Returns 0 = portal fully behind (contributes nothing), 1 = rect valid,
   2 = crosses the near plane (caller must use FULL SCREEN, conservative). */
static fix pcl_cY4, pcl_sY4, pcl_cP4, pcl_sP4;   /* set once per frame */
static int pcl_camx, pcl_camy, pcl_camz;
#define PCL_FOCAL   190              /* MUST match the kernel's FOCAL */
#define PCL_CX      160
#define PCL_CY      (RENDER_H/2)
#define PCL_FOCALY  ((RENDER_H)==240 ? 190 : 95)
static int portal_rect(const long *pr, int *rx0, int *rx1, int *ry0, int *ry1)
{
    int i, behind = 0;
    int minx = 9999, maxx = -9999, miny = 9999, maxy = -9999;
    for (i = 0; i < 4; i++) {
        int32_t dx = (int32_t)pr[1+i*3]   - pcl_camx;
        int32_t dy = (int32_t)pr[1+i*3+1] - pcl_camy;
        int32_t dz = (int32_t)pr[1+i*3+2] - pcl_camz;
        int32_t rx, rz, ry, rz2, sx, sy;
        rx  = (PMUL(dx,pcl_cY4) - PMUL(dz,pcl_sY4)) >> 12;
        rz  = (PMUL(dx,pcl_sY4) + PMUL(dz,pcl_cY4)) >> 12;
        ry  = (PMUL(dy,pcl_cP4) - PMUL(rz,pcl_sP4)) >> 12;
        rz2 = (PMUL(dy,pcl_sP4) + PMUL(rz,pcl_cP4)) >> 12;
        if (rz2 < 32) {
            behind++;
            if (rz2 > -1024) behind |= 0x100;   /* CROSSING the plane, not
                                                   well behind: portal must
                                                   go conservative, not cull */
            continue;
        }
        sx = PCL_CX + rx*PCL_FOCAL/rz2;
        sy = PCL_CY + ry*PCL_FOCALY/rz2;
        if (sx < minx) minx = sx;  if (sx > maxx) maxx = sx;
        if (sy < miny) miny = sy;  if (sy > maxy) maxy = sy;
    }
    if ((behind & 0xFF) == 4)
        return (behind & 0x100) ? 2 : 0;    /* all behind: cull only when
                                               WELL behind (else we're
                                               walking through the doorway
                                               -> conservative full screen) */
    if (behind)      return 2;              /* crosses near plane */
    *rx0 = minx - 1; *rx1 = maxx + 1;       /* 1px rounding margin */
    *ry0 = miny - 1; *ry1 = maxy + 1;
    if (*rx0 < 0) *rx0 = 0;  if (*rx1 > 319) *rx1 = 319;
    if (*ry0 < 0) *ry0 = 0;  if (*ry1 > RENDER_H-1) *ry1 = RENDER_H-1;
    return 1;
}
/* union of the doorway rects from room a into room b (a's portal list).
   Same returns as portal_rect (0 none visible / 1 rect / 2 full screen). */
static const long (*S_portalv)[13];
static const unsigned short *S_portal_ofs;
static int room_link_rect(int a, int b, int *x0, int *x1, int *y0, int *y1)
{
    int p, got = 0;
    int ux0=0, ux1=0, uy0=0, uy1=0;
    for (p = S_portal_ofs[a]; p < S_portal_ofs[a+1]; p++) {
        int px0, px1, py0, py1, r;
        if ((int)S_portalv[p][0] != b) continue;
        r = portal_rect(S_portalv[p], &px0, &px1, &py0, &py1);
        if (r == 2) return 2;
        if (r == 0) continue;
        if (!got) { ux0=px0; ux1=px1; uy0=py0; uy1=py1; got=1; }
        else { if (px0<ux0) ux0=px0; if (px1>ux1) ux1=px1;
               if (py0<uy0) uy0=py0; if (py1>uy1) uy1=py1; }
    }
    if (!got) return 0;
    *x0=ux0; *x1=ux1; *y0=uy0; *y1=uy1;
    return 1;
}

/* TR wall rule: a WALL sector (0x7FFF) in the room Lara is IN blocks her
   outright — even when an overlapping room has floor beyond it. Real TR only
   crosses room boundaries through portal openings, never through wall
   columns (she was walking through the library wall into the void because an
   adjacent room's floor was reachable on the far side). Positions OUTSIDE
   the room's bounds are NOT walls: that's a portal edge, the neighbour-room
   floor search handles it. */
static int room_wall_at(const uint8_t *sp, int wx, int wz)
{
    int xS = (sp[0]<<8)|sp[1], zS = (sp[2]<<8)|sp[3];
    int ix = (int)(((uint32_t)sp[4]<<24)|((uint32_t)sp[5]<<16)|((uint32_t)sp[6]<<8)|sp[7]);
    int iz = (int)(((uint32_t)sp[8]<<24)|((uint32_t)sp[9]<<16)|((uint32_t)sp[10]<<8)|sp[11]);
    int lx = wx - ix, lz = wz - iz;
    const uint8_t *e;
    if (lx < 0 || lx >= xS*1024 || lz < 0 || lz >= zS*1024) return 0;
    e = sp + 12 + (mul16(lx>>10, zS) + (lz>>10))*6;
    return (int16_t)(((uint16_t)e[0]<<8)|e[1]) == 0x7FFF;
}
static int g_flr_grab;    /* 1 = ledge-search mode: plain closest-floor (the
                             vault/grab checks look for floors ABOVE Lara) */
static int g_flr_wy;      /* caller's current Y for Y-AWARE floor selection:
   with vertically STACKED rooms (10-room sets), "lowest floor wins" grabbed
   floors in rooms BELOW; instead prefer the nearest floor at/below wy that's
   reachable (within step-up), falling back to the old rule if none. */
#ifdef MV_SIDE
/* SIDESTEP, deliberately OUT OF LINE.  Inlined into main() this costs a local
   that stays live across the whole tank-control block, and the extra pressure
   re-allocates registers throughout a 6,000-instruction function - 522 changed
   instruction hunks against the control, and a build that dies on silicon
   before it ever draws a frame.  Its own frame keeps main()'s codegen alone. */
static void lara_sidestep(int side, const uint8_t **rsect, int roomCount)
    __attribute__((noinline));
#endif
static int room_floor_mr(const uint8_t **rsect, int n, int wx, int wz, int *floorY)
{
    int r, found = 0, best = 0, best_w = 0;
    int nfound = 0, nbest = 0, nbestd = 0, nbest_w = 0;  /* Y-aware: CLOSEST reachable floor */
    int nbtier = 0;
    g_floorwater = 0;
    for (r = 0; r < n; r++) {
        const uint8_t *sp = rsect[r];
        int xS, zS;
        if (g_flr_limit ? !room_reachable(g_curroom_fwd(), r)
                        : !room_within3(g_curroom_fwd(), r)) continue;
        xS = (sp[0]<<8)|sp[1]; zS = (sp[2]<<8)|sp[3];
        int ix = (int)(((uint32_t)sp[4]<<24)|((uint32_t)sp[5]<<16)|((uint32_t)sp[6]<<8)|sp[7]);
        int iz = (int)(((uint32_t)sp[8]<<24)|((uint32_t)sp[9]<<16)|((uint32_t)sp[10]<<8)|sp[11]);
        int lx = wx - ix, lz = wz - iz;
        const uint8_t *e; int fy, dx, dz, sxs, szs, w;
        if (lx < 0 || lx >= xS*1024 || lz < 0 || lz >= zS*1024) continue;
        e = sp + 12 + (mul16(lx>>10, zS) + (lz>>10))*6;   /* cell stride 6 (slant added) */
        fy = (int16_t)(((uint16_t)e[0]<<8)|e[1]);
        if (fy >= 0x7FFE) continue;   /* 7FFF wall / 7FFE floor OPENING: the
                                         room below supplies the floor */
        w = fy & 1; fy &= ~1;   /* bit0 = extractor's water-surface mark; take it
                                   NOW (stored floor is *256 = even) then keep the
                                   height even so the slope math below can't forge
                                   a false water flag on sloped (snowy) ground. */
        /* SLOPE: the floor tilts within the sector (TR FLOOR slantX/slantZ).
         * dx/dz = fractional position in the 1024-unit cell; replicate OpenLara
         * getFloorInfo so ramps read as a smooth surface (walk up slanted rock). */
        dx = lx & 1023; dz = lz & 1023;
        sxs = (int8_t)e[4]; szs = (int8_t)e[5];
        /* mul16: slant s8 x frac <=1023 both fit s16 — this multiply ran as
           __mulsi3 per CANDIDATE CELL of every floor search (~400/render,
           the pc-histogram's #1 game-code cost after the bars fix) */
        fy -= mul16(sxs, (sxs > 0 ? (dx - 1023) : dx)) >> 2;
        fy -= mul16(szs, (szs > 0 ? (dz - 1023) : dz)) >> 2;
        /* rooms overlap at portals: pick the LOWEST floor (largest Y, +Y down)
         * so Lara stands on the actual ground, not a phantom higher surface
         * from an adjoining room (that caused her to float near walls). */
        if (!found || fy > best) { best = fy; best_w = w; found = 1; g_floorroom = r;
                                   g_flr_slx = sxs; g_flr_slz = szs; }
        if (fy >= g_flr_wy - 384) {
            /* prefer the floor CLOSEST to the caller's feet: picking the
               HIGHEST reachable floor made phantom stacked floors ~300 up
               win over the real ground ("floating above the ground").
               TWO TIERS (ground mode): a floor AT/BELOW the feet always
               beats one ABOVE them — walking off a crate, the overlapping
               room-above's floor 256 up must not beat the marble 1024
               below ("floating above the marble"). Above-feet floors are
               only used when the column has nothing below (stair step-up).
               LEDGE mode (g_flr_grab, vault/grab searches): plain closest —
               those searches legitimately look for a floor above. */
            int d = fy - g_flr_wy, tier = 0;
            if (d < 0) { d = -d; tier = g_flr_grab ? 0 : 1; }
            if (!nfound || tier < nbtier ||
                (tier == nbtier && d < nbestd)) {
                nbest = fy; nbest_w = w; nbestd = d; nbtier = tier; nfound = 1; g_floorroom = r;
            }
        }
    }
    if (nfound) { if (floorY) *floorY = nbest & ~1; g_floorwater = nbest_w; return 1; }
    if (!found) return 0;
    if (floorY) *floorY = best & ~1;
    g_floorwater = best_w;
    return 1;
}

#endif

#define EYE_HEIGHT 640          /* camera height above the sector floor */
/* TR1's OWN velocities, read from the level's animation records (32-byte
 * anim struct, speed at +8 in 16.16 fixed).  Ground truth, not a guess and
 * not measured off a video:
 *     ANIM   0 RUN  speed = 47.000 units/frame
 *     ANIM   1 WALK speed = 15.000
 *     ANIM  38 BACK speed =  5.000 (accel -0.625)
 * The old value here was 140 - THREE TIMES the real run speed - and the walk
 * was WALK_SPEED/2 = 70 against a real 15.  With TIMESTEP supplying a correct
 * 30 Hz time base, those wrong constants are what made her feel squirrelly.
 * Dump them again any time with LARA_ANIMDUMP=0,1,38 on the extractor. */
#define WALK_SPEED 47           /* RUN: TR1 anim 0, units per 30 Hz frame */
#define RUN_SPEED_TR1  47
#define WALK_SPEED_TR1 15       /* PAD_C walk: TR1 anim 1 (0.32x run, not 0.5) */
#define BACK_SPEED_TR1  5       /* TR1 anim 38 BACK: speed 5, accel -0.625  */
/* ☠️ TR1 anim 88 FAST_BACK has speed **0** and accel **+4.6667/frame** - the
   backward hop ACCELERATES from rest and reaches ~47 by the end of its 10
   frames.  We drove it at BACK_SPEED_TR1 (the slow step-back speed) for both
   retreats, so the hop ANIMATION played while she barely moved.  Same lesson
   as the jump velocities: the anim record carries the real number.
   Counted in TICKS (like g_turnf) so it is frame-rate independent. */
#define FASTBACK_ACC_N  14      /* 4.6667 = 14/3 units per 30 Hz tick^2      */
#define FASTBACK_ACC_D   3
#define FASTBACK_MAX    47      /* the ramp's end speed over the 10 frames   */
/* TURN RATES, from OpenLara's lara.h (a faithful TR1 reimplementation) - they
 * are RADIANS PER SECOND, converted here into our units (256 per full circle)
 * at the 30 Hz logic tick:  units/tick = deg_per_sec / 360 * 256 / 30
 *     TURN_FAST   PI      = 180 deg/s -> 4.27   (running)
 *     TURN_NORMAL PI/2    =  90 deg/s -> 2.13   (walking)
 *     TURN_SLOW   PI/3    =  60 deg/s -> 1.42
 * We used a SINGLE constant of 3 for every state = 126.6 deg/s: too slow to
 * run, too fast to walk.  TR1 makes it state-dependent, which is most of why
 * turning felt wrong regardless of how TURNDIV was set. */
/* Overridable: TR1's rate is correct in WALL-CLOCK terms, but at a low frame
 * rate 4 units/tick x 4 ticks lands as a ~22 deg jump per DISPLAYED frame,
 * which reads faster than it is.  User asked for slightly slower. */
/* EIGHTHS of a SINTAB unit per 30 Hz tick (SINTAB is 256 per full turn).
   34 = TR1's authentic 4.27 u/tick = 180 deg/s;  24 = the old value, 126 deg/s.
   Tune with make TURNRUN=/TURNWALK=. */
#ifndef TURN_RUN_TR1
#define TURN_RUN_TR1    7       /* 37 deg/s (user 2026-08-07: '8 still a bit
                                   faster than the PSX, less touchy' - one
                                   notch down from 42) */
#endif
#ifndef TURN_WALK_TR1
#define TURN_WALK_TR1   5       /* 26 deg/s (was 6 = 32, same PSX feedback) */
#endif
#ifndef TURN_RAMP
#define TURN_RAMP       4       /* ticks to reach full turn rate (ease-in) */
#endif
#define SECTOR_SH  10           /* 1 sector = 1024 world units             */

extern const uint8_t room_data[];


/* Lara as a 3rd-person character (laramesh.bin, baked default pose;
 * same scale/axis as room verts: 1 unit = 1 world unit, +Y down). */
#define CAMDIST   1200          /* camera distance behind Lara (PS1-closer;
                                   was 1500 -- with the narrower FOV, 1200
                                   reproduces TR1's follow framing) */
#define CAMHEIGHT 700           /* camera height above the floor    */
#define CAM_PITCH 6             /* slight downward look (0..255)    */
#define LARA_FEET 86            /* model maxY -> origin/feet offset  */

typedef struct { uint16_t magic, vcount, qcount, tcount; } LHdr;
typedef struct { int16_t x, y, z; } LVert;
extern const uint8_t lara_data[];

static const LVert *g_lverts;     /* CURRENT run-cycle frame's verts */
static const RQuad *g_lquads;
static const RTri  *g_ltris;
static int g_lnv, g_lnq, g_lnt;
static int g_lnframes;            /* run-cycle frame count           */
static const uint8_t *g_lframe0;  /* frame-0 verts base              */
static const uint8_t *g_lshades;  /* per-frame per-face shade base   */
static int g_lanim;               /* current run-cycle frame index   */
#define LARA_ANIM_STEP 2          /* run-cycle frames advanced per moving tick */
/* Footfall frames as EIGHTHS of the cycle (see the phase-lock below). 2 and 6
   put them a half-cycle apart, off the cycle's start so the sound lands mid
   stride rather than on the loop seam. Tune by ear with LARA_FOOT1/2. */
#ifndef LARA_FOOT1
#define LARA_FOOT1 2
#endif
#ifndef LARA_FOOT2
#define LARA_FOOT2 6
#endif
static fix g_lax, g_laz, g_lafloor;   /* Lara world position       */
static fix g_lay, g_lavy;             /* Lara feet Y + vertical vel (jump) */
static int g_lajf;                    /* running-jump: carry forward while airborne */
static int g_jfwd;                    /* horizontal launch speed, latched at takeoff
                                         (TR1 velZ: 75 running, 50 standing/walking) */
static fix g_layprev;                 /* feet Y before this frame's gravity step,
                                         so the ledge grab can be a SWEPT test */
/* ---- state for the animations the audit found extracted but never wired
   (2026-08-02). LARA_MOVEAUDIT named 13 of them; these carry the extra state
   the selector needs to tell the cases apart. ---- */
static int g_jumped;                  /* airborne because she JUMPED (vs walked off
                                         a ledge) — picks JUMP anims vs FALL */
static int g_jdir;                    /* 0=fwd 1=back 2=left 3=right at launch   */
static int g_backf;                   /* ticks held BACK, for the FAST_BACK
                                         acceleration ramp (TR1 anim 88)     */
static int g_turnf;                   /* consecutive frames turning in place —
                                         TR1 escalates to FAST_TURN             */
static int g_sliding;                 /* 0=no, 1=facing downhill (SLIDE),
                                         2=facing uphill (SLIDE_BACK)           */
static int g_slideang;                /* downhill direction, SINTAB units        */
static int g_dead;                    /* death anim playing                      */
static int g_fally;                   /* feet Y where the current fall began —
                                         TR1 takes damage past ~3 blocks         */
static int g_hang;                    /* hanging from a ledge by the hands       */
static int g_airfr;                   /* consecutive airborne frames (land-thud gate) */
static int g_swim;                    /* 1 = swimming (water room)          */
static int g_watery;                  /* water surface Y for the active pool */
/* JUMP PHYSICS — TR1's OWN NUMBERS, read out of LEVEL1.PSX (2026-08-02).
 * Every animation carries an ANIM_CMD_JUMP (opcode 2) command holding its
 * launch (velY, velZ); `LARA_MOVEAUDIT=1` in the extractor prints them:
 *     anim 91 UP_JUMP        velY=-110 velZ= 2
 *     anim 76 FORWARD_JUMP   velY=-100 velZ=50   (standing)
 *     anim 16 FORWARD_JUMP   velY=-100 velZ=75   (running)
 *     anim 74 BACK_JUMP / 78 LEFT / 80 RIGHT     velY=-100 velZ=50
 * Gravity is 6 per 30 Hz TICK, damped to +1 once fallSpeed passes 128
 * (OpenLara controller.h applyGravity).  Check: up-jump apex = 110^2/(2*6)
 * = 1008 units = one block; running jump = 2*100/6 ticks * 75 = 2.4 blocks.
 * Both match the game.
 * ☠️ The old values (245 / 70 / 135) were hand-tuned "PS1-feel clamps" and,
 * worse, were integrated ONCE PER RENDERED FRAME while ground movement was
 * scaled by g_ticks -- so walking ran on a 30 Hz clock and jumping ran on a
 * 7.5 Hz one.  The arc could not be right at any frame rate. */
#define GRAVITY        6              /* units per 30 Hz TICK, per TR1      */
#define GRAVITY_TERM 128              /* above this fallSpeed, accel is +1  */
#define JUMP_VEL_UP  110              /* straight-up jump (anim 91)         */
#define JUMP_VEL_FWD 100              /* any directional jump               */
#define JUMP_FWD_STAND 50             /* standing forward jump (anim 76)    */
#define JUMP_FWD_RUN   75             /* running jump (anim 16/18)          */
/* Max step she can WALK up; anything taller needs a vault.
   TR1 is ONE CLICK = 256: OpenLara's checkClimb treats `h >= 256` as needing a
   climb animation, so under 256 is a free walk-up and 256+ must be vaulted.
   ☠️ This was 384 (1.5 clicks) — 50% too generous, so she auto-stepped ledges
   TR1 makes you vault. Over rocky terrain with many small ledges that compounds
   into walking up walls (user report + screencast 2026-08-02 19-26-49). */
#define LARA_STEPUP 256
/* ledge vault / pull-up: a step too tall to walk up but low enough to climb
   triggers a pull-up; while airborne, hands within reach of a ledge grab it. */
static int g_vault;                   /* pull-up in progress (0/1)        */
static int g_autoj;                   /* AUTO JUMP-REACH armed (UP at a wall
                                         with a grabbable ledge above)     */
static int g_fwdblk;                  /* forward held but BLOCKED this frame
                                         (gates the auto-reach probe)      */
#ifdef VIDDIAG
static uint32_t g_jv_vfy;             /* SRAM-verify mismatches after load */
static int g_jvmin_game;              /* context probe: 1=ran at game entry,
                                         2=failed there too, 0=not run yet */
static int g_jvmin2;                  /* ...after a verify-retry load       */
static int g_jv_d240;                 /* load-under-disp240: 1 ok, 2 corrupt */
#endif
#ifdef VIDPANEL
/* the same counters vidrom reports, so the SAME host decoder reads both */
/* ☠️ BSS IS FULL - even SIX longs here overflows the 16KB stack-headroom
   check. The diagnostic counters live in the CRASH SCRATCH instead: startup.S
   reserves $800-$8FF for exception dumps ($820..$844 used), so $880 up is
   free whenever the game is not already dead. Costs no BSS at all. */
#define VPC ((volatile uint32_t *)0x880u)
#define vp_frames  VPC[0]
#define vp_fields  VPC[1]
#define vp_read    VPC[2]
#define vp_tom     VPC[3]
#define vp_copy    VPC[4]
#define vp_tomfail VPC[5]
/* the untimed remainder is where the game's chug hid, so EVERY phase of the
   frame is bracketed now - read/tom/copy/pace/audio/paint. Anything still
   left over is parse + flip + loop overhead. $880..$8A3 (scratch runs to
   $8FF; exc_catch owns $820..$844). */
#define vp_pace    VPC[6]
#define vp_audio   VPC[7]
#define vp_pnt     VPC[8]   /* the instrument's own cost */
/* why the clip loop ended: 1 = played every frame, 2 = corrupt record header,
   3 = gd_fread failed, 4 = stream exhausted early, 5 = pad skip.  Colour
   timing off a capture cannot tell "played fast" from "ended early", and
   guessing between them wasted a roll. */
#define vp_exit    VPC[9]
#define VP_EXIT(c) do { vp_exit = (uint32_t)(c); } while (0)
#else
#define VP_EXIT(c) do { } while (0)
#endif

#ifdef BOOTVID
/* THE 68000 DOES NOT CARRY BYTES (user, 2026-08-08: "the 68000 is the enemy -
 * boot code runs on it, everything else should use Tom/Jerry exclusively").
 *
 * Compacting the video stream buffer is a bulk move, so the Blitter does it.
 * The move must start from an 8-aligned source for phrase mode, so the phase
 * (pos & 7) is carried across rather than squeezed out - records stay
 * 4-aligned relative to vb either way, which is what the audio long-reads and
 * the Tom kick need.  The Blitter moves whole 320-byte rows; the <320 tail is
 * moved as LONGS.  If the Blitter declines (misaligned) or never idles, the
 * long path covers the whole move - slower, but never wrong and never the old
 * byte-at-a-time loop that cost 478ms a frame.
 *
 * Returns the new `have`; *pos becomes the carried phase (0 or 4).
 */
static int stream_compact(uint8_t *vb, int have, int *pos)
{
    int p = *pos, keep = p & 7;
    const uint8_t *s = vb + (p - keep);
    int mv = have - p + keep;
    int moved = (int)blit_bytes(s, vb, (unsigned)mv);
    { int nl = (mv - moved) >> 2, i, k;
      uint32_t *d4 = (uint32_t *)(vb + moved);
      const uint32_t *s4 = (const uint32_t *)(s + moved);
      for (i = 0; i < nl; i++) d4[i] = s4[i];
      for (k = moved + (nl << 2); k < mv; k++) vb[k] = s[k]; }
    *pos = keep;
    return mv;
}
#endif
static int g_jv_ok;                   /* jvdec kernel VERIFIED in GPU SRAM:
                                         gate the Tom kick on it, or a bad
                                         load silently costs every frame the
                                         68k fallback walk */
static int g_jv_early;                /* jvdec kernel loaded at boot (skip
                                         the video-block load entirely)    */
#ifdef VIDDIAG
static uint32_t g_sr[4] = {99,99,99,99}; /* SRAM r/w test per context:
                                        0=post-init 1=video 2=startgame 3=game */
#endif
#ifdef VIDDIAG
#endif
static int g_autograb;                /* airborne: treat ACTION as held so
                                         the auto jump catches and pulls up */
static fix g_vaultx, g_vaultz;        /* foothold on top of the ledge     */
static int g_vaulty;                  /* target feet Y once up            */
static int g_climbf;                  /* climb tick counter               */
static int g_climby0;                 /* feet Y at the moment she grabs    */
static int g_climbx0, g_climbz0;      /* x/z at grab: eased onto the ledge  */
static int g_climbanim = LANIM_STOP;  /* which climb/vault anim to play    */
static int g_pickupt;                 /* pickup animation countdown (ticks) */
#define PICKUP_TICKS 8                 /* ticks the pickup pose plays        */
/* ---- ROLL + SIDESTEP (2026-07-28) ------------------------------------
   TR1 Control Method 1 puts roll on Circle and step-left/right on L2/R2.
   The Jaguar 3-button pad has neither, so:
     Y (Pro 6-button pad) or PAUSE = roll
     C + LEFT/RIGHT                = sidestep   (C alone still = walk)
   C+turn (slow turn) is traded for sidestep — it does the same
   ledge-positioning job, and the original binds the two separately.
   Anim ids are TR1's own (OpenLara lara.h): ANIM_STAND_ROLL_BEGIN/END and
   ANIM_STAND_LEFT/RIGHT; all 160 anims are already baked into mrt_lara. */
/* Roll has no entry in mrt_lara.h, so add it (TR1 ANIM_STAND_ROLL_BEGIN/END).
   SIDESTEP DOES: mrt_lara.h already defines LANIM_STEPL=65 / LANIM_STEPR=67.
   An earlier draft redefined them to OpenLara's ANIM_STAND_LEFT/RIGHT (2/3),
   which both fought the generated header and played the wrong animation. */
/* TR1 anim 63 SWITCH_DOWN, frames 961..1005 (45 frames, rate 3): Lara's
   two-handed pull on a wall lever.  Anim 64 is SWITCH_UP (the reverse). */
#define LANIM_SWITCH     63
#define SWITCH_ANIM_TICKS 45
static int g_swact;                   /* ticks left in the switch pull      */
#define LANIM_ROLL      146
#define LANIM_ROLLEND   147
/* NB: SINTAB is 256 ENTRIES PER TURN (SIN(a)=SINTAB[a&255]), so a quarter
   turn is 64 and 180 deg is 128 — NOT 1024-based like the title ring. */
#define ROLL_TICKS        8    /* 8 x (128/8=16) = exactly 128 = 180 deg */
/* MV_ROLL / MV_SIDE gate the two features SEPARATELY so the silicon bisect
   is a build flag, not an edit (2026-07-29: the combined build black-screens
   the console from t=0 while rendering identically to the control in jagemu). */
#ifdef MV_ROLL
static int g_rollt;                   /* >0 = mid-roll, controls locked     */
#endif
#ifdef PADTEXT
/* Dead weight in .TEXT, deliberately.  PADBYTES (below) grows .rodata, which
   sits AFTER .text and therefore moves nothing the code is linked against —
   it only proves the image SIZE is harmless.  This one grows .text by the
   same 1016 bytes the moveset does, so .rodata, .data and .bss all land at
   exactly the addresses the failing build puts them at, with not one new
   instruction executed.  Boots => the fault is the moveset code running.
   Black => the fault is positional and the moveset code is innocent. */
__attribute__((used, section(".text")))
static const uint8_t g_padtext[PADTEXT] = { 1 };
#endif
#ifdef PADBYTES
/* Dead weight, deliberately: PADBYTES bytes of .rodata nothing ever reads.
   It moves every later address and grows the image without adding a single
   executed instruction, which is exactly what separates "the build got
   bigger" from "the new code runs" as a cause of a silicon black screen. */
static const uint8_t g_padding[PADBYTES] __attribute__((used)) = { 1 };
#endif
static int g_curroom;                 /* room Lara is standing in (visibility) */
static int g_curroom_fwd(void) { return g_curroom; }
#define LARA_JUMPGRAB 1664            /* AUTO JUMP-REACH ceiling: up-jump apex
                                         (110^2/2g ~= 1008) + hand reach 720,
                                         less a catch margin — ledges to ~6.5
                                         clicks are worth jumping for */
#define LARA_CLIMB    768             /* max standing VAULT = 3 clicks (TR1-authentic;
                                         taller ledges need a jump+grab at the right
                                         height — 7-click vaults let her scale the
                                         mansion from the courtyard) */
#define LARA_GRABREACH 720            /* hands above feet; TR1 LARA_HANG_OFFSET=724 */
#define SLIDE_SPEED     70            /* units per 30 Hz tick down a slide     */
#define FASTTURN_TICKS  15            /* half a second of held turn -> FAST_TURN */
#define LARA_GRAB_TOL   64            /* TR1 checkHang: |ledge - hands| < 64.
                                         Applied to the SWEPT hand interval, not
                                         to a single frame's position. */
#define CLIMB_TICKS    6              /* game ticks the pull-up spans (fps  */
                                      /* is low, so play the 26f anim fast) */
/* alignToWall: TR squares Lara up to the wall before a climb (nearest 90 deg).
   The sector grid is axis-aligned, so snap her heading to the nearest cardinal. */
#define ALIGN_WALL(yaw) ((uint8_t)(((yaw) + 0x20) & 0xC0))
static uint8_t g_layaw;               /* Lara heading (0..255)     */
#ifdef TIMESTEP
/* FIXED TIMESTEP (2026-08-02): WALK_SPEED is "forward units per FRAME" and
 * lanim_step advances one animation frame per RENDERED frame, so Lara's speed
 * was proportional to the frame rate - 25% of intended at 7.50 fps.
 * g_ticks = 30 Hz logic ticks this rendered frame stands for, from the vblank
 * field counter.  Movement, turn and animation all scale by it, together.
 * CLAMPED TO 4: one tick is 140 units, so 4 is 560 - inside a 1024 sector, so
 * she cannot skip a sector boundary and tunnel.  Do NOT raise the clamp.
 * TURNDIV divides ONLY the turn scaling: at a low frame rate a fully-scaled
 * turn is a ~22 deg jump per displayed frame, which reads as twitchy even
 * though it is correct in wall-clock terms. */
#ifndef TURNDIV
#define TURNDIV 1
#endif
/* SPEEDDIV divides the LOGIC RATE itself, so movement AND animation slow
 * together and her feet stay planted.  Halving movement alone would make her
 * skate - legs at full cadence, body at half.  SPEEDDIV=2 => one logic tick
 * per 4 fields = 15 Hz. */
#ifndef SPEEDDIV
#define SPEEDDIV 1
#endif
/* ANIMDIV divides ONLY the animation rate, leaving translation alone - the
 * user asked for her MODEL speed halved, not her movement.  Note this
 * deliberately desyncs cadence from travel, so her feet will slide; that is
 * an aesthetic call at this frame rate, not a bug. */
#ifndef ANIMDIV
#define ANIMDIV 1
#endif
static int g_ticks = 1;
#ifdef STEADYREAD
static volatile int g_sr_on = SRGUARD;
#endif
static int g_tturn = 1;
static int g_tanim = 1;
#endif
static int lsx[512], lsy[512], lz[512];
static uint8_t lbehind[512];

#if defined(GEOTEX) || defined(MULTIROOM)
/* Build Lara's per-frame WORLD geometry in room0_tex format so the SAME
 * gpu_geotex kernel draws her over the textured room (no separate kernel).
 * Verts use the offX/offZ split (world = local + off<<8) so they fit s16;
 * every face's UVs point at one solid Lara swatch cell (flat colour). */
/* STAGEDIET: the geotex kernel expects a 12-byte plane prefix on EVERY face
   record (room bins carry real planes from the extractor's FACE_PLANES=1).
   Runtime-built blobs (Lara/props/title) are posed per frame, so baked
   normals don't exist — they emit a DUMMY plane {N=0, d=INT32_MIN}, which
   the kernel's early cull can never take (0 >= -2^27; not INT32_MIN —
   that overflows the kernel's 32-bit compare). */
#ifdef ABLADDER
/* content-ladder troubleshooting (2026-07-20, user-directed): find where
   the frame time lives by starting from Lara alone and admitting the
   world back one room at a time, live from the pad. */
#ifndef ABBOOT
#define ABBOOT 0
#endif
static int g_abrooms = ABBOOT;   /* boots Lara-only (0) unless -DABBOOT=N */
#endif
#ifdef FARDIAL
static int g_fardist = 9000;   /* per-vert kernel far cull; OPTION+UP/DOWN */
#endif
/* LOADING PROGRESS BAR (2026-07-25, user request).  The loading panel is
   painted and FLIPPED, so its buffer is the one the OP is displaying while the
   68k grinds through the 10-15 s level load.  Nothing else is drawing yet — the
   render loop has not started — so the bar is painted DIRECTLY into that same
   buffer.  No second buffer, no flips, no race, and it appears immediately.
   load_prog(num,den) is safe to call before the panel exists (g_loadfb is 0). */
/* Streaming buffer for the PS1 loading art, shared by the pre-title screen
   and the in-game loading panel.  24 rows a chunk = ten whole 512-byte
   sectors; it was 48 rows, and halving it is what let DBGROOM fit again. */
static uint8_t g_artbuf[7680] __attribute__((aligned(8)));   /* 8: blit_bytes phrase mode */

/* show_title_load: the PSX's pre-title loading screen.
 *
 * The disc shows the TOMB RAIDER splash with a red progress bar filling along
 * the bottom, between the attract videos and the title menu (user reference,
 * 2026-08-09).  We already ship that exact art - title.bin / title_pal.bin,
 * down to the "TM & (C) Core Design Ltd 1996" line - and the title screen
 * paints it with blit_copy straight out of ROM, so this is the same paint one
 * step earlier, plus the bar.  Geometry was measured off the reference frames:
 * the bar runs x 56..250, y 206..216 in 320x240 space, and title_pal index 80
 * is rgb(160,8,8) against the disc's rgb(157,16,1).
 *
 * The art is 320x240 and the boot display is still plain-240 here (g_disp240
 * is 1 until the game takes over), so it copies 1:1 - no decimation.
 */
/* Jaguar RGB16 is R<<11 | B<<6 | G<<1 (green and blue swapped vs 565), so
   the disc's bar red rgb(160,8,8) is 20<<11 | 1<<6 | 1<<1. */
#define LOADBAR_IDX 253
#define LOADBAR_RGB 0xA042u
#define TL_X0 56
#define TL_X1 250
#define TL_Y0 206
#define TL_Y1 217
#define TL_IDX 80
static void show_title_load(uint32_t fields)
{
    extern const uint8_t title_img[];
    extern const uint16_t title_pal[];
    extern volatile uint32_t frame_count;
    uint8_t *fb = (uint8_t *)video_backbuffer();
    uint32_t t0;
    int done = TL_X0;
    (void)0;

    blit_copy(title_img, fb, 240);        /* Blitter, straight out of ROM -
                                             the title screen's own paint */
    video_set_clut(title_pal);
    video_flip();
    t0 = frame_count;
    for (;;) {
        uint32_t el = frame_count - t0;
        int w, y3, x3;
        if (el > fields) el = fields;
        w = TL_X0 + (int)(((uint32_t)(TL_X1 - TL_X0) * el) / fields);
        /* fill only the newly-uncovered columns - a few dozen bytes a step,
           into the buffer already on screen (no flip runs during the fill,
           exactly as the in-game load_prog bar works) */
        if (w > done)
            blit_fill_rect(fb, done, TL_Y0, w - done, TL_Y1 - TL_Y0, TL_IDX);
        done = w;
        if (el >= fields)
            break;
    }
}

static uint8_t *g_loadfb;
static int g_loadlh, g_loadbx0, g_loadbx1, g_loadby, g_loadbh;
static void load_prog(int num, int den)
{
    int inner0, inner1, w, x, y;
    if (!g_loadfb || den <= 0) return;
    if (num < 0) num = 0;
    if (num > den) num = den;
    inner0 = g_loadbx0 + 1;
    inner1 = g_loadbx1 - 1;
    w = ((inner1 - inner0) * num) / den;
    if (w > 0)
        blit_fill_rect(g_loadfb, inner0, g_loadby, w, g_loadbh, LOADBAR_IDX);
}

#ifdef HOPDIAL
/* live draw-distance dial (user experiment 2026-07-21): OPTION+RIGHT/LEFT
   raises/lowers the portal-hop dispatch cap. 4 = uncapped;
   1 = current room + neighbours only (the user's room±1 proposal —
   EYE-CLEARED no black doorways + dips 5.7->8.5-11.6 + ceiling
   21.5->23.8, silicon 2026-07-21; HOPBOOT sets the boot value). */
#ifndef HOPBOOT
#define HOPBOOT 4
#endif
static int g_hopcap = HOPBOOT;
static int g_hop_cached_a = -1;
static uint8_t g_hop_inset[64];
#ifdef GOVERNOR
/* FRAME GOVERNOR (2026-07-22, PERFHUNT_CAMPAIGN.md smoothness campaign):
   VARIANCE tool, opt-in.  Measures each frame's VBL span at the loop top
   (frame_count delta — the PROFILE pfA machinery replicated for free);
   one frame longer than GOV_HI fields clamps g_hopcap to 1 for the
   following frames; GOV_K consecutive frames at/below GOV_LO restore the
   dial's cap.  Hysteresis: GOV_LO < GOV_HI plus the K-count — after a
   restore the worst oscillation is ONE over-budget probe frame per K calm
   frames (bounded ~1/K duty).  In persistently heavy views calm never
   accumulates and the clamp simply stays on (the intent).  Inactive by
   construction in steady light views (span never crosses GOV_HI), so
   steady-view rendering is untouched. */
#ifndef GOV_HI
#define GOV_HI 6     /* fields: a frame LONGER than this trips the clamp */
#endif
#ifndef GOV_LO
#define GOV_LO 4     /* fields: frames at/below this count as calm */
#endif
#ifndef GOV_K
#define GOV_K 20     /* consecutive calm frames before the cap restores */
#endif
static int g_gov_user = HOPBOOT;   /* the dial-chosen cap (restore target) */
static int g_gov_on;               /* clamp currently engaged */
static int g_gov_calm;             /* consecutive calm frames while clamped */
static uint32_t g_gov_trips;       /* telemetry: engagements */
#endif
#endif
#ifdef PIPELINE
/* frame N's LAST dispatch batch is still on Tom while frame N+1's 68k
   logic runs; collected (sync+flip) at the loop-top collect point.
   TWO flags: g_tominflight = kernel unsynced (a mid-frame consumer may
   clear it, e.g. the !lara_disp fallback kick); g_pipeframe = a frame
   awaits its deferred flip (ALWAYS flipped at the collect — conflating
   the two ate the flip whenever the fallback fired: boot hang). */
static int g_tominflight;
static int g_pipeframe;
#endif
/* Pulled in EARLY (it is include-guarded) purely for MRT_LPLANE_[QT]COUNT, so
   lara_blob[] is sized from the ACTUAL face counts. It used to be the literal
   16896, commented "12288 + 375 faces * 12B plane prefix" -- i.e. correct for
   exactly 375 faces and silently too small for 376. Adding 12 HEADFILL tris
   overflowed the static buffer and hung the board with a black screen
   (2026-07-31). Never hand-maintain this number again. */
#include "mrt_lplanes.h"
#ifdef STAGEDIET
/* d = -(1<<27), NOT INT32_MIN: the kernel's cmp computes N.C - d and
   INT32_MIN overflows it (sign flips -> face wrongly culled; cost Lara). */
#define QREC 36
#define TREC 30
#define LPLANE_PREFIX 12     /* bytes of plane prefix per face */
#define EMIT_PLANE(w) do { (w)[0]=0;(w)[1]=0;(w)[2]=0;(w)[3]=0; \
                           (w)[4]=0xF800;(w)[5]=0; (w)+=6; } while (0)
#else
#define QREC 24
#define TREC 18
#define LPLANE_PREFIX 0
#define EMIT_PLANE(w) do { } while (0)
#endif
#define LARA_BLOB_SZ ((MRT_LPLANE_QCOUNT * (QREC + LPLANE_PREFIX) +            \
                       MRT_LPLANE_TCOUNT * (TREC + LPLANE_PREFIX) + 256 + 7) & ~7)

static void build_lara_blob(uint8_t *buf, int atlasW)
{
    fix laC = COS(g_layaw), laS = SIN(g_layaw);
    int offX = g_lax >> 8, offZ = g_laz >> 8;      /* room base, fits s16   */
    int rx0  = g_lax & 255, rz0 = g_laz & 255;     /* sub-256 kept per-vert */
    int base_y = g_lafloor + LARA_FEET;
    uint16_t *h = (uint16_t *)buf;
    uint16_t *w;
    int i;
    /* per-face shades for the current run-cycle frame; vL/vH row is constant */
    const uint8_t *sh = g_lshades + g_lanim * (g_lnq + g_lnt);
    int vL = LARA_SWY_ + 1, vH = LARA_SWY_ + LARA_CELL_ - 2;
    /* header: vcount,qcount,tcount,atlasW,atlasH ; offX,offY,offZ */
    h[0]=(uint16_t)g_lnv; h[1]=(uint16_t)g_lnq; h[2]=(uint16_t)g_lnt;
    h[3]=(uint16_t)atlasW; h[4]=(uint16_t)LARA_ATH_;
    h[5]=(uint16_t)offX;   h[6]=0; h[7]=(uint16_t)offZ;
    /* verts (stride 8: s16 x,y,z ; u16 shade) */
    w = (uint16_t *)(buf + 16);
    for (i = 0; i < g_lnv; i++) {
        int mx = g_lverts[i].x, my = g_lverts[i].y, mz = g_lverts[i].z;
        int rx = ((mx * laC + mz * laS) >> 16) + rx0;
        int rz = ((-mx * laS + mz * laC) >> 16) + rz0;
        w[0]=(uint16_t)(int16_t)rx;
        w[1]=(uint16_t)(int16_t)(base_y + my);
        w[2]=(uint16_t)(int16_t)rz;
        w[3]=255;                                   /* full-bright         */
        w += 4;
    }
    /* quads (stride 24: u16 v0..v3 ; u0,v0,u1,v1,u2,v2,u3,v3) - per-face shade */
    for (i = 0; i < g_lnq; i++) {
        const uint16_t *qv = g_lquads[i].v;
        int s = sh[i];
        int uL = s*LARA_CELL_ + 1, uH = s*LARA_CELL_ + LARA_CELL_ - 2;
        EMIT_PLANE(w);
        w[0]=qv[0]; w[1]=qv[1]; w[2]=qv[2]; w[3]=qv[3];
        w[4]=(uint16_t)uL; w[5]=(uint16_t)vL;
        w[6]=(uint16_t)uH; w[7]=(uint16_t)vL;
        w[8]=(uint16_t)uH; w[9]=(uint16_t)vH;
        w[10]=(uint16_t)uL; w[11]=(uint16_t)vH;
        w += 12;
    }
    /* tris (stride 18: u16 v0,v1,v2 ; u0,v0,u1,v1,u2,v2) - per-face shade */
    for (i = 0; i < g_lnt; i++) {
        const uint16_t *tv = g_ltris[i].v;
        int s = sh[g_lnq + i];
        int uL = s*LARA_CELL_ + 1, uH = s*LARA_CELL_ + LARA_CELL_ - 2;
        EMIT_PLANE(w);
        w[0]=tv[0]; w[1]=tv[1]; w[2]=tv[2];
        w[3]=(uint16_t)uL; w[4]=(uint16_t)vL;
        w[5]=(uint16_t)uH; w[6]=(uint16_t)vL;
        w[7]=(uint16_t)uH; w[8]=(uint16_t)vH;
        w += 9;
    }
}
#endif

#ifdef MULTIROOM
/* mrt_lara.bin: constant per-face UVs into the shared atlas + header. Posed
 * verts are built at RUNTIME from the skeleton in mrt_lskin.bin. */
static const uint8_t *g_ltx_quads;   /* qcount * 24B (u16 v0..v3 ; 8 UVs)  */
static const uint8_t *g_ltx_tris;    /* tcount * 18B (u16 v0..v2 ; 6 UVs)  */
static const uint8_t *g_ltx_frames;  /* (legacy; unused with runtime skin)  */
static int g_ltx_atH;                /* shared atlas height (blob header)  */

/* --- runtime skinning (mrt_lskin.bin): skeleton + per-frame joint angles.
 * Lara is posed on the 68k each frame instead of storing pre-posed verts, so
 * ALL ~160 of her animations fit. Pose math is a fixed-point (.12) port of the
 * extractor's bake(): a matrix stack walks the node hierarchy, YXZ-euler joint
 * rotations from the 256-entry SINTAB, mesh-local verts transformed to Lara-
 * local space, then heading-rotated + placed on the floor (as before). --- */
static const uint8_t *g_sk_meshinfo; /* mcount * {u16 vbase, vlen}          */
static const uint8_t *g_sk_meshverts;/* vcount  * {s16 x,y,z}  (local)      */
static const uint8_t *g_sk_nodes;    /* (mcount-1) * {u16 flags, s16 x,y,z} */
static const uint8_t *g_sk_frames;   /* framecount * framestride            */
static const uint8_t *g_sk_anims;    /* animcount * {u16 start,cnt; u8 st,rt}*/
static int g_sk_mcount, g_sk_framestride, g_sk_animcount, g_sk_framecount;
static int g_lframe;                 /* absolute skin-frame index to pose   */
static int g_anim_start;             /* frame-0 of the current anim (in-place root) */
static int g_camx, g_camy, g_camz;   /* camera world pos (for Lara depth sort) */
static uint8_t g_qmesh[256], g_tmesh[256];  /* which mesh each face belongs to */

/* pose fast path: skeleton data converted ONCE at init from the big-endian
 * blob into native s16 arrays (kills the per-component byte-assembly in the
 * hot loop), plus faces pre-grouped by mesh (kills the 15x375 scan per frame). */
static int16_t  skv[MRT_LSKIN_VCOUNT*3];          /* mesh-local verts (native) */
static int16_t  sknode[(MRT_LSKIN_MCOUNT-1)*4];   /* nodes: flags,x,y,z        */
static uint16_t skvb[MRT_LSKIN_MCOUNT], skvl[MRT_LSKIN_MCOUNT]; /* vbase,vlen  */
static uint16_t mq_list[MRT_LARA_QCOUNT], mq_start[MRT_LSKIN_MCOUNT+1];
static uint16_t mt_list[MRT_LARA_TCOUNT], mt_start[MRT_LSKIN_MCOUNT+1];

static int rd16(const uint8_t *p){ return (int16_t)(((uint16_t)p[0]<<8)|p[1]); }

/* 68000 has NO 32-bit multiply (int*int => a slow __mulsi3 call). Every skinning
 * multiply has both operands within signed 16 bits (matrix .12 <=4096, cos/sin
 * <=4096, mesh verts small), so a single hardware muls.w replaces the whole
 * software routine — the pose loop's dominant cost. */
/* mul16 hoisted above the collision helpers (see there) */

typedef struct { int32_t R[3][3]; int32_t t[3]; } SkMat;   /* R in .12 fixed */
static void sk_ident(SkMat *m){
    int i,j; for(i=0;i<3;i++){ for(j=0;j<3;j++) m->R[i][j]=(i==j)?4096:0; m->t[i]=0; }
}
static void sk_rot(SkMat *m,int ia,int ib,int a8){       /* rotate cols ia,ib */
    int c=COS(a8)>>4, s=SIN(a8)>>4, i;                    /* SINTAB 16.16 -> .12 */
    for(i=0;i<3;i++){
        int a=m->R[i][ia], b=m->R[i][ib];
        m->R[i][ia]=(mul16(a,c) - mul16(b,s))>>12;
        m->R[i][ib]=(mul16(b,c) + mul16(a,s))>>12;
    }
}
static void sk_rot_yxz(SkMat *m,int ax,int ay,int az){    /* Y then X then Z */
    if(ay) sk_rot(m,0,2,ay);
    if(ax) sk_rot(m,2,1,ax);
    if(az) sk_rot(m,1,0,az);
}
static void sk_translate(SkMat *m,int x,int y,int z){
    int i; for(i=0;i<3;i++)
        m->t[i]+=(mul16(m->R[i][0],x) + mul16(m->R[i][1],y) + mul16(m->R[i][2],z))>>12;
}
static SkMat g_skstack[20];
#ifdef LPLANES
/* LPLANES (2026-07-26): cull Lara's back faces on the 68k using REAL per-face
 * planes, so the artifact does not depend on the kernel's screen-space signed
 * area (whose sign is quantised on her sub-pixel head triangles -> her face
 * painted over the back of her skull).  Her meshes are RIGID, so the plane is
 * STATIC in mesh-local space and baked by the extractor; at runtime we only
 * transform the CAMERA into each mesh's space -- 15 per frame, not 375 normal
 * rotations.  Visible iff (N . C_local - d) < 0  (N points inward: WINDSIGN=-1).
 * This also SHRINKS the blob Tom has to chew, so it should pay for itself. */
#include "mrt_lplanes.h"
static int32_t g_clocal[MRT_LSKIN_MCOUNT][3];   /* camera in each mesh's space */
static int32_t g_lcamL[3];                       /* camera in LARA-local space  */
static int g_lp_cull;                            /* faces culled this frame    */
#endif

/* SPLIT POSE: resumable across mesh ranges so each half hides under a
 * different GPU room draw (pose ~60ms > one room's ~40ms of GPU time).
 * Persistent state carries the matrix stack between parts. */
static SkMat ps_m;                    /* current matrix across parts        */
static int   ps_sp;                   /* matrix stack depth across parts    */
static uint16_t *ps_w;                /* vert write cursor across parts     */
static int32_t ps_mdepth[16]; static int ps_morder[16];
static void build_lara_part(uint8_t *buf, int atlasW, int m0, int m1)
{
    int laC = COS(g_layaw)>>2, laS = SIN(g_layaw)>>2;   /* .14 (fits 16b for muls.w) */
    int offX = g_lax >> 8, offZ = g_laz >> 8;
    int rx0 = g_lax & 255, rz0 = g_laz & 255;
    int base_y = g_lay + LARA_FEET;
    const uint8_t *fr   = g_sk_frames + g_lframe * g_sk_framestride;
    const uint8_t *ang  = fr + 6;                          /* current-frame angles */
    const uint8_t *rfr  = g_sk_frames + g_anim_start * g_sk_framestride;
    int rootx=rd16(rfr), rooty=rd16(rfr+2), rootz=rd16(rfr+4);  /* in-place root */
    int mc = g_sk_mcount, i, k;
    int wbaseX = offX<<8, wbaseZ = offZ<<8;   /* world base for depth calc */
#define mdepth ps_mdepth
#define morder ps_morder
#define sp ps_sp
#define m ps_m
    uint16_t *h = (uint16_t *)buf, *w;
    if (m1 > mc) m1 = mc;
    if (m0 == 0) {
        h[0]=(uint16_t)g_lnv; h[1]=(uint16_t)g_lnq; h[2]=(uint16_t)g_lnt;
        h[3]=(uint16_t)atlasW; h[4]=(uint16_t)g_ltx_atH;
        h[5]=(uint16_t)offX; h[6]=0; h[7]=(uint16_t)offZ;
        ps_w = (uint16_t *)(buf + 16);
        ps_sp = 0;
#ifdef LPLANES
        /* camera -> LARA-LOCAL space, ONCE per frame.  The vertex path maps
         * Lara-local (mx,my,mz) to world with the heading matrix
         * [laC laS; -laS laC] >> 14 plus the base offsets, so the inverse is
         * that matrix TRANSPOSED.  Clamped to s16 so mul16 (muls.w) is legal
         * even if the camera ends up unusually far from her. */
        { int dx = g_camx - (wbaseX + rx0);
          int dy = g_camy - base_y;
          int dz = g_camz - (wbaseZ + rz0);
          if (dx >  30000) dx =  30000; else if (dx < -30000) dx = -30000;
          if (dz >  30000) dz =  30000; else if (dz < -30000) dz = -30000;
          g_lcamL[0] = (mul16(dx,laC) - mul16(dz,laS)) >> 14;
          g_lcamL[1] = dy;
          g_lcamL[2] = (mul16(dx,laS) + mul16(dz,laC)) >> 14; }
#endif
    }
    w = ps_w;
    for (i = m0; i < m1; i++) {
        const int16_t *vp = skv + skvb[i]*3;
        int vlen = skvl[i];
        const uint8_t *a3 = ang + i*3;
        int32_t sx=0, sy=0, sz=0;
        if (i == 0) { sk_ident(&m); sk_translate(&m, rootx, rooty, rootz); }
        else {
            const int16_t *nd = sknode + (i-1)*4;
            if (nd[0] & 1) m = g_skstack[--sp];    /* POP  */
            if (nd[0] & 2) g_skstack[sp++] = m;     /* PUSH */
            sk_translate(&m, nd[1], nd[2], nd[3]);
        }
        sk_rot_yxz(&m, a3[0], a3[1], a3[2]);
#ifdef LPLANES
        /* camera -> this mesh's local space: undo the mesh matrix (R is
         * orthonormal, so R^T is its inverse) after undoing Lara's heading. */
        { int j; int32_t lm[3];
          lm[0]=g_lcamL[0]-m.t[0]; lm[1]=g_lcamL[1]-m.t[1]; lm[2]=g_lcamL[2]-m.t[2];
          for (j=0;j<3;j++)
              g_clocal[i][j]=(mul16(m.R[0][j],lm[0]) + mul16(m.R[1][j],lm[1])
                            + mul16(m.R[2][j],lm[2])) >> 12; }
#endif
        for (k = 0; k < vlen; k++) {
            int lx=vp[0], ly=vp[1], lz=vp[2]; vp += 3;
            int mx=m.t[0]+((mul16(m.R[0][0],lx) + mul16(m.R[0][1],ly) + mul16(m.R[0][2],lz))>>12);
            int my=m.t[1]+((mul16(m.R[1][0],lx) + mul16(m.R[1][1],ly) + mul16(m.R[1][2],lz))>>12);
            int mz=m.t[2]+((mul16(m.R[2][0],lx) + mul16(m.R[2][1],ly) + mul16(m.R[2][2],lz))>>12);
            int rx=((mul16(mx,laC) + mul16(mz,laS))>>14)+rx0;
            int rz=((mul16(-mx,laS) + mul16(mz,laC))>>14)+rz0;
            int wy=base_y+my;
            w[0]=(uint16_t)(int16_t)rx;
            w[1]=(uint16_t)(int16_t)wy;
            w[2]=(uint16_t)(int16_t)rz;
            w[3]=255;
            w += 4;
            sx+=rx; sy+=wy; sz+=rz;
        }
        /* mesh centroid depth (squared dist from camera) for painter sort */
        if (vlen>0){ int cx=wbaseX+sx/vlen-g_camx, cy=sy/vlen-g_camy, cz=wbaseZ+sz/vlen-g_camz;
                     mdepth[i]=mul16(cx,cx)+mul16(cy,cy)+mul16(cz,cz); } else mdepth[i]=0;
        morder[i]=i;
    }
    ps_w = w;
    if (m1 < mc) return;               /* later part finishes the blob */
    /* sort meshes FAR->NEAR (tiny insertion sort; mc<=15) so limbs behind the
       body draw first and don't show through (the kernel has no depth buffer). */
    for (i=1;i<mc;i++){ int j=i, mo=morder[i]; int32_t md=mdepth[mo];
        while (j>0 && mdepth[morder[j-1]] < md){ morder[j]=morder[j-1]; j--; }
        morder[j]=mo; }
    /* emit faces grouped by mesh in that order via the PREGROUPED per-mesh
       face lists (built once at init - no per-frame face scan). */
    { const uint16_t *qb=(const uint16_t*)g_ltx_quads;
      const uint16_t *tb=(const uint16_t*)g_ltx_tris;
      int mo, x;
#ifdef LPLANES
      int nq_out=0, nt_out=0;
      g_lp_cull=0;
#define LP_BACKFACE(tab,dtab,idx,mm) \
        ((mul16(tab[idx][0],(int)g_clocal[mm][0]) + mul16(tab[idx][1],(int)g_clocal[mm][1]) \
        + mul16(tab[idx][2],(int)g_clocal[mm][2])) - dtab[idx] >= 0)
#endif
      for (mo=0;mo<mc;mo++){ int mm=morder[mo];
        for (x=mq_start[mm];x<mq_start[mm+1];x++){
            const uint16_t *s=qb+mq_list[x]*12;
#ifdef LPLANES
            if (LP_BACKFACE(mrt_lplane_qn,mrt_lplane_qd,mq_list[x],mm)) { g_lp_cull++; continue; }
            nq_out++;
#endif
            EMIT_PLANE(w);
            for(k=0;k<12;k++) w[k]=s[k]; w+=12; } }
      for (mo=0;mo<mc;mo++){ int mm=morder[mo];
        for (x=mt_start[mm];x<mt_start[mm+1];x++){
            const uint16_t *s=tb+mt_list[x]*9;
#ifdef LPLANES
            if (LP_BACKFACE(mrt_lplane_tn,mrt_lplane_td,mt_list[x],mm)) { g_lp_cull++; continue; }
            nt_out++;
#endif
            EMIT_PLANE(w);
            for(k=0;k<9;k++) w[k]=s[k]; w+=9; } }
#ifdef LPLANES
      /* the blob header MUST match what was actually emitted - a stale count
       * here is exactly what black-screened a flash on 2026-07-25 */
      h[1]=(uint16_t)nq_out; h[2]=(uint16_t)nt_out;
#endif
    }
#undef mdepth
#undef morder
#undef sp
#undef m
}
static void build_lara_tex(uint8_t *buf, int atlasW)
{   /* one-shot fallback (no-rooms path) */
    build_lara_part(buf, atlasW, 0, g_sk_mcount);
}

/* JERRY POSE finish: Jerry wrote the posed verts; compute per-mesh centroid
 * depths from the blob, sort far->near, emit faces. (The 68k's remaining
 * share of Lara is this ~5ms; the ~55ms vert math now runs on the DSP in
 * parallel with Tom's room draws.) */
/* ---- SFX (resident Jerry mixer) ---- */
#include "sfx.h"
extern const uint8_t sfx_bank[];
extern void jerry_sfx(int, const void*, uint32_t, uint32_t);
static int g_sfx_ok;
#define SFX_STEP_RATE 45211u    /* 11025Hz -> ~16kHz mixer, 16.16 */
/* SOUND PAGE (2026-08-04): 0 mutes the class entirely; DSP per-sample gain
   is a follow-up, so mid slider positions are visual-only for now. */
static int g_musvol = 10, g_sfxvol = 10;
static void sfx_play(int voice, int id)
{
    const uint8_t *b = sfx_bank;
    int n = (b[0]<<8)|b[1];
    uint32_t off, len;
    if (!g_sfx_ok || id < 0 || id >= n) return;
    if (!g_sfxvol) return;
    off = ((uint32_t)b[4+id*8]<<24)|((uint32_t)b[5+id*8]<<16)
        | ((uint32_t)b[6+id*8]<<8)|b[7+id*8];
    len = ((uint32_t)b[8+id*8]<<24)|((uint32_t)b[9+id*8]<<16)
        | ((uint32_t)b[10+id*8]<<8)|b[11+id*8];
    jerry_sfx(voice, sfx_bank + off, len, SFX_STEP_RATE);
}

#ifdef PROFILE
/* half-line uclock (63.5us ticks): frame_count*525 + VC. Sub-phase probes
   for the 68k-side cost (coarse vbl buckets can't see it). */
static uint32_t hlm[10]; static uint32_t hla[10];
static uint32_t g_lemits;
/* HONEST fps (2026-07-22, PACING campaign): fps100's pftt window opens at
   pfA (AFTER game logic) — it excludes the logic phase entirely and
   overstated fps ~4.7x at spawn (fps100=2500 while wall-clocked block
   cadence — 60 renders per fps-block — measured 11-12s = 5.3 fps TRUE).
   pftt2 spans loop-top to loop-top; fpsT is the number to trust. */
static uint32_t pftt2, pftop;
#define HLP(i) (hlm[i] = frame_count*525u + (uint32_t)VC)

/* ROOMS-BLOCK sub-profile. The coarse vbl bars say the frame is 100% "rooms";
   these split that block into its four phases. NOTE the hla[] trap: a probe
   inside an `if` goes STALE when the branch is skipped, and the d<60000 guard
   (1.9s!) is far too loose to catch it — that is why hla[7] read a bogus 99.9%.
   RP_ALL() stamps every slot at block entry, so a phase that does not run
   contributes exactly 0 instead of a multi-frame delta. */
static uint32_t g_rp[10]; static uint32_t g_rpa[10];
#define RP(i)   (g_rp[i] = frame_count*525u + (uint32_t)VC)
#define RP_ALL() do { uint32_t _t = frame_count*525u + (uint32_t)VC; \
                      int _i; for (_i=0;_i<10;_i++) g_rp[_i]=_t; } while (0)
#ifdef PACEPROBE
/* PACING discriminator (PACING_CAMPAIGN.md step 2): hl_clear is a 3-way
   blend (collect-wait + safe-VC dodge + clear blit) and NOTHING measures
   Tom's real render span under PIPELINE ("hl_tom" = dispatch..kick only).
   pp_wait = 68k halflines asleep in the loop-top collect; pp_span = Tom
   kick -> collect-return (>= Tom's true render span; == it when pp_wait>0);
   pp_safe/pp_blit split the rest of hl_clear; pp_mid counts mid-frame
   fallback collects (the 3685 site) that would invalidate the model. */
static uint32_t pp_kick, pp_wait, pp_span, pp_safe, pp_blit, pp_mid;
/* rev 2: fps100's window (pfA..loop-end) EXCLUDES the logic phase — it
   overstates. pp_per/pp_pmax = HONEST wall period between consecutive
   render passes (loop-top deltas); pp_coll = how many loop-top collects
   actually ran (the missing denominator for pp_wait/pp_span averages);
   pp_wmax/pp_smax = worst single collect-wait / kick->collect span (the
   monster-stall tail that per-block averages smear away). */
static uint32_t pp_prev0, pp_per, pp_pmax, pp_coll, pp_wmax, pp_smax;
#define PPNOW() (frame_count*525u + (uint32_t)VC)
#define PPADD(acc, d) do { uint32_t _d = (d); if (_d < 60000u) (acc) += _d; } while (0)
#endif
#else
#define HLP(i) ((void)0)
#define RP(i) ((void)0)
#define RP_ALL() ((void)0)
#endif

static int32_t g_mtpos[16*3] __attribute__((aligned(8)));  /* Jerry: mesh T per pose */

#ifdef LEMITDIET
/* LARA EMIT DIET (2026-07-22, PERFHUNT_CAMPAIGN.md): the face re-emit on a
 * mesh-order flip fires on MOST driven renders (order changed in 61% of
 * 5-field windows while turning, measured in-emu) and copied ~13KB via a
 * per-face gather (mq_list[x]*12 random reads + per-record plane writes).
 * Diet, three stacked cuts:
 *  1. PAYLOAD-ONLY RE-EMIT: record slots are fixed-size, so the STAGEDIET
 *     12B plane prefixes sit at ORDER-INDEPENDENT offsets and are constant
 *     -> written once at init, never re-emitted (-33% of writes).
 *  2. PRE-GROUPED BANK: payloads packed per-mesh at init; re-emit copies
 *     are sequential movem.l bursts (lemit_qcopy/tcopy in cpu68k.S), no
 *     per-face pointer math, ~1 fetch word per 3 data words.
 *  3. CHANGED-WINDOW EMIT: only mesh positions first-diff..last-diff are
 *     re-copied.  The untouched prefix has identical meshes at identical
 *     offsets; the untouched suffix holds the same meshes at the same
 *     offsets because [fd..ld] is a permutation of the same mesh set
 *     (prefix equal + suffix equal => middle multisets equal).  Measured
 *     driven window: median 12/15 (turn churn) — worth ~20%, the floor.
 * Byte-for-byte the same blob content as the legacy emit. */
extern void lemit_qcopy(void *dst, const void *src, int n, int stride);
extern void lemit_tcopy(void *dst, const void *src, int n, int stride);
static uint8_t  lemit_qbank[MRT_LARA_QCOUNT*24] __attribute__((aligned(4)));
static uint8_t  lemit_tbank[MRT_LARA_TCOUNT*18] __attribute__((aligned(4)));
static uint16_t lemit_qoff[MRT_LSKIN_MCOUNT+1], lemit_toff[MRT_LSKIN_MCOUNT+1];

static void lemit_init(uint8_t *buf)
{
    int mc = g_sk_mcount, m, x;
    const uint16_t *qb=(const uint16_t*)g_ltx_quads;
    const uint16_t *tb=(const uint16_t*)g_ltx_tris;
    uint8_t *qo = lemit_qbank, *to = lemit_tbank;
    for (m = 0; m < mc; m++) {
        lemit_qoff[m] = (uint16_t)(qo - lemit_qbank);
        for (x = mq_start[m]; x < mq_start[m+1]; x++) {
            const uint32_t *s=(const uint32_t*)(qb+mq_list[x]*12);
            uint32_t *d=(uint32_t*)qo;
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; d[4]=s[4]; d[5]=s[5];
            qo += 24;
        }
        lemit_toff[m] = (uint16_t)(to - lemit_tbank);
        for (x = mt_start[m]; x < mt_start[m+1]; x++) {
            const uint16_t *s=tb+mt_list[x]*9;
            uint32_t *d=(uint32_t*)to;
            d[0]=((const uint32_t*)s)[0]; d[1]=((const uint32_t*)s)[1];
            d[2]=((const uint32_t*)s)[2]; d[3]=((const uint32_t*)s)[3];
            ((uint16_t*)to)[8]=s[8];
            to += 18;
        }
    }
#ifdef STAGEDIET
    {   /* constant plane prefixes, one-time (see note 1 above) */
        uint16_t *w = (uint16_t *)(buf + 16) + g_lnv*4;
        int k;
        for (k = 0; k < g_lnq; k++) { EMIT_PLANE(w); w += 12; }
        for (k = 0; k < g_lnt; k++) { EMIT_PLANE(w); w += 9;  }
    }
#else
    (void)buf;
#endif
}

static void lemit_range(uint8_t *buf, int fd, int ld)
{
    uint8_t *qregion = buf + 16 + (uint32_t)g_lnv*8;
    uint8_t *tregion = qregion + (uint32_t)g_lnq*QREC;
    int p, qslot = 0, tslot = 0;
    for (p = 0; p < fd; p++) { int mm = ps_morder[p];
        qslot += mq_start[mm+1]-mq_start[mm];
        tslot += mt_start[mm+1]-mt_start[mm]; }
    { uint8_t *qd = qregion + qslot*QREC + (QREC-24);
      uint8_t *td = tregion + tslot*TREC + (TREC-18);
      for (p = fd; p <= ld; p++) { int mm = ps_morder[p];
          int nq = mq_start[mm+1]-mq_start[mm];
          int nt = mt_start[mm+1]-mt_start[mm];
          if (nq) { lemit_qcopy(qd, lemit_qbank+lemit_qoff[mm], nq, QREC); qd += nq*QREC; }
          if (nt) { lemit_tcopy(td, lemit_tbank+lemit_toff[mm], nt, TREC); td += nt*TREC; }
      } }
}
#endif /* LEMITDIET */

static void lara_finish(uint8_t *buf, int atlasW)
{
    int mc = g_sk_mcount, i, k;
    int offX = g_lax >> 8, offZ = g_laz >> 8;
    int wbaseX = offX<<8, wbaseZ = offZ<<8;
    uint16_t *w;
    (void)atlasW;
    /* centroid mesh depth (vert-summing). MEASURED BEST (4.80 vs 4.56):
       the T-export/joint-origin variant (g_mtpos, kernel block dormant via
       params[5]=0) made the sort order flip ~46/60 frames -> the face
       re-emit dominated. Centroids average the limb: stabler order. */
#ifdef M68D_A3
    /* PERFHUNT A3: (a) the vertex sums read the big-endian s16 fields as
       WORDS (the blob is even-aligned; halves the DRAM accesses of the
       hottest awake-68k loop in driven play — 9.8% of wall), (b) the 48
       per-render __divsi3 calls (3 per mesh) become one reciprocal
       multiply each (vlen is a per-mesh constant; recip fits s16 for
       vlen>=3, meshes are all bigger).  Centroid rounding differs from
       C division by <=1 unit — only exact depth TIES could reorder, and
       the emit cache absorbs order-stability anyway. */
    { static uint16_t ps_recip[16]; static int ps_recip_ok = 0;
      if (!ps_recip_ok) { for (i=0;i<mc && i<16;i++)
              ps_recip[i] = (skvl[i]>=3) ? (uint16_t)(65536u/(unsigned)skvl[i]) : 0;
          ps_recip_ok = 1; }
      for (i = 0; i < mc; i++) {
        const int16_t *v = (const int16_t *)(buf + 16 + skvb[i]*8);
        int vlen = skvl[i];
        int32_t sx=0, sy=0, sz=0;
        for (k = 0; k < vlen; k++) {
            sx += v[0]; sy += v[1]; sz += v[2];
            v += 4;
        }
        if (vlen>=3){ int rc=(int)ps_recip[i];
                     int cx=wbaseX+(mul32x16(sx,rc)>>16)-g_camx;
                     int cy=(mul32x16(sy,rc)>>16)-g_camy;
                     int cz=wbaseZ+(mul32x16(sz,rc)>>16)-g_camz;
                     ps_mdepth[i]=mul16(cx,cx)+mul16(cy,cy)+mul16(cz,cz); }
        else if (vlen>0){ int cx=wbaseX+sx/vlen-g_camx, cy=sy/vlen-g_camy, cz=wbaseZ+sz/vlen-g_camz;
                     ps_mdepth[i]=mul16(cx,cx)+mul16(cy,cy)+mul16(cz,cz); } else ps_mdepth[i]=0;
        ps_morder[i]=i;
      } }
#else
    for (i = 0; i < mc; i++) {
        const uint8_t *v = buf + 16 + skvb[i]*8;
        int vlen = skvl[i];
        int32_t sx=0, sy=0, sz=0;
        for (k = 0; k < vlen; k++) {
            sx += (int16_t)((v[0]<<8)|v[1]);
            sy += (int16_t)((v[2]<<8)|v[3]);
            sz += (int16_t)((v[4]<<8)|v[5]);
            v += 8;
        }
        if (vlen>0){ int cx=wbaseX+sx/vlen-g_camx, cy=sy/vlen-g_camy, cz=wbaseZ+sz/vlen-g_camz;
                     ps_mdepth[i]=mul16(cx,cx)+mul16(cy,cy)+mul16(cz,cz); } else ps_mdepth[i]=0;
        ps_morder[i]=i;
    }
#endif
    for (i=1;i<mc;i++){ int j=i, mo=ps_morder[i]; int32_t md=ps_mdepth[mo];
        while (j>0 && ps_mdepth[ps_morder[j-1]] < md){ ps_morder[j]=ps_morder[j-1]; j--; }
        ps_morder[j]=mo; }
    HLP(8);   /* depth+sort done; 8-3 = mdepth cost, 4-3 = whole finish */
#ifdef LEMITDIET
    (void)w;
    /* changed-window emit (see lemit_* above): find first/last differing
       mesh position; equal orders skip entirely (the old cache hit). */
    { static int prev_order[16], prev_valid = 0;
      int fd = 0, ld = mc - 1;
      if (prev_valid) {
          fd = -1; ld = -1;
          for (i = 0; i < mc; i++)
              if (prev_order[i] != ps_morder[i]) { if (fd < 0) fd = i; ld = i; }
          if (fd < 0) return;
      }
      for (i = 0; i < mc; i++) prev_order[i] = ps_morder[i];
      if (!prev_valid) lemit_init(buf);
      prev_valid = 1;
#ifdef PROFILE
      g_lemits++;
#endif
      lemit_range(buf, fd, ld);
    }
    return;
#else
    /* FACE-EMIT CACHE: the ~375 face records (7.6KB of copies) only depend on
       the mesh SORT ORDER, and the camera rides behind Lara so it rarely
       changes — skip the whole emit when the order matches last frame's
       (Jerry rewrites only the VERTS region; faces persist in lara_blob). */
    { static int prev_order[16], prev_valid = 0;
      int same = prev_valid;
      for (i = 0; i < mc && same; i++) if (prev_order[i] != ps_morder[i]) same = 0;
      if (same) return;
      for (i = 0; i < mc; i++) prev_order[i] = ps_morder[i];
      prev_valid = 1; }
#ifdef PROFILE
    g_lemits++;
#endif
    w = (uint16_t *)(buf + 16) + g_lnv*4;
    { const uint16_t *qb=(const uint16_t*)g_ltx_quads;
      const uint16_t *tb=(const uint16_t*)g_ltx_tris;
      int mo, x;
      /* longword copies: face records are even-aligned both sides (68000
         allows even-aligned longs); quad = 6 longs, tri = 4 longs + 1 word */
      for (mo=0;mo<mc;mo++){ int mm=ps_morder[mo];
        for (x=mq_start[mm];x<mq_start[mm+1];x++){
            const uint32_t *s=(const uint32_t*)(qb+mq_list[x]*12);
            uint32_t *d;
            EMIT_PLANE(w);
            d=(uint32_t*)w;
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; d[4]=s[4]; d[5]=s[5];
            w+=12; } }
      for (mo=0;mo<mc;mo++){ int mm=ps_morder[mo];
        for (x=mt_start[mm];x<mt_start[mm+1];x++){
            const uint16_t *s=tb+mt_list[x]*9;
            uint32_t *d;
            EMIT_PLANE(w);
            d=(uint32_t*)w;
            d[0]=((const uint32_t*)s)[0]; d[1]=((const uint32_t*)s)[1];
            d[2]=((const uint32_t*)s)[2]; d[3]=((const uint32_t*)s)[3];
            w[8]=s[8];
            w+=9; } }
    }
#endif /* !LEMITDIET */
}
/* DEPTH-SORT OT: order one Jerry-cached room's faces far->near. Runs on
   the 68k while Tom rasters earlier rooms (time it used to sleep through).
   Writes qlist/tlist of RECORD ADDRESSES; Tom's face iterator walks them.
   DEAD since JERRYX retirement (no caller) — strides here are pre-STAGEDIET
   (24/18); fix to QREC/TREC if ever revived. */
static void ot_sort(const uint8_t *room, uint32_t *slot)
{
    int vc = rd16(room), qc = rd16(room+2), tc = rd16(room+4);
    const uint32_t *dep = slot + 1028;
    uint32_t *ql = slot + 1540, *tl = slot + 1988;
    uint32_t qbase = (uint32_t)(room + 16 + vc*8);
    uint32_t tbase = qbase + (uint32_t)qc*24;
    int cnt[16], pos[16], i, b;
    if (qc > 448) qc = 448;
    if (tc > 256) tc = 256;
    /* quads */
    for (b = 0; b < 16; b++) cnt[b] = 0;
    for (i = 0; i < qc; i++) {
        const uint8_t *r = (const uint8_t *)(qbase + i*24);
        uint32_t d0, d;
        d = dep[rd16(r)]; d0 = dep[rd16(r+2)]; if (d0 > d) d = d0;
        d0 = dep[rd16(r+4)]; if (d0 > d) d = d0;
        d0 = dep[rd16(r+6)]; if (d0 > d) d = d0;
        b = (int)(d >> 10); if (b > 15) b = 15;
        cnt[b]++;
        ((uint8_t *)&ql[i])[0] = (uint8_t)b;   /* stash bucket in entry */
        ql[i] = ((uint32_t)b << 28) | (i & 0xFFFFFF);
    }
    pos[15] = 0;                       /* FAR (big depth) placed FIRST */
    for (b = 14; b >= 0; b--) pos[b] = pos[b+1] + cnt[b+1];
    { static uint32_t tmp[448];
      for (i = 0; i < qc; i++) tmp[i] = ql[i];
      for (i = 0; i < qc; i++) {
          uint32_t e = tmp[i]; int bb = (int)(e >> 28);
          ql[pos[bb]++] = qbase + (e & 0xFFFFFF)*24;
      } }
    /* tris */
    for (b = 0; b < 16; b++) cnt[b] = 0;
    for (i = 0; i < tc; i++) {
        const uint8_t *r = (const uint8_t *)(tbase + i*18);
        uint32_t d0, d;
        d = dep[rd16(r)]; d0 = dep[rd16(r+2)]; if (d0 > d) d = d0;
        d0 = dep[rd16(r+4)]; if (d0 > d) d = d0;
        b = (int)(d >> 10); if (b > 15) b = 15;
        cnt[b]++;
        tl[i] = ((uint32_t)b << 28) | (i & 0xFFFFFF);
    }
    pos[15] = 0;
    for (b = 14; b >= 0; b--) pos[b] = pos[b+1] + cnt[b+1];
    { static uint32_t tmp2[256];
      for (i = 0; i < tc; i++) tmp2[i] = tl[i];
      for (i = 0; i < tc; i++) {
          uint32_t e = tmp2[i]; int bb = (int)(e >> 28);
          tl[pos[bb]++] = tbase + (e & 0xFFFFFF)*18;
      } }
    slot[1] = (uint32_t)qc;
    slot[2] = (uint32_t)tc;
}
static int g_jerry_ok;
static int g_jerry_frame_done;   /* pose consumed inside the dispatch block */

/* ---- animation driver: pick a TR animation (relative index) and advance ---- */
static int g_lanim_id = LANIM_STOP, g_lanim_fr;
static int g_lsub;   /* 8.8 sub-frame accumulator for ANIMRATE */
static void lanim_set(int id){ if(id!=g_lanim_id){ g_lanim_id=id; g_lanim_fr=0; g_lsub=0; } }
/* FOOTSTEP PHASE LOCK (2026-08-01). Did the frame window (prev, cur] pass
   frame f? The animation advances 2 frames per tick and wraps, so a plain
   equality test would MISS the footfall half the time. Wrap-aware on purpose.
   Footsteps used to fire off a free-running tick counter (every 3 ticks
   running, 5 walking) with no reference to the animation at all, so the sound
   and the foot were on independent clocks - and since the frame rate varies
   (7-9 fps) the phase wandered continuously. The user heard it immediately:
   "the sounds of footsteps do not line up with when Lara's feet hit the
   ground". */
static int anim_crossed(int prev, int cur, int cnt, int f)
{
    if (prev < 0 || cur == prev) return 0;
    if (cur > prev) return (f > prev && f <= cur);
    return (f > prev && f < cnt) || (f <= cur);      /* wrapped this tick */
}
/* NOINLINE ON PURPOSE. Inlined into main() this block's locals churn register
   allocation across a ~6,000-instruction function, and that is the A10 boot
   hang: the first cut of this fix black-screened on silicon with the body
   inline. The sidestep and the ring bake needed exactly the same treatment.
   Its own frame keeps main() alone. */
__attribute__((noinline))
static void lara_footstep(int cnt, int cur, int id)
{
    static int prevfr = -1, previd = -1;
    int f1, f2;
    if (cnt < 1) cnt = 1;
    /* TR1's OWN footfall frames, from the animation's SOUND commands (the
       extractor now emits them as mrt_lara_foot[]). The old code guessed one
       pair of EIGHTHS for every animation, which is late for the RUN (real:
       4 and 15 of 22) and wrong for the WALK (real: 4 and 24 of 36) — a single
       fraction cannot serve both, which is why footsteps never sat under her
       feet. 255 = this animation has no footfall. */
    if (id >= 0 && id < MRT_LARA_ANIMCOUNT) {
        f1 = mrt_lara_foot[id][0];
        f2 = mrt_lara_foot[id][1];
        if (f1 >= cnt) f1 = -1;
        if (f2 >= cnt) f2 = -1;
    } else {
        f1 = (LARA_FOOT1 * cnt) / 8;   /* fallback for ids outside the table */
        f2 = (LARA_FOOT2 * cnt) / 8;
    }
    if (id != previd) prevfr = -1;      /* no spurious fire on walk<->run */
    if ((f1 >= 0 && anim_crossed(prevfr, cur, cnt, f1)) ||
        (f2 >= 0 && anim_crossed(prevfr, cur, cnt, f2)))
        sfx_play(0, SFX_STEP);
    prevfr = cur; previd = id;
}
static int  lanim_done(void){                 /* one-shot reached last frame? */
    return g_lanim_fr >= rd16(g_sk_anims+g_lanim_id*6+2) - 1;
}
static void lanim_step(int loop, int step){   /* advance + compute g_lframe   */
    int st=rd16(g_sk_anims+g_lanim_id*6), cnt=rd16(g_sk_anims+g_lanim_id*6+2);
    if(cnt<1) cnt=1;
#ifdef ANIMRATE
    /* ANIMRATE (2026-08-02): cadence from the ANIMATION'S OWN frameRate, not
     * a hand-picked step.  Our anim record is {u16 start, cnt; u8 st, rt} and
     * `rt` IS TR1's frameRate - the number of 30 Hz game frames between stored
     * keyframes.  We have carried it since the extractor was written and never
     * read it; every call site instead passed step=1 or step=2 by eye.
     *     anim  0 RUN  rt=1  -> one stored frame per game frame
     *     anim  1 WALK rt=2  -> one stored frame per TWO game frames
     * So the correct advance is 1/rt frames per tick, which needs sub-frame
     * precision - hence the 8.8 accumulator.  Passing step=2 for the run (as
     * the old code did) played her cycle at DOUBLE cadence. */
    { int rt = g_sk_anims[g_lanim_id*6+5];
      if (rt < 1) rt = 1;
      g_lsub += ((256 / rt) * g_tanim * (step > 0 ? 1 : 0)) >> 1;
      g_lanim_fr += (g_lsub >> 8);
      g_lsub &= 255; }
#elif defined(TIMESTEP)
    g_lanim_fr += step * g_tanim;   /* wall-clock, scaled by ANIMDIV */
#else
    g_lanim_fr += step;
#endif
    if(g_lanim_fr >= cnt){ if(loop) g_lanim_fr %= cnt; else g_lanim_fr = cnt-1; }
    g_lframe = st + g_lanim_fr; g_anim_start = st;
#ifdef LFREEZE
    /* DIAGNOSTIC (2026-07-26): pin the pose to ONE animation frame.  Splits the
     * head twitch into its two possible causes -- if her head still churns
     * frame-to-frame with a COMPLETELY STATIC pose, the instability is
     * NUMERICAL (accumulated .12 error down a 15-deep matrix chain, head last);
     * if it goes quiet, it is the idle animation being amplified down that
     * chain. */
    g_lframe = st; g_anim_start = st;
#endif
}

/* --- collectible PICKUP: a small spinning cube drawn by the same gpu_geotex
   kernel (8 verts, 6 quads). All faces sample one solid GOLD swatch cell
   (palette index PICKUP_IDX, poked into the atlas + CLUT at init). Lara
   collects it by walking into it. --- */
static int g_pickidx, g_dooridx;   /* free palette slots found at RUNTIME
   (10-room atlases use up to idx 253 — no hardcoded slot is safe; overwriting
   a used index tinted every texture sharing it, the "red everywhere" bug) */
#define ITEM_S      90             /* cube half-size (world units)        */
static const int16_t item_v[8][3] = {
  {-ITEM_S,-ITEM_S,-ITEM_S},{ ITEM_S,-ITEM_S,-ITEM_S},{ ITEM_S,-ITEM_S, ITEM_S},{-ITEM_S,-ITEM_S, ITEM_S},
  {-ITEM_S, ITEM_S,-ITEM_S},{ ITEM_S, ITEM_S,-ITEM_S},{ ITEM_S, ITEM_S, ITEM_S},{-ITEM_S, ITEM_S, ITEM_S}
};
static const uint16_t item_q[6][4] = {
  {0,3,2,1},{4,5,6,7},{0,1,5,4},{3,7,6,2},{0,4,7,3},{1,2,6,5}
};
static fix g_itemx, g_itemz, g_itemy;   /* item world position       */
static int g_itemcollected, g_pickups;  /* state + collected count   */
static uint8_t g_itemspin;              /* spin angle                */
#define ITEM_UL 1
#define ITEM_UH (MRT_LARA_CELL - 2)
/* ---- LEVEL SET SELECT (caves / Lara's Home): runtime pointers into the
   chosen data set. Structures are identical (verified: LANIM/LSKIN tables
   match; SW_Y always = atlasH - 2*CELL). Menu picks g_useset. ---- */
static int g_useset;                     /* 0 = caves, 1 = gym (Lara's Home) */
static const uint8_t  *S_index, *S_geom, *S_sect, *S_lara, *S_lskin;
static uint8_t        *S_atlas;
static const uint16_t *S_pal;
typedef unsigned char AdjRow[MRT_ADJ_MAX];
static const AdjRow   *S_adj;
static int g_swy;                        /* runtime swatch Y = atlasH - 16   */
/* room adjacency now comes from the extractor (portals + vertical sector
   links): mrt_adjgen / gym_adjgen in the *_spawn.h headers */
/* is room b portal-adjacent to (or the same as) room a? Grab/vault targets in
   non-adjacent rooms are through walls (courtyard->upper-interior exploits). */
static int room_reachable(int a, int b)
{
    int i;
    if (a == b) return 1;
    for (i = 0; i < MRT_ADJ_MAX && S_adj[a][i] != 255; i++)
        if (S_adj[a][i] == b) return 1;
    return 0;
}
/* b within K portal hops of a. K<=3 uses the fall-cull BFS; the HOPDIAL
   dispatch cap reuses it with its own cache. */
static int room_withinK(int a, int b, int K, int *cached_a_p, uint8_t *inset)
{
    if (a != *cached_a_p || K < 0) { /* K<0 = force refresh sentinel */
        int i, j, k, n1;
        for (i = 0; i < 64; i++) inset[i] = 0;
        inset[a] = 1;
        if (K >= 1)
        for (i = 0; i < MRT_ADJ_MAX && S_adj[a][i] != 255; i++) {
            n1 = S_adj[a][i]; inset[n1] = 1;
            if (K >= 2)
            for (j = 0; j < MRT_ADJ_MAX && S_adj[n1][j] != 255; j++) {
                int n2 = S_adj[n1][j]; inset[n2] = 1;
                if (K >= 3)
                for (k = 0; k < MRT_ADJ_MAX && S_adj[n2][k] != 255; k++)
                    inset[S_adj[n2][k]] = 1;
            }
        }
        *cached_a_p = a;
    }
    return inset[b & 63];
}
static int room_within3(int a, int b)
{
    static int cached_a = -1;
    static uint8_t inset[64];
    return room_withinK(a, b, 3, &cached_a, inset);
}
static int room_within3_dead(int a, int b)
{
    static int cached_a = -1;
    static uint8_t inset[64];
    if (a != cached_a) {
        int i, j, k, n1;
        for (i = 0; i < 64; i++) inset[i] = 0;
        inset[a] = 1;
        for (i = 0; i < MRT_ADJ_MAX && S_adj[a][i] != 255; i++) {
            n1 = S_adj[a][i]; inset[n1] = 1;
            for (j = 0; j < MRT_ADJ_MAX && S_adj[n1][j] != 255; j++) {
                int n2 = S_adj[n1][j]; inset[n2] = 1;
                for (k = 0; k < MRT_ADJ_MAX && S_adj[n2][k] != 255; k++)
                    inset[S_adj[n2][k]] = 1;
            }
        }
        cached_a = a;
    }
    return inset[b & 63];
}
#define ITEM_VL (g_swy + 1)
#define ITEM_VH (g_swy + MRT_LARA_CELL - 2)
static void build_item_blob(uint8_t *buf, int atlasW)
{
    fix c = COS(g_itemspin), s = SIN(g_itemspin);
    int offX = g_itemx >> 8, offZ = g_itemz >> 8;
    int rx0 = g_itemx & 255, rz0 = g_itemz & 255;
    uint16_t *h = (uint16_t *)buf; uint16_t *w; int i;
    h[0]=8; h[1]=6; h[2]=0; h[3]=(uint16_t)atlasW; h[4]=(uint16_t)g_ltx_atH;
    h[5]=(uint16_t)offX; h[6]=0; h[7]=(uint16_t)offZ;
    w = (uint16_t *)(buf + 16);
    for (i=0;i<8;i++) {
        int mx=item_v[i][0], my=item_v[i][1], mz=item_v[i][2];
        int rx=((mx*c + mz*s)>>16)+rx0;
        int rz=((-mx*s + mz*c)>>16)+rz0;
        w[0]=(uint16_t)(int16_t)rx; w[1]=(uint16_t)(int16_t)(g_itemy+my);
        w[2]=(uint16_t)(int16_t)rz; w[3]=255;
        w += 4;
    }
    for (i=0;i<6;i++) {
        EMIT_PLANE(w);
        w[0]=item_q[i][0]; w[1]=item_q[i][1]; w[2]=item_q[i][2]; w[3]=item_q[i][3];
        w[4]=ITEM_UL;w[5]=ITEM_VL; w[6]=ITEM_UH;w[7]=ITEM_VL;
        w[8]=ITEM_UH;w[9]=ITEM_VH; w[10]=ITEM_UL;w[11]=ITEM_VH;
        w += 12;
    }
}

/* --- DOOR: a wide/tall/thin slab (item cube scaled) that blocks a passage
   until Lara collects the pickup (the "key"); then it slides up out of the
   way. Brown, palette index DOOR_IDX. --- */

static fix g_doorx, g_doorz, g_doory0;   /* door base world position     */
static int g_dooryoff;                    /* slide-up offset (0=shut, -=open) */
#define DOOR_UL (MRT_LARA_CELL + 1)
#define DOOR_UH (2*MRT_LARA_CELL - 2)
#define DOOR_VL (g_swy + 1)
#define DOOR_VH (g_swy + MRT_LARA_CELL - 2)
static void build_door_blob(uint8_t *buf, int atlasW)
{
    int offX = g_doorx >> 8, offZ = g_doorz >> 8;
    int rx0 = g_doorx & 255, rz0 = g_doorz & 255;
    uint16_t *h = (uint16_t *)buf; uint16_t *w; int i;
    h[0]=8; h[1]=6; h[2]=0; h[3]=(uint16_t)atlasW; h[4]=(uint16_t)g_ltx_atH;
    h[5]=(uint16_t)offX; h[6]=0; h[7]=(uint16_t)offZ;
    w = (uint16_t *)(buf + 16);
    for (i=0;i<8;i++) {
        int mx = item_v[i][0]*5;    /* wide  */
        int my = item_v[i][1]*8;    /* tall  */
        int mz = item_v[i][2]/2;    /* thin  */
        w[0]=(uint16_t)(int16_t)(mx + rx0);
        w[1]=(uint16_t)(int16_t)(g_doory0 + g_dooryoff + my);
        w[2]=(uint16_t)(int16_t)(mz + rz0);
        w[3]=255;
        w += 4;
    }
    for (i=0;i<6;i++) {
        EMIT_PLANE(w);
        w[0]=item_q[i][0]; w[1]=item_q[i][1]; w[2]=item_q[i][2]; w[3]=item_q[i][3];
        w[4]=DOOR_UL;w[5]=DOOR_VL; w[6]=DOOR_UH;w[7]=DOOR_VL;
        w[8]=DOOR_UH;w[9]=DOOR_VH; w[10]=DOOR_UL;w[11]=DOOR_VH;
        w += 12;
    }
}

#ifdef ENTITIES
/* ====================================================================
   REAL DOORS + SWITCHES, from the level's own entity table and
   FloorData triggers (mrt_spawn.h).  Replaces the single hand-placed
   door above, which sat at a fixed offset from Lara's spawn and was
   opened by picking up a stand-in "key".

   LEVEL1 has 3 SWITCH entities and 8 doors:
     room 11 sect(1,3)   switch e10 -> opens e9  (DOOR_4)
     room 20 sect(2,16)  switch e52 -> opens e36, e35
     room 34 sect(18,3)  switch e42 -> opens e44, e43

   ☠️☠️ The extractor stores trigger command words VERBATIM, so BOTH TR1
   layout traps are decoded HERE, in one place:
     - a SWITCH/KEY/PICKUP trigger's FIRST word is the switch ENTITY
       INDEX, not an action;
     - CAMERA_SWITCH (action 1) eats the NEXT word as its parameter.
   Two of the three switches above carry a CAMERA_SWITCH, so skipping
   that word wrong silently drops the doors they open.
   ==================================================================== */
#define ENT_DOOR_LO   57
#define ENT_DOOR_HI   60
/* ---- DOORS SWING OUTWARD ON A HINGE (PS1 footage Part 2 @ 9m28s: the two
   carved panels stand OPEN AT AN ANGLE and Lara runs out between them).
   They were sliding straight up, which is not what the Caves doors do.
   Angles are SINTAB units - 256 per full turn, so a quarter turn is 64. */
#define DOOR_ANG_OPEN  64                /* 90 degrees = fully open     */
#define DOOR_ANG_STEP   3                /* swing per tick (~21 ticks)  */
#define DOOR_ANG_CLEAR 40                /* swung far enough to walk by */
/* ★ REAL MODEL DIMENSIONS, read out of the level file with MRT_DOORAUDIT=1
   (every door is a 1-mesh, 6-face BOX - none of them is the lashed pole
   lattice, which is room geometry, not an entity):
     DOOR_1/2/3 (57-59)  1024 x 2048 x 93
     DOOR_4     (60)     1000 x 1023 x 100
   We were drawing 900 x 1440 x 90 for all four. A door is exactly ONE SECTOR
   wide, which is also why DOOR_SPAN 512 is the right collision half-width. */
#define DOOR_SPANH    512                /* half the slab's span        */
#define DOOR_THINH     47                /* half its thickness          */
#define DOOR_H_TALL  2048                /* DOOR_1/2/3                  */
#define DOOR_H_SHORT 1023                /* DOOR_4                      */
#define SWITCH_REACH  700                /* |dx|+|dz| to reach a switch */

/* ONE BLOB PER DOOR DRAWN THIS FRAME.  The display list holds POINTERS that
   the renderer reads later, so a single shared buffer made every door in a
   room draw at the last one's position - rooms 14, 17 and 34 each hold two
   doors, so half of them silently vanished into the other. */
#define ENT_DOOR_MAXDRAW 4
static uint8_t ent_door_blob[ENT_DOOR_MAXDRAW][640] __attribute__((aligned(8)));
#define ENT_SW_MAXDRAW 2
static uint8_t ent_sw_blob[ENT_SW_MAXDRAW][640] __attribute__((aligned(8)));
#define ENT_BR_MAXDRAW 6      /* room 22 has 12 bridge pieces, 6 per column  */
static uint8_t ent_br_blob[ENT_BR_MAXDRAW][384] __attribute__((aligned(8)));
#define ENT_PK_MAXDRAW 3
static uint8_t ent_pk_blob[ENT_PK_MAXDRAW][384] __attribute__((aligned(8)));
#ifdef DARTS
#define ENT_DART_MAXDRAW 8
static uint8_t ent_dart_blob[ENT_DART_MAXDRAW][384] __attribute__((aligned(8)));
#endif
/* Lara's health is a GAME concept, not an enemy one: it lived inside the
   ENEMIES block, so anything else that can hurt her - the dart trap - could
   not even compile against it.  (And ENEMIES itself is not plumbed in the
   Makefile at all, so that block has never been built; see the note there.) */
#ifdef STAGECHK
static uint32_t g_stageck, g_stagecam, g_stagechg;
#endif
#ifdef BUSPROBE
/* WHO OWNS THE BUS, AND FOR HOW LONG?
 * The flicker scales with 68k bus pressure during Tom's render, so before
 * moving any work we need the split: how many halflines the 68000 spends
 * ACTIVE in the overlap window (loop top -> collect) versus how many it
 * spends ASLEEP in the collect waiting for Tom.  Sleep is free - STOP
 * releases the bus entirely.  Active time is what costs Tom his reads.
 *   bar 0 = mean active halflines per frame (the exposure window)
 *   bar 1 = mean asleep halflines per frame (0 => the 68k is the bottleneck)
 * A halfline is ~31.7us; a whole 60Hz field is 525. */
static uint32_t bp_t0, bp_active, bp_sleep, bp_safe, bp_blit, bp_n;
/* PHASE BREAKDOWN of the 68000's frame.  The active window is ~130ms and Tom
   idles through most of it, so this is where the frame rate lives.  Marks are
   taken at the loop's existing HLP() sites so the phases line up with the
   profiler's own vocabulary:
     p1 = loop top -> pre-collect   : game logic (pad, camera, Lara, collision,
                                      entities) - the part that runs UNDER Tom
     p2 = pre-collect -> post-clear : collect, flip, safe-vc, clear blit
     p3 = post-clear -> rooms done  : visibility walk, display-list build,
                                      Jerry pose + lara_finish, dispatch
     p4 = rooms done -> loop top    : HUD, overlays, tail */
static uint32_t bp_m, bp_p1, bp_p2, bp_p3, bp_p4, bp_frames;
/* p1 (the game logic, ~36% of the frame) split by what it does, to find out
   whether the eleven per-frame passes over the 60-entity table are actually
   where the time goes - before handing anything to Jerry across a protocol
   that has produced hardware-visible corruption once already. */
static uint32_t bp_s0, bp_sA, bp_sB, bp_sC, bp_sD;
#endif
static int  g_health = 1000;             /* TR1 full health              */

#if defined(ENEMIES)
/* THE BAT (enemy model 9): 45 verts, 41 faces, 8 fly-cycle frames baked by the
   extractor (mrt_bat.h). Flat dark swatch (door grey cell). First real enemy;
   wolf/bear are much heavier models. */
#define ENT_BAT_MAXDRAW 4
static uint8_t ent_bat_blob[ENT_BAT_MAXDRAW][1792] __attribute__((aligned(8)));
/* wolf/bear are ~6x the bat's faces (251/261) - cap simultaneous draws low
   and gate tight, face count IS the frame cost. Blobs sized for the largest
   model: 264v*16B + 173q*36B + 132t*30B + 16 hdr ~= 10.5KB, round to 10752. */
#define ENT_WOLF_MAXDRAW 2
#define ENT_BEAR_MAXDRAW 1
static uint8_t ent_wolf_blob[ENT_WOLF_MAXDRAW][10752] __attribute__((aligned(8)));
static uint8_t ent_bear_blob[ENT_BEAR_MAXDRAW][10752] __attribute__((aligned(8)));
static uint8_t g_batframe;               /* shared fly-cycle frame    */
static int g_batx[MRT_ENTCOUNT], g_baty[MRT_ENTCOUNT], g_batz[MRT_ENTCOUNT];
static uint8_t g_batinit;                /* positions seeded from spawn  */
static uint8_t g_batdead[MRT_ENTCOUNT];  /* killed by Lara               */
static int  g_firecd;                    /* fire cooldown ticks          */
static int  g_kills;                     /* enemies killed (stats)       */
static int ent_is_bat(int t) { return t == 9; }
static int ent_is_wolf(int t) { return t == 7; }
static int ent_is_bear(int t) { return t == 8; }
static int ent_is_enemy(int t) { return t==7 || t==8 || t==9; }

/* Shared enemy-model blob builder: every enemy (bat/wolf/bear) uses the same
   posed-verts + quad/tri-index layout the extractor emits, so one builder takes
   the model's tables and the live world position and writes a gpu_geotex blob.
   Flat DOOR grey swatch (no per-enemy texture yet). */
static void build_ent_model(uint8_t *buf, int atlasW, int wx, int wy, int wz,
                            const short *V, int vc,
                            const unsigned short (*Q)[4], int qc,
                            const unsigned short (*T)[3], int tc)
{
    int offX = wx>>8, offZ = wz>>8, rx0 = wx&255, rz0 = wz&255, i;
    uint16_t *h = (uint16_t *)buf; uint16_t *w;
    h[0]=(uint16_t)vc; h[1]=(uint16_t)qc; h[2]=(uint16_t)tc;
    h[3]=(uint16_t)atlasW; h[4]=(uint16_t)g_ltx_atH;
    h[5]=(uint16_t)offX; h[6]=0; h[7]=(uint16_t)offZ;
    w = (uint16_t *)(buf + 16);
    for (i=0;i<vc;i++) {
        w[0]=(uint16_t)(int16_t)(V[i*3]+rx0);
        w[1]=(uint16_t)(int16_t)(wy+V[i*3+1]);
        w[2]=(uint16_t)(int16_t)(V[i*3+2]+rz0);
        w[3]=255; w+=4;
    }
    for (i=0;i<qc;i++) {
        EMIT_PLANE(w);
        w[0]=Q[i][0]; w[1]=Q[i][1]; w[2]=Q[i][2]; w[3]=Q[i][3];
        w[4]=DOOR_UL;w[5]=DOOR_VL; w[6]=DOOR_UH;w[7]=DOOR_VL;
        w[8]=DOOR_UH;w[9]=DOOR_VH; w[10]=DOOR_UL;w[11]=DOOR_VH; w+=12;
    }
    for (i=0;i<tc;i++) {
        EMIT_PLANE(w);
        w[0]=T[i][0]; w[1]=T[i][1]; w[2]=T[i][2];
        w[3]=DOOR_UL;w[4]=DOOR_VL; w[5]=DOOR_UH;w[6]=DOOR_VL; w[7]=DOOR_UH;w[8]=DOOR_VH;
        w+=9;
    }
}
static void build_ent_bat(uint8_t *buf, int atlasW, int e, int frame)
{
    build_ent_model(buf, atlasW, g_batx[e], g_baty[e], g_batz[e],
                    MRT_BAT_verts[frame], MRT_BAT_VCOUNT,
                    MRT_BAT_quads, MRT_BAT_QCOUNT,
                    MRT_BAT_tris, MRT_BAT_TCOUNT);
}
static void build_ent_wolf(uint8_t *buf, int atlasW, int e, int frame)
{
    build_ent_model(buf, atlasW, g_batx[e], g_baty[e], g_batz[e],
                    MRT_WOLF_verts[frame], MRT_WOLF_VCOUNT,
                    MRT_WOLF_quads, MRT_WOLF_QCOUNT,
                    MRT_WOLF_tris, MRT_WOLF_TCOUNT);
}
static void build_ent_bear(uint8_t *buf, int atlasW, int e, int frame)
{
    build_ent_model(buf, atlasW, g_batx[e], g_baty[e], g_batz[e],
                    MRT_BEAR_verts[frame], MRT_BEAR_VCOUNT,
                    MRT_BEAR_quads, MRT_BEAR_QCOUNT,
                    MRT_BEAR_tris, MRT_BEAR_TCOUNT);
}
#endif

/* PICKUPS: CRYSTAL(83), MEDIKIT_SMALL(93), MEDIKIT_BIG(94) - spinning items
   Lara collects by walking over them (no inventory yet, just remove them). */
static int ent_is_pickup(int t) { return t==83 || t==93 || t==94; }
static uint8_t g_pickgot[MRT_ENTCOUNT];  /* pickup already collected */
static uint8_t g_pickspin;               /* shared pickup spin angle */
#define PICKUP_REACH 700                 /* |dx|+|dz| to grab it     */

/* a spinning pickup cube at the entity's floor position (gold swatch, like the
   old DEMO_PROPS item). Rests on the floor: cube bottom at the entity Y. */
static void build_ent_pickup(uint8_t *buf, int atlasW, int e)
{
    fix c = COS(g_pickspin), s = SIN(g_pickspin);
    /* float it ~1/3 sector above the floor and spin, TR-style */
    int wx = mrt_ent[e].x, wz = mrt_ent[e].z, y = mrt_ent[e].y - 300;
    int offX = wx >> 8, offZ = wz >> 8, rx0 = wx & 255, rz0 = wz & 255;
    uint16_t *h = (uint16_t *)buf; uint16_t *w; int i;
    h[0]=8; h[1]=6; h[2]=0; h[3]=(uint16_t)atlasW; h[4]=(uint16_t)g_ltx_atH;
    h[5]=(uint16_t)offX; h[6]=0; h[7]=(uint16_t)offZ;
    w = (uint16_t *)(buf + 16);
    for (i=0;i<8;i++) {
        int mx=item_v[i][0], my=item_v[i][1], mz=item_v[i][2];
        int rx=(int)(((int32_t)mx*c + (int32_t)mz*s)>>16)+rx0;
        int rz=(int)(((int32_t)-mx*s + (int32_t)mz*c)>>16)+rz0;
        w[0]=(uint16_t)(int16_t)rx; w[1]=(uint16_t)(int16_t)(y+my);
        w[2]=(uint16_t)(int16_t)rz; w[3]=255; w += 4;
    }
    for (i=0;i<6;i++) {
        EMIT_PLANE(w);
        w[0]=item_q[i][0]; w[1]=item_q[i][1]; w[2]=item_q[i][2]; w[3]=item_q[i][3];
        w[4]=ITEM_UL;w[5]=ITEM_VL; w[6]=ITEM_UH;w[7]=ITEM_VL;
        w[8]=ITEM_UH;w[9]=ITEM_VH; w[10]=ITEM_UL;w[11]=ITEM_VH;
        w += 12;
    }
}

#ifdef DARTS
/* ---- DART TRAP (user 2026-08-09, the room-0 pass) -------------------------
 * TR1's TRAP_DART_EMITTER (type 40) is a wall socket that spits a dart across
 * the corridor.  Level 1 has ten of them; the four the player meets first sit
 * in pairs facing each other across the passage past the start room -
 * emitters 5..8, at x=73216 (yaw 64) and x=76288 (yaw 192), which is +X and
 * -X: TR yaw 0 faces +Z, so the byte yaw maps to (sin, cos) directly and the
 * pairs fire INTO the corridor, exactly as the geometry implies.
 *
 * A dart is a thin box built along its own flight axis, so it reads as a
 * needle rather than a cube, and it is drawn through the same shared display
 * list every other entity uses.  It expires on a life counter rather than a
 * wall test: the corridor is three sectors wide and DART_LIFE * DART_SPEED
 * covers just over that, so a dart that misses dies about where the far wall
 * is anyway.  Damage is TR1's: 50 of 1000.
 */
#define DART_MAX    8
#define DART_SPEED  190          /* world units per tick                     */
#define DART_LIFE   26           /* ticks alive: ~3.5 sectors of travel      */
#define DART_HALF   13           /* half thickness                           */
#define DART_LEN    50           /* half length along the flight axis        */
#define DART_DMG    50           /* TR1 dart damage, out of 1000             */
#define DART_PERIOD 26           /* ticks between shots from one emitter     */
#define DART_RANGE  5120         /* emitter sleeps until Lara is this close  */
static struct { int x, y, z; short vx, vz; unsigned char life; } g_dart[DART_MAX];
static unsigned char g_dartcd[MRT_ENTCOUNT];   /* per-emitter cooldown */
static int ent_is_dart_emitter(int t) { return t == 40; }

/* thin box along the flight axis; same blob layout as the pickup cube */
static void build_ent_dart(uint8_t *buf, int atlasW, int d)
{
    int wx = g_dart[d].x, wz = g_dart[d].z, y = g_dart[d].y;
    int offX = wx >> 8, offZ = wz >> 8, rx0 = wx & 255, rz0 = wz & 255;
    int ax = g_dart[d].vx > 0 ? 1 : (g_dart[d].vx < 0 ? -1 : 0);
    int az = g_dart[d].vz > 0 ? 1 : (g_dart[d].vz < 0 ? -1 : 0);
    int px = -az, pz = ax;              /* perpendicular, in the floor plane */
    uint16_t *h = (uint16_t *)buf; uint16_t *w; int i;
    h[0]=8; h[1]=6; h[2]=0; h[3]=(uint16_t)atlasW; h[4]=(uint16_t)g_ltx_atH;
    h[5]=(uint16_t)offX; h[6]=0; h[7]=(uint16_t)offZ;
    w = (uint16_t *)(buf + 16);
    for (i = 0; i < 8; i++) {
        int lo = (item_v[i][2] > 0) ?  DART_LEN : -DART_LEN;   /* along flight */
        int si = (item_v[i][0] > 0) ?  DART_HALF : -DART_HALF; /* across       */
        int hi = (item_v[i][1] > 0) ?  DART_HALF : -DART_HALF; /* vertical     */
        w[0]=(uint16_t)(int16_t)(rx0 + ax*lo + px*si);
        w[1]=(uint16_t)(int16_t)(y + hi);
        w[2]=(uint16_t)(int16_t)(rz0 + az*lo + pz*si);
        w[3]=255; w += 4;
    }
    for (i = 0; i < 6; i++) {
        EMIT_PLANE(w);
        w[0]=item_q[i][0]; w[1]=item_q[i][1]; w[2]=item_q[i][2]; w[3]=item_q[i][3];
        w[4]=ITEM_UL;w[5]=ITEM_VL; w[6]=ITEM_UH;w[7]=ITEM_VL;
        w[8]=ITEM_UH;w[9]=ITEM_VH; w[10]=ITEM_UL;w[11]=ITEM_VH;
        w += 12;
    }
}

/* fire, fly, hit.  Called once per frame from the game loop. */
static void darts_update(int ticks, int room)
{
    int i, e;
    for (i = 0; i < DART_MAX; i++) {
        if (!g_dart[i].life) continue;
        g_dart[i].x += (int)g_dart[i].vx * ticks;
        g_dart[i].z += (int)g_dart[i].vz * ticks;
        if (g_dart[i].life <= ticks) { g_dart[i].life = 0; continue; }
        g_dart[i].life = (unsigned char)(g_dart[i].life - ticks);
        { int hx = g_lax - g_dart[i].x, hz = g_laz - g_dart[i].z;
          int hy = g_lay - g_dart[i].y;
          if (hx < 0) hx = -hx; if (hz < 0) hz = -hz; if (hy < 0) hy = -hy;
          if (hx < 200 && hz < 200 && hy < 620) {
              g_dart[i].life = 0;
              if (g_health > 0) g_health -= DART_DMG;
          } }
    }
    for (e = 0; e < MRT_ENTCOUNT; e++) {
        int dx, dz, si, ci, slot;
        if (!ent_is_dart_emitter(mrt_ent[e].type)) continue;
        dx = g_lax - mrt_ent[e].x; dz = g_laz - mrt_ent[e].z;
        if (dx < 0) dx = -dx; if (dz < 0) dz = -dz;
        if (dx + dz > DART_RANGE) { g_dartcd[e] = 0; continue; }
        (void)room;
        if (g_dartcd[e] > ticks) {
            g_dartcd[e] = (unsigned char)(g_dartcd[e] - ticks);
            continue;
        }
        g_dartcd[e] = DART_PERIOD;
        for (slot = 0; slot < DART_MAX && g_dart[slot].life; slot++)
            ;
        if (slot >= DART_MAX) continue;          /* all in flight - skip a shot */
        /* TR yaw: 0 faces +Z, and the byte is a full turn in 256 steps */
        si = (int)SIN((int)mrt_ent[e].yaw << 8);
        ci = (int)COS((int)mrt_ent[e].yaw << 8);
        g_dart[slot].x = mrt_ent[e].x;
        g_dart[slot].y = mrt_ent[e].y - 256;     /* socket height, not the floor */
        g_dart[slot].z = mrt_ent[e].z;
        g_dart[slot].vx = (short)((si * DART_SPEED) >> 16);
        g_dart[slot].vz = (short)((ci * DART_SPEED) >> 16);
        g_dart[slot].life = DART_LIFE;
    }
}
#endif /* DARTS */

/* one bridge as a THIN SLAB (box, 1 sector wide, ~90 thick) so it is visible
   from ANY angle - a zero-thickness quad vanishes edge-on when Lara stands at
   the bridge's own height. Top surface at the entity Y (where she walks);
   textured with the door's carved-stone tile under DOORTEX, swatch otherwise. */
#define BR_HW  512            /* sector half-width in X/Z            */
#define BR_TH   90            /* slab thickness (down from the top)  */
static void build_ent_bridge(uint8_t *buf, int atlasW, int e)
{
    int wx = mrt_ent[e].x, wz = mrt_ent[e].z, y = mrt_ent[e].y;
    int offX = wx >> 8, offZ = wz >> 8, rx0 = wx & 255, rz0 = wz & 255;
    uint16_t *h = (uint16_t *)buf; uint16_t *w; int i;
    uint16_t *hh;
    h[0]=8; h[1]=6; h[2]=0; h[3]=(uint16_t)atlasW; h[4]=(uint16_t)g_ltx_atH;
    h[5]=(uint16_t)offX; h[6]=0; h[7]=(uint16_t)offZ;
    w = (uint16_t *)(buf + 16);
    for (i=0;i<8;i++) {
        int mx = item_v[i][0] > 0 ?  BR_HW : -BR_HW;
        int mz = item_v[i][2] > 0 ?  BR_HW : -BR_HW;
        int my = item_v[i][1] > 0 ?  BR_TH : 0;      /* top surface at y (walk) */
        w[0]=(uint16_t)(int16_t)(mx + rx0);
        w[1]=(uint16_t)(int16_t)(y + my);
        w[2]=(uint16_t)(int16_t)(mz + rz0);
        w[3]=255; w += 4;
    }
    (void)hh;
    for (i=0;i<6;i++) {
        EMIT_PLANE(w);
        w[0]=item_q[i][0]; w[1]=item_q[i][1]; w[2]=item_q[i][2]; w[3]=item_q[i][3];
#if defined(DOORTEX)
        w[4]=MRT_DOOR_TEX_U0;w[5]=MRT_DOOR_TEX_V0; w[6]=MRT_DOOR_TEX_U1;w[7]=MRT_DOOR_TEX_V0;
        w[8]=MRT_DOOR_TEX_U1;w[9]=MRT_DOOR_TEX_V1; w[10]=MRT_DOOR_TEX_U0;w[11]=MRT_DOOR_TEX_V1;
#else
        w[4]=DOOR_UL;w[5]=DOOR_VL; w[6]=DOOR_UH;w[7]=DOOR_VL;
        w[8]=DOOR_UH;w[9]=DOOR_VH; w[10]=DOOR_UL;w[11]=DOOR_VH;
#endif
        w += 12;
    }
}
static uint8_t g_entact[MRT_ENTCOUNT];   /* activated by a trigger      */
static uint8_t g_swpull[MRT_ENTCOUNT];   /* switch already pulled       */
static int16_t g_dooff[MRT_ENTCOUNT];    /* door swing angle, 0 = shut  */
static int16_t g_doorbase[MRT_ENTCOUNT]; /* floor Y at the door, baked  */
static int8_t  g_doorhinge[MRT_ENTCOUNT];/* -1/+1: end it is hinged on  */
static int8_t  g_doorsw[MRT_ENTCOUNT];   /* -1/+1: side it swings into  */

static int ent_is_door(int t) { return t >= ENT_DOOR_LO && t <= ENT_DOOR_HI; }
static int ent_is_switch(int t) { return t == MRT_ENT_SWITCH; }
/* BRIDGE_1/2/3 = types 68/69/70: flat wooden platforms Lara walks across a
   chasm. Their FLOOR collision is already in the sector data (she "doesn't
   fall"); they just were never DRAWN -> she walked on an invisible bridge. */
static int ent_is_bridge(int t) { return t >= 68 && t <= 70; }

/* -1 = that sector is off this room's map, 0 = open, 1 = solid */
static int ent_cell_solid(const uint8_t *sp, int sx, int sz)
{
    int xS = (sp[0]<<8)|sp[1], zS = (sp[2]<<8)|sp[3];
    const uint8_t *c;
    if (sx < 0 || sx >= xS || sz < 0 || sz >= zS) return -1;
    c = sp + 12 + (mul16(sx, zS) + sz)*6;
    return ((uint16_t)(((uint16_t)c[0]<<8)|c[1]) >= 0x7FFE) ? 1 : 0;
}

/* How OPEN the doorway is on one side, as sectors ACROSS the passage.
   ☠️ Leaving the room counts as WIDE OPEN, not as rock: rooms meet at
   portals, so the far side of a doorway is usually another room and reads as
   "off map" here.  Treating that as solid is what made the room-17 pair swing
   INTO their tunnel instead of out of it. */
static int ent_side_open(const uint8_t *sp, int sx, int sz, int dx, int dz)
{
    int nx = sx + dx, nz = sz + dz, s = ent_cell_solid(sp, nx, nz), w, k, t;
    if (s < 0) return 99;
    if (s)     return 0;
    w = 1;
    for (t = -1; t <= 1; t += 2)
        for (k = 1; k < 6; k++) {           /* perp of a cardinal (dx,dz) */
            if (ent_cell_solid(sp, nx + dz*t*k, nz + dx*t*k) != 0) break;
            w++;
        }
    return w;
}

/* ---- THE LEVER (PS1 footage, Part 2 @ 9m26s: a pale bone handle mounted on
   the wall at hip height, protruding toward Lara; it flips DOWN when pulled).
   Until now the SWITCH entity drew nothing at all, so there was no way for a
   player to see there was anything to pull.

   ☠️ PALETTE/ATLAS ARE BOTH FULL — neither slot was guessed:
   - palette: 0..239 texels, 240 pickup, 241 door, 242..253 Lara, 254/255 UI.
     The lever uses **255**, already set to white at level load; no texel
     quantizes into it, and the reference handle is near-white bone anyway.
   - atlas: the swatch band (rows g_swy..g_swy+7) holds LARA'S flat-shade
     cells 242..245 at x 0..31 — cell 2 is HERS, not free. x >= 32 in that
     band is all zero and unreferenced, so the lever takes cell 4.           */
#define SW_IDX      255                  /* CLUT white, set at level load   */
#define SW_CELLX    (4*MRT_LARA_CELL)    /* free swatch cell, past Lara's   */
#define SW_UL       (SW_CELLX + 1)
#define SW_UH       (SW_CELLX + MRT_LARA_CELL - 2)
#define SW_VL       (g_swy + 1)
#define SW_VH       (g_swy + MRT_LARA_CELL - 2)
#define LEV_WALLOFF 512                  /* back from sector centre to wall */
#define LEV_HW       33                  /* model is 66 wide                */
#define LEV_OUT0     20                  /* near face, out of the wall      */
#define LEV_OUT1     59                  /* model is 39 thick               */
#define LEV_LEN     326                  /* model is 326 long               */
#define LEV_HIP     500                  /* pivot height above the floor    */

/* The direction the lever sticks OUT of its wall, from the entity yaw (256
   units per full turn; all three LEVEL1 switches are cardinal).
   ☠️ A TR1 switch's rotation points INTO the wall - it is the direction LARA
   FACES to use it - so the handle protrudes along the NEGATED yaw vector.
   Proved from the level's own sector map before building: for e42 (yaw 64)
   and e52 (yaw 192) the SOLID neighbour is on the side the un-negated vector
   pointed away from. e10 cannot show this - its sector is a corridor with
   walls on BOTH sides, so it would have looked right either way. */
static void ent_sw_dir(int yaw, int *dx, int *dz)
{
    switch (yaw & 255) {
    case  64: *dx = -1; *dz =  0; break;
    case 128: *dx =  0; *dz =  1; break;
    case 192: *dx =  1; *dz =  0; break;
    default:  *dx =  0; *dz = -1; break;      /* yaw 0 = Lara faces +Z */
    }
}

/* one wall lever, flipped by g_swpull[e].  Same 8-vertex/6-face blob the door
   and the pickup use; only the vertex placement differs. */
static void build_ent_switch(uint8_t *buf, int atlasW, int e)
{
    int ex = mrt_ent[e].x, ez = mrt_ent[e].z;
    int offX = ex >> 8, offZ = ez >> 8;
    int rx0 = ex & 255, rz0 = ez & 255;
    int dx, dz, pivot, ytop, ybot;
    uint16_t *h = (uint16_t *)buf; uint16_t *w; int i;

    ent_sw_dir(mrt_ent[e].yaw, &dx, &dz);
    pivot = g_doorbase[e] + 720 - LEV_HIP;    /* g_doorbase is floor - 720  */
    if (g_swpull[e]) { ytop = pivot; ybot = pivot + LEV_LEN; }  /* flipped  */
    else             { ytop = pivot - LEV_LEN; ybot = pivot;  } /* upright  */

    h[0]=8; h[1]=6; h[2]=0; h[3]=(uint16_t)atlasW; h[4]=(uint16_t)g_ltx_atH;
    h[5]=(uint16_t)offX; h[6]=0; h[7]=(uint16_t)offZ;
    w = (uint16_t *)(buf + 16);
    for (i=0;i<8;i++) {
        /* item_v's sign pattern reused as (along-wall, vertical, out-of-wall) */
        int u  = (item_v[i][0] > 0 ?  LEV_HW : -LEV_HW);
        int n  = (item_v[i][2] > 0 ?  LEV_OUT1 : LEV_OUT0) - LEV_WALLOFF;
        int mx = dx*n - dz*u;                 /* perp = (-dz, dx)            */
        int mz = dz*n + dx*u;
        w[0]=(uint16_t)(int16_t)(mx + rx0);
        w[1]=(uint16_t)(int16_t)(item_v[i][1] > 0 ? ybot : ytop);
        w[2]=(uint16_t)(int16_t)(mz + rz0);
        w[3]=255;
        w += 4;
    }
    for (i=0;i<6;i++) {
        EMIT_PLANE(w);
        w[0]=item_q[i][0]; w[1]=item_q[i][1]; w[2]=item_q[i][2]; w[3]=item_q[i][3];
#if defined(DOORTEX)
        w[4]=MRT_SW_TEX_U0;w[5]=MRT_SW_TEX_V0; w[6]=MRT_SW_TEX_U1;w[7]=MRT_SW_TEX_V0;
        w[8]=MRT_SW_TEX_U1;w[9]=MRT_SW_TEX_V1; w[10]=MRT_SW_TEX_U0;w[11]=MRT_SW_TEX_V1;
#else
        w[4]=SW_UL;w[5]=SW_VL; w[6]=SW_UH;w[7]=SW_VL;
        w[8]=SW_UH;w[9]=SW_VH; w[10]=SW_UL;w[11]=SW_VH;
#endif
        w += 12;
    }
}

/* the trigger covering a (room, sector), or -1.  110 entries, scanned
   only when Lara changes sector - not per frame. */
static int ent_trig_at(int room, int sx, int sz)
{
    int i;
    for (i = 0; i < MRT_TRIGCOUNT; i++)
        if (mrt_trig[i].room == (unsigned char)room &&
            mrt_trig[i].sx   == (unsigned char)sx &&
            mrt_trig[i].sz   == (unsigned char)sz) return i;
    return -1;
}

/* run a trigger's ACTIVATE commands (see the two traps above) */
static void ent_fire(int t)
{
    int k = (mrt_trig[t].type >= 2 && mrt_trig[t].type <= 4) ? 1 : 0;
    for (; k < mrt_trig[t].ncmd; k++) {
        unsigned w = mrt_trigcmd[mrt_trig[t].cmd0 + k];
        unsigned a = (w >> 10) & 0x1F, g = w & 0x3FF;
        if      (a == 0) { if (g < MRT_ENTCOUNT) g_entact[g] = 1; }
        else if (a == 1) k++;            /* CAMERA_SWITCH parameter word */
    }
}

/* Lara's sector in her current room, from the SAME blob layout that
   room_floor_mr walks: u16 xS,zS ; s32 info_x,info_z ; cells.
   sx/sz here must match the extractor's (index/zS, index%zS). */
static int ent_lara_sector(const uint8_t **rsect, int room, int wx, int wz,
                           int *psx, int *psz)
{
    const uint8_t *sp = rsect[room];
    int xS = (sp[0]<<8)|sp[1], zS = (sp[2]<<8)|sp[3];
    int ix = (int)(((uint32_t)sp[4]<<24)|((uint32_t)sp[5]<<16)
                 | ((uint32_t)sp[6]<<8)|sp[7]);
    int iz = (int)(((uint32_t)sp[8]<<24)|((uint32_t)sp[9]<<16)
                 | ((uint32_t)sp[10]<<8)|sp[11]);
    int lx = wx - ix, lz = wz - iz;
    if (lx < 0 || lx >= xS*1024 || lz < 0 || lz >= zS*1024) return 0;
    *psx = lx >> 10; *psz = lz >> 10;
    return 1;
}

/* one tick: trigger the sector Lara stands in, then slide open doors. */
static void ent_update(const uint8_t **rsect, int room, int wx, int wz, int act)
{
    int sx, sz, e;
    if (room >= 0 && ent_lara_sector(rsect, room, wx, wz, &sx, &sz)) {
        int t = ent_trig_at(room, sx, sz);
        if (t >= 0) {
            if (mrt_trig[t].type == 2) {          /* SWITCH: needs ACTION   */
                unsigned sw = mrt_trigcmd[mrt_trig[t].cmd0] & 0x3FF;
                if (act && sw < MRT_ENTCOUNT && !g_swpull[sw]) {
                    int dx = wx - mrt_ent[sw].x, dz = wz - mrt_ent[sw].z;
                    if (dx < 0) dx = -dx;
                    if (dz < 0) dz = -dz;
                    if (dx + dz < SWITCH_REACH) {
                        g_swpull[sw] = 1;
                        ent_fire(t);
                        /* square her up to the lever and lock her into the
                           pull: the switch entity's own yaw IS the direction
                           she faces to use it (see ent_sw_dir). */
                        g_layaw = mrt_ent[sw].yaw;
                        g_swact = SWITCH_ANIM_TICKS;
                    }
                }
            } else if (mrt_trig[t].type == 0 || mrt_trig[t].type == 1) {
                ent_fire(t);                      /* ACTIVATE / PAD: step on */
            }
        }
    }
    for (e = 0; e < MRT_ENTCOUNT; e++)
        if (g_entact[e] && ent_is_door(mrt_ent[e].type)) {
            /* ON THE FIRST TICK, swing TOWARD the side Lara is standing on.
               ☠️ I first had this backwards - "away from her" seemed obvious,
               but the user's screencast of the real game (10-20-18, 16.0-17.5s)
               shows both panels ending with their FACES TOWARD THE CAMERA,
               forming a V that opens back toward her approach: the doors open
               INTO the room she is in, and she then walks between them.
               Only valid when she is actually in the door's room - a switch
               can open a door rooms away (room 20's switch opens the room-14
               pair), and then the baked open-side default stands. */
            if (g_dooff[e] == 0 && mrt_ent[e].room == (unsigned char)room) {
                int aX  = (mrt_ent[e].yaw == 0 || mrt_ent[e].yaw == 128);
                int rel = aX ? (wz - mrt_ent[e].z) : (wx - mrt_ent[e].x);
                if (rel) g_doorsw[e] = (int8_t)(rel < 0 ? -1 : 1);
            }
            if (g_dooff[e] < DOOR_ANG_OPEN) g_dooff[e] += DOOR_ANG_STEP;
        }
}

/* A SHUT DOOR IS A WALL ACROSS ITS DOORWAY, NOT A BOX FILLING ITS SECTOR.
   ☠️☠️ 2026-08-03: the first version forbade a square +-512 around the door
   entity - its ENTIRE 1024-unit sector.  TR1 stores a door and the switch
   that opens it at the SAME sector centre (checked in the level file itself,
   not inferred: e9 DOOR_4 and e10 SWITCH are both at 49664,7680,57856 in
   room 9).  So that square sealed Lara out of the very sector whose trigger
   pulls the switch, and the room-11 switch could NEVER be reached - only a
   measure-zero line on the sector boundary was both standable and within
   SWITCH_REACH.  A door entity's position is the sector CENTRE; the slab is
   what occupies the doorway.
   Collision now matches what build_ent_door draws: the full sector width
   across the doorway, thin through it, oriented by the same yaw test.
   ★ Tested as a PLANE CROSSING, not a thickness band, because at 7.5 fps one
   movement step can be longer than the slab is thick - the same trap the
   ledge GRAB hit (sweep the interval, don't widen the window). */
#define DOOR_SPAN    512         /* half the doorway, across it            */
#define DOOR_THICK   160         /* slab half-thickness 45 + Lara's radius */

static int ent_door_blocks(int room, int ox, int oz, int nx, int nz)
{
    int e;
    if (g_useset) return 0;      /* Lara's Home has no entity table        */
    for (e = 0; e < MRT_ENTCOUNT; e++) {
        int span, o, n, c, din, dout;
        if (!ent_is_door(mrt_ent[e].type)) continue;
        if (mrt_ent[e].room != (unsigned char)room) continue;
        if (g_dooff[e] >= DOOR_ANG_CLEAR) continue;     /* swung clear     */
        /* yaw 0/128 = the slab lies along X, so it is THIN in Z and it is
           Z movement it stops; otherwise the other way round. */
        if (mrt_ent[e].yaw == 0 || mrt_ent[e].yaw == 128) {
            span = nx - mrt_ent[e].x; o = oz; n = nz; c = mrt_ent[e].z;
        } else {
            span = nz - mrt_ent[e].z; o = ox; n = nx; c = mrt_ent[e].x;
        }
        /* The slab sits at the FAR end of the hallway - past the switch, on
           the side AWAY from Lara - so it never stands between her and the
           lever.  Block on that plane, not at the sector centre. */
        c -= g_doorsw[e] * DOOR_SPANH;
        if (span < 0) span = -span;
        if (span >= DOOR_SPAN) continue;                /* beside the door */
        if ((o < c) != (n < c)) return 1;               /* crossed it      */
        din  = n > c ? n - c : c - n;
        dout = o > c ? o - c : c - o;
        /* stepping INTO the slab is blocked; already inside is not, so a
           bad spawn or a door that shuts on her can still be walked out of */
        if (din < DOOR_THICK && dout >= DOOR_THICK) return 1;
    }
    return 0;
}

/* one door slab, oriented by the entity's yaw (0/128 = wide in X). */
static void build_ent_door(uint8_t *buf, int atlasW, int e)
{
    int wx = mrt_ent[e].x, wz = mrt_ent[e].z;
    int offX = wx >> 8, offZ = wz >> 8;
    int rx0 = wx & 255, rz0 = wz & 255;
    int alongX = (mrt_ent[e].yaw == 0 || mrt_ent[e].yaw == 128);
    uint16_t *h = (uint16_t *)buf; uint16_t *w; int i;
    h[0]=8; h[1]=6; h[2]=0; h[3]=(uint16_t)atlasW; h[4]=(uint16_t)g_ltx_atH;
    h[5]=(uint16_t)offX; h[6]=0; h[7]=(uint16_t)offZ;
    w = (uint16_t *)(buf + 16);
    /* swing the slab about the vertical edge it is hinged on.  The two
       panels of a pair hinge on opposite ends, so their rotation must take
       opposite SIGNS for both free edges to swing to the same side - hence
       the -hinge factor, not a shared angle. */
    { int hinge = g_doorhinge[e];
      /* g_doorsw is the side LARA is on.  The slab is parked on the FAR side
         of the doorway (past the switch, at the end of the hall) and swings
         AWAY from her, into the new room - so the angle takes the opposite
         sign to the position offset below.
         ☠️ I flipped this to "toward her" once, misreading the first
         screencast; the user's 11-16-03 capture settles it: she walks
         BETWEEN the open panels into the lit space beyond. */
      int ang   = g_doorsw[e] *  hinge * g_dooff[e];
      int cs = COS(ang), sn = SIN(ang);
      int H     = (mrt_ent[e].type == 60) ? DOOR_H_SHORT : DOOR_H_TALL;
      int floorY = g_doorbase[e] + 720;            /* baked as floor-720 */
      for (i=0;i<8;i++) {
        int a = item_v[i][0], b = item_v[i][2];
        int span = ((alongX ? a : b) > 0) ?  DOOR_SPANH : -DOOR_SPANH;
        int thin = ((alongX ? b : a) > 0) ?  DOOR_THINH : -DOOR_THINH;
        int rel  = span - hinge*DOOR_SPANH;          /* 0 at the hinge   */
        int rs   = (int)(((int32_t)rel*cs - (int32_t)thin*sn) >> 16);
        int ts   = (int)(((int32_t)rel*sn + (int32_t)thin*cs) >> 16);
        int mspan = hinge*DOOR_SPANH + rs;
        /* ★ THE SLAB IS NOT CENTRED IN ITS SECTOR.  The real models put the
           thickness wholly on ONE side of the origin plane (DOOR_1/2/3 bbox
           z[0..93], never -46..+46), so a shut door sits AT the doorway with
           only its thickness inside the room it opens into - and that is the
           face it then hinges away from.  g_doorsw is that side. */
        ts -= g_doorsw[e] * (DOOR_SPANH - DOOR_THINH);
        /* item_v[i][1] > 0 is the BOTTOM row: the slab stands ON the floor */
        w[0]=(uint16_t)(int16_t)((alongX ? mspan : ts) + rx0);
        w[1]=(uint16_t)(int16_t)(item_v[i][1] > 0 ? floorY : floorY - H);
        w[2]=(uint16_t)(int16_t)((alongX ? ts : mspan) + rz0);
        w[3]=255;
        w += 4;
      } }
    /* DOUBLE-SIDED: emit every quad in BOTH windings.  A pair's two panels
       hinge on opposite ends, and the span-axis-vs-hinge combination inverts
       one panel's winding so the kernel back-face-culls it -> that panel went
       see-through (user, 2026-08-03).  A door is a thin slab meant to be seen
       from either face anyway, so drawing both windings is correct: whichever
       faces the camera survives the cull, its twin is culled -> no double-draw. */
    h[1] = 12;
    for (i=0;i<12;i++) {
        int q = i % 6, back = i >= 6;
        int v0=item_q[q][0], v1=item_q[q][1], v2=item_q[q][2], v3=item_q[q][3];
        EMIT_PLANE(w);
        if (!back) { w[0]=v0; w[1]=v1; w[2]=v2; w[3]=v3; }
        else       { w[0]=v0; w[1]=v3; w[2]=v2; w[3]=v1; }  /* reversed */
#if defined(DOORTEX)
        /* real carved-stone door texture (objtex 897), appended to the atlas
           by MRT_DOORPATCH; replaces the flat DOOR_UL swatch cell */
        if (!back) {
            w[4]=MRT_DOOR_TEX_U0;w[5]=MRT_DOOR_TEX_V0; w[6]=MRT_DOOR_TEX_U1;w[7]=MRT_DOOR_TEX_V0;
            w[8]=MRT_DOOR_TEX_U1;w[9]=MRT_DOOR_TEX_V1; w[10]=MRT_DOOR_TEX_U0;w[11]=MRT_DOOR_TEX_V1;
        } else {
            w[4]=MRT_DOOR_TEX_U0;w[5]=MRT_DOOR_TEX_V0; w[6]=MRT_DOOR_TEX_U0;w[7]=MRT_DOOR_TEX_V1;
            w[8]=MRT_DOOR_TEX_U1;w[9]=MRT_DOOR_TEX_V1; w[10]=MRT_DOOR_TEX_U1;w[11]=MRT_DOOR_TEX_V0;
        }
#else
        if (!back) { w[4]=DOOR_UL;w[5]=DOOR_VL; w[6]=DOOR_UH;w[7]=DOOR_VL;
                     w[8]=DOOR_UH;w[9]=DOOR_VH; w[10]=DOOR_UL;w[11]=DOOR_VH; }
        else       { w[4]=DOOR_UL;w[5]=DOOR_VL; w[6]=DOOR_UL;w[7]=DOOR_VH;
                     w[8]=DOOR_UH;w[9]=DOOR_VH; w[10]=DOOR_UH;w[11]=DOOR_VL; }
#endif
        w += 12;
    }
}
#endif /* ENTITIES */
#endif

#define MAX_V 4096
#define MAX_F 2048

static int   psx[MAX_V], psy[MAX_V];
static fix   pvz[MAX_V];
static uint8_t pbehind[MAX_V];

/* Unified draw list: projected polygons (room + Lara together) so they
 * depth-sort against each other. */
typedef struct { int sx[4], sy[4]; int z; int n; uint16_t color; } DPoly;
static DPoly dp[MAX_F];
static int   dord[MAX_F];
static int   ndp;
static int   bkt_cnt[1024], bkt_pos[1024];   /* depth-bucket sort scratch */

#ifdef GEOMXFORM
/* world-space poly list for the GPU transform kernel (Tom does the
 * per-vertex transform/project/cull; 68k only builds world verts + a
 * cheap per-poly depth key). */
typedef struct { int wx[4], wy[4], wz[4]; int z; int n; uint16_t color; } WPoly;
static WPoly    wp[MAX_F];
static int      word_[MAX_F];
static int      nwp;
static int      lwx[512], lwy[512], lwz[512];   /* Lara world verts     */
static uint32_t polyx[2][MAX_F * 13];           /* packets (double-buf) */
static uint32_t camblk[2][8];
static int      gcur;                            /* active packet buffer */
#ifdef GEOMDIRECT
static uint32_t roomlist[16 * 9];               /* per-room base ptrs   */
#endif

static void push_wpoly(const int *wx, const int *wy, const int *wz,
                       int n, int z, uint16_t color)
{
    WPoly *p;
    int j;
    if (nwp >= MAX_F) return;
    p = &wp[nwp];
    for (j = 0; j < n; j++) { p->wx[j] = wx[j]; p->wy[j] = wy[j]; p->wz[j] = wz[j]; }
    p->n = n; p->z = z; p->color = color;
    word_[nwp] = nwp;
    nwp++;
}
#endif

static void push_poly(const int *sx, const int *sy, int n, int z, uint16_t color)
{
    DPoly *p;
    int j;
    if (ndp >= MAX_F)
        return;
    p = &dp[ndp];
    for (j = 0; j < n; j++) { p->sx[j] = sx[j]; p->sy[j] = sy[j]; }
    p->n = n; p->z = z; p->color = color;
    dord[ndp] = ndp;
    ndp++;
}

/* Span list drained by Tom (gpu_spanfill). Each record = 3 longs
 * precomputed for the Blitter: A1_PIXEL, B_SRCD, B_COUNT. Painter
 * draws far->near, so if the list ever fills the NEAREST spans are
 * the ones dropped - size generously. */
#define MAX_SPANS 24000
static uint32_t spanlist[MAX_SPANS * 3];

/* GEOMWALK poly packets: per poly (n<<16|color) + n*(sx<<16|sy). */
static uint32_t polylist[MAX_F * 5];
static uint32_t nspans;

static inline void emit_span(int y, int x0, int x1, uint16_t c)
{
    int n = x1 - x0 + 1;
    uint32_t *r;
    if (n <= 0 || nspans >= MAX_SPANS)
        return;
    r = &spanlist[nspans * 3];
    r[0] = ((uint32_t)(uint16_t)y << 16) | (uint32_t)(uint16_t)x0;
    r[1] = ((uint32_t)c << 16) | c;
    r[2] = (1u << 16) | (uint32_t)n;
    nspans++;
}

/* Brighten an RGB16 R5·B5·G6 colour (~2x, saturating) - the Caves are
 * dim and the source shades are low; this makes the geometry legible. */
static inline uint16_t brighten(uint16_t c)
{
    int r = (c >> 11) & 31, b = (c >> 6) & 31, g = c & 63;
    r += r; if (r > 31) r = 31;
    b += b; if (b > 31) b = 31;
    g += g; if (g > 63) g = 63;
    return (uint16_t)((r << 11) | (b << 6) | g);
}

/* Two-chain DDA convex fill: slope computed once per edge (not per
 * scanline), x stepped incrementally. Winding-agnostic - the two
 * chains' x are min/max'd, so either vertex order works. Every span
 * goes to the Blitter. */
static void fill_convex(uint16_t *fb, const int *sx, const int *sy,
                        int n, uint16_t c)
{
    int i, itop = 0, y, y0, y1, ymin, ymax;
    int aci, bci, ayend, byend, guard;
    fix ax = 0, bx = 0, aslope = 0, bslope = 0;
    (void)fb;                        /* spans are emitted to the list now */

    for (i = 1; i < n; i++)
        if (sy[i] < sy[itop]) itop = i;
    ymin = ymax = sy[itop];
    for (i = 0; i < n; i++) {
        if (sy[i] > ymax) ymax = sy[i];
    }
    y0 = ymin < 0 ? 0 : ymin;
    y1 = ymax > RENDER_H - 2 ? RENDER_H - 2 : ymax;   /* +1 line stays in-bounds */
    y0 &= ~1;                                          /* even-align: 2-line spans */

    aci = bci = itop;
    ayend = byend = sy[itop];

    /* Step 2 scanlines at a time; each span is 2 lines tall (emit_span).
     * Halves the per-scanline edge-walk, the frame's bottleneck. */
    for (y = y0; y <= y1; y += 2) {
        guard = 0;
        while (y >= ayend && guard++ <= n) {       /* chain A: dir +1 */
            int cur = aci, ni = aci + 1 == n ? 0 : aci + 1;
            int dy = sy[ni] - sy[cur];
            if (dy > 0) {
                aslope = (((fix)(sx[ni] - sx[cur])) << 16) / dy;
                ax = ((fix)sx[cur] << 16) + aslope * (y - sy[cur]);
            } else {
                ax = (fix)sx[ni] << 16; aslope = 0;
            }
            ayend = sy[ni]; aci = ni;
        }
        guard = 0;
        while (y >= byend && guard++ <= n) {       /* chain B: dir -1 */
            int cur = bci, ni = bci == 0 ? n - 1 : bci - 1;
            int dy = sy[ni] - sy[cur];
            if (dy > 0) {
                bslope = (((fix)(sx[ni] - sx[cur])) << 16) / dy;
                bx = ((fix)sx[cur] << 16) + bslope * (y - sy[cur]);
            } else {
                bx = (fix)sx[ni] << 16; bslope = 0;
            }
            byend = sy[ni]; bci = ni;
        }
        {
            int xa = ax >> 16, xb = bx >> 16;
            int xl = xa < xb ? xa : xb, xr = xa < xb ? xb : xa;
            if (xl < 0) xl = 0;
            if (xr > RENDER_W - 1) xr = RENDER_W - 1;
            if (xr >= xl) { emit_span(y, xl, xr, c); emit_span(y + 1, xl, xr, c); }
        }
        ax += aslope << 1; bx += bslope << 1;   /* y advanced by 2 */
    }
}

/* Transform Lara's baked-pose mesh model->world (rotate by her heading,
 * translate to her world position with feet on the floor) -> camera,
 * project, and push her faces into the shared depth-sorted draw list.
 * No backface cull: the global painter sort draws her far faces first,
 * near faces last, so she self-occludes correctly. */
static void draw_lara(fix camx, fix camy, fix camz, fix cY, fix sY, fix cP, fix sP)
{
    fix cY4 = cY >> 4, sY4 = sY >> 4, cP4 = cP >> 4, sP4 = sP >> 4;
    fix laC = COS(g_layaw), laS = SIN(g_layaw);
    int i;

    if (!g_lnv)
        return;

    for (i = 0; i < g_lnv; i++) {
        int mx = g_lverts[i].x, my = g_lverts[i].y, mz = g_lverts[i].z;
        int wx = g_lax + ((mx * laC + mz * laS) >> 16);   /* model->world */
        int wz = g_laz + ((-mx * laS + mz * laC) >> 16);
        int wy = g_lafloor + LARA_FEET + my;              /* feet on floor */
        int dx = wx - camx, dy = wy - camy, dz = wz - camz;
        int rx = (dx * cY4 - dz * sY4) >> 12;
        int rz = (dx * sY4 + dz * cY4) >> 12;
        int ry = (dy * cP4 - rz * sP4) >> 12;
        int rz2 = (dy * sP4 + rz * cP4) >> 12;
        if (rz2 < NEAR) {
            lbehind[i] = 1;
        } else {
            lbehind[i] = 0;
            lsx[i] = CENTER_X + rx * FOCAL / rz2;
            lsy[i] = CENTER_Y + ry * FOCAL_Y / rz2; /* +Y is DOWN in TR */
            lz[i] = rz2;
        }
    }

    for (i = 0; i < g_lnq; i++) {
        const uint16_t *q = g_lquads[i].v;
        int sx[4], sy[4], j, ar;
        if (q[0] >= g_lnv || q[1] >= g_lnv || q[2] >= g_lnv || q[3] >= g_lnv)
            continue;
        if (lbehind[q[0]] || lbehind[q[1]] || lbehind[q[2]] || lbehind[q[3]])
            continue;
        ar = (lsx[q[1]] - lsx[q[0]]) * (lsy[q[2]] - lsy[q[0]])
           - (lsx[q[2]] - lsx[q[0]]) * (lsy[q[1]] - lsy[q[0]]);
        if (ar <= 0) continue;           /* backface cull (calibrate on HW) */
        for (j = 0; j < 4; j++) { sx[j] = lsx[q[j]]; sy[j] = lsy[q[j]]; }
        push_poly(sx, sy, 4, lz[q[0]] + lz[q[1]] + lz[q[2]] + lz[q[3]],
                  g_lquads[i].color);
    }
    for (i = 0; i < g_lnt; i++) {
        const uint16_t *t = g_ltris[i].v;
        int sx[3], sy[3], j, ar;
        if (t[0] >= g_lnv || t[1] >= g_lnv || t[2] >= g_lnv)
            continue;
        if (lbehind[t[0]] || lbehind[t[1]] || lbehind[t[2]])
            continue;
        ar = (lsx[t[1]] - lsx[t[0]]) * (lsy[t[2]] - lsy[t[0]])
           - (lsx[t[2]] - lsx[t[0]]) * (lsy[t[1]] - lsy[t[0]]);
        if (ar <= 0) continue;           /* backface cull */
        for (j = 0; j < 3; j++) { sx[j] = lsx[t[j]]; sy[j] = lsy[t[j]]; }
        push_poly(sx, sy, 3, (lz[t[0]] + lz[t[1]] + lz[t[2]]) * 4 / 3,
                  g_ltris[i].color);
    }
}

/* Transform one room's verts to common camera space, cull, push its
 * faces into the shared draw list. Reuses psx/psy/pvz per room. */
static void render_room(const RoomDesc *rm, int camx, int camy, int camz,
                        fix cY4, fix sY4, fix cP4, fix sP4)
{
    int nv = rm->nv, i;
    if (nv > MAX_V) nv = MAX_V;
    for (i = 0; i < nv; i++) {
        int wx = rm->verts[i].x + rm->offX;
        int wy = rm->verts[i].y + rm->yTopD;
        int wz = rm->verts[i].z + rm->offZ;
        int dx = wx - camx, dy = wy - camy, dz = wz - camz;
        int rx = (dx * cY4 - dz * sY4) >> 12;
        int rz = (dx * sY4 + dz * cY4) >> 12;
        int ry = (dy * cP4 - rz * sP4) >> 12;
        int rz2 = (dy * sP4 + rz * cP4) >> 12;
        pvz[i] = rz2;
        if (rz2 < NEAR) {
            pbehind[i] = 1;
        } else {
            pbehind[i] = 0;
            psx[i] = CENTER_X + rx * FOCAL / rz2;
            psy[i] = CENTER_Y + ry * FOCAL_Y / rz2;
        }
    }
    for (i = 0; i < rm->nq; i++) {
        const uint16_t *q = rm->quads[i].v;
        int sx[4], sy[4], j, ar;
        if (q[0] >= nv || q[1] >= nv || q[2] >= nv || q[3] >= nv) continue;
        if (pbehind[q[0]] || pbehind[q[1]] || pbehind[q[2]] || pbehind[q[3]]) continue;
        ar = (psx[q[1]] - psx[q[0]]) * (psy[q[2]] - psy[q[0]])
           - (psx[q[2]] - psx[q[0]]) * (psy[q[1]] - psy[q[0]]);
        if (ar <= 0) continue;
        for (j = 0; j < 4; j++) { sx[j] = psx[q[j]]; sy[j] = psy[q[j]]; }
        push_poly(sx, sy, 4, pvz[q[0]] + pvz[q[1]] + pvz[q[2]] + pvz[q[3]],
                  brighten(rm->quads[i].color));
    }
    for (i = 0; i < rm->nt; i++) {
        const uint16_t *t = rm->tris[i].v;
        int sx[3], sy[3], j;
        if (t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
        if (pbehind[t[0]] || pbehind[t[1]] || pbehind[t[2]]) continue;
        for (j = 0; j < 3; j++) { sx[j] = psx[t[j]]; sy[j] = psy[t[j]]; }
        push_poly(sx, sy, 3, (pvz[t[0]] + pvz[t[1]] + pvz[t[2]]) * 4 / 3,
                  brighten(rm->tris[i].color));
    }
}

#ifdef GEOMXFORM
/* Push a room's polys in WORLD space (no transform on the 68k) with a
 * cheap per-poly depth key = the view-z of the centroid (yaw only). */
static void render_room_x(const RoomDesc *rm, int camx, int camz,
                          fix cY4, fix sY4)
{
    static int wvx[MAX_V], wvy[MAX_V], wvz[MAX_V];
    int nv = rm->nv, i;
    if (nv > MAX_V) nv = MAX_V;
    for (i = 0; i < nv; i++) {
        wvx[i] = rm->verts[i].x + rm->offX;
        wvy[i] = rm->verts[i].y + rm->yTopD;
        wvz[i] = rm->verts[i].z + rm->offZ;
    }
    for (i = 0; i < rm->nq; i++) {
        const uint16_t *q = rm->quads[i].v;
        int wx[4], wy[4], wz[4], j, cxc = 0, czc = 0, zk;
        if (q[0] >= nv || q[1] >= nv || q[2] >= nv || q[3] >= nv) continue;
        for (j = 0; j < 4; j++) {
            wx[j] = wvx[q[j]]; wy[j] = wvy[q[j]]; wz[j] = wvz[q[j]];
            cxc += wx[j]; czc += wz[j];
        }
        cxc >>= 2; czc >>= 2;
        zk = ((cxc - camx) * sY4 + (czc - camz) * cY4) >> 12;
        push_wpoly(wx, wy, wz, 4, zk, brighten(rm->quads[i].color));
    }
    for (i = 0; i < rm->nt; i++) {
        const uint16_t *t = rm->tris[i].v;
        int wx[3], wy[3], wz[3], j, cxc = 0, czc = 0, zk;
        if (t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
        for (j = 0; j < 3; j++) {
            wx[j] = wvx[t[j]]; wy[j] = wvy[t[j]]; wz[j] = wvz[t[j]];
            cxc += wx[j]; czc += wz[j];
        }
        cxc /= 3; czc /= 3;
        zk = ((cxc - camx) * sY4 + (czc - camz) * cY4) >> 12;
        push_wpoly(wx, wy, wz, 3, zk, brighten(rm->tris[i].color));
    }
}

/* Lara: 68k does model->world (rotate by heading + place on floor); Tom
 * does the camera transform along with the room. */
static void draw_lara_x(int camx, int camz, fix cY4, fix sY4)
{
    fix laC = COS(g_layaw), laS = SIN(g_layaw);
    int i;
    if (!g_lnv) return;
    for (i = 0; i < g_lnv; i++) {
        int mx = g_lverts[i].x, my = g_lverts[i].y, mz = g_lverts[i].z;
        lwx[i] = g_lax + ((mx * laC + mz * laS) >> 16);
        lwz[i] = g_laz + ((-mx * laS + mz * laC) >> 16);
        lwy[i] = g_lafloor + LARA_FEET + my;
    }
    for (i = 0; i < g_lnq; i++) {
        const uint16_t *q = g_lquads[i].v;
        int wx[4], wy[4], wz[4], j, cxc = 0, czc = 0, zk;
        if (q[0] >= g_lnv || q[1] >= g_lnv || q[2] >= g_lnv || q[3] >= g_lnv) continue;
        for (j = 0; j < 4; j++) {
            wx[j] = lwx[q[j]]; wy[j] = lwy[q[j]]; wz[j] = lwz[q[j]];
            cxc += wx[j]; czc += wz[j];
        }
        cxc >>= 2; czc >>= 2;
        zk = ((cxc - camx) * sY4 + (czc - camz) * cY4) >> 12;
        push_wpoly(wx, wy, wz, 4, zk, g_lquads[i].color);
    }
    for (i = 0; i < g_lnt; i++) {
        const uint16_t *t = g_ltris[i].v;
        int wx[3], wy[3], wz[3], j, cxc = 0, czc = 0, zk;
        if (t[0] >= g_lnv || t[1] >= g_lnv || t[2] >= g_lnv) continue;
        for (j = 0; j < 3; j++) {
            wx[j] = lwx[t[j]]; wy[j] = lwy[t[j]]; wz[j] = lwz[t[j]];
            cxc += wx[j]; czc += wz[j];
        }
        cxc /= 3; czc /= 3;
        zk = ((cxc - camx) * sY4 + (czc - camz) * cY4) >> 12;
        push_wpoly(wx, wy, wz, 3, zk, g_ltris[i].color);
    }
}
#endif /* GEOMXFORM */

#ifdef BLITPROBE
/* GPU-driven Blitter test matrix, run MID-GAME after ~15s of proven
 * chained rendering (the working environment) — see gpu_blitprobe.gas
 * for the 13-round history. Never returns. */
static void blitprobe_run(void)
{
    /* SRCSHADE/GOURD-on-8bpp micro-probe. Runs INSTEAD of the game (Tom and
     * Jerry are never started, so the 68k owns the Blitter). Silicon is the
     * only oracle here: jsim writes static B_PATD for GOURD ("computation
     * deferred") and does not model SRCSHADE at all, and the TRM says the
     * intensity path is "16-bit pixel mode only" — whether it does anything
     * usable to an 8bpp pixel-mode span is exactly the open question.
     *
     * Each test blits 16 px into a DRAM scratch row (dest pre-filled 0xEE so
     * a non-write is visible), source/dest A-gen config IDENTICAL to the
     * kernel's textured span (8bpp, XADDPIX dest, DSTA2). The 68k then draws
     * every result byte on screen as 8 bit-cells (MSB..LSB, 4px per bit),
     * which survives the video capture byte-exactly — the fps-bar run-length
     * trick is only good to +/-2 through the scaler.
     * Row 0 is a calibration pattern (FF 00 AA 55 F0 0F 80 01) so the decode
     * script can locate cell centres before reading the test rows. */
    {
        static uint8_t bp_src[512] __attribute__((aligned(16)));
        static uint8_t bp_dst[512] __attribute__((aligned(16)));
        static uint16_t bp_pal[256];
        static uint8_t bp_res[8][8];        /* [row][byte] shown on screen */
        static const uint8_t bp_cal[8] = { 0xFF,0x00,0xAA,0x55,0xF0,0x0F,0x80,0x01 };
        uint32_t aflags = BLIT_PIX8 | BLIT_WID320 | BLIT_XPIX;
        int t, i, b2;

        for (i = 0; i < 256; i++) bp_pal[i] = 0;
        bp_pal[255] = 0xFFFEu;               /* white in Jag RGB16 fb fmt */
        video_set_clut(bp_pal);

        /* PROBE v2 (2026-07-20). v1 answered the intensity question: SRCSHADE
         * and GOURD in ALL combinations write ZEROS to an 8bpp dest on real
         * silicon (control copy byte-perfect) — the intensity path is dead
         * for FB8, full stop. v2 probes the ARITHMETIC fallback for ramp-
         * palette shading: LFU-OR and ADDDSEL (dst = src + dst) in pixel and
         * phrase mode, ADDDSEL's saturation semantics on a byte pixel, and
         * CLIP_A1 per-pixel write-inhibit in phrase mode (needed to run the
         * cheap phrase-wide add pass over a pixel-exact span). Row 7 re-runs
         * GOURD with a NONZERO B_PATD poked directly (v1 scattered via B_I0,
         * so "wrote PATD(=0)" and "zeroed" were indistinguishable — this
         * tells the emulator folks which to model). */
        {
        uint32_t pflags = BLIT_PIX8 | BLIT_WID320;   /* phrase-mode (no XPIX) */
        for (i = 0; i < 8; i++) bp_res[0][i] = bp_cal[i];
        /* v8: SELF-DIAGNOSING HARNESS. Sentinel rows: a row still reading
         * 0x11 = the loop never reached that test (hang upstream); 0xBB =
         * that test's pre-wait TIMED OUT (blitter stuck busy — B_CMD never
         * re-idled); real data = the blit ran. Post-launch B_CMD poll is
         * GONE (blit_band, the per-frame in-game clear, never polls after
         * launch and provably chains on silicon) — replaced by a dumb delay
         * long enough for a 16px blit (~7us) many times over. */
        /* v9: WHERE do blit 2's reads and writes actually go? v8 proved the
         * later blits RUN (loop reached them, B_CMD idles, no post-poll) yet
         * the dest bytes read 0x00. Theory: the A-gen INTERNAL pointers are
         * not reloaded from A1_PIXEL/A2_PIXEL on silicon after a completed
         * blit, so blit 2 reads from bp_src+16 — which was zero BSS. v9
         * makes positions visible: dst prefilled ONCE with 0x80+i (0x00 can
         * no longer masquerade as anything), src tail beyond the 16 real
         * texels = 0xC3, and multiple dst windows are snapshot per blit.
         * 3 identical control copies; display:
         *   row1 = blit1 dst[0..7]   (expect ramp)
         *   row2 = blit2 dst[0..7]   row3 = blit2 dst[8..15]
         *   row4 = blit2 dst[16..23] row5 = blit2 dst[24..31]
         *   row6 = blit3 dst[16..23] row7 = blit3 dst[32..39]
         * ramp at +16/+32 = stale-pointer proven; 0xC3 anywhere = stale
         * SOURCE pointer; untouched 0x80+i = that window never written. */
        /* v13: GPU-DRIVEN matrix — see gpu_blitprobe.gas for why (twelve
         * 68k-driven rounds proved only the FIRST 68k-programmed blit ever
         * lands on silicon; the GPU kernel is the production-proven chained
         * programmer and is what the shipping shade pass will use anyway).
         * The 68k only loads the kernel, points it at the buffers, kicks,
         * and displays the 7x8 result bytes the GPU wrote to DRAM. */
        {
            extern const uint8_t gpu_probe_kernel[], gpu_probe_kernel_end[];
            static volatile uint32_t bpmail[4] __attribute__((aligned(16)));
            volatile uint32_t *gctrl = (volatile uint32_t *)0xF02114u;
            volatile uint32_t *gpc   = (volatile uint32_t *)0xF02110u;
            volatile uint32_t *gsram = (volatile uint32_t *)0xF03000u;
            volatile uint32_t *gpar  = (volatile uint32_t *)0xF03F00u;
            const uint32_t *kp = (const uint32_t *)gpu_probe_kernel;
            uint32_t kn = (uint32_t)(gpu_probe_kernel_end - gpu_probe_kernel) / 4;
            uint32_t j;
            (void)pflags;
            *gctrl = 0;
            for (j = 0; j < kn; j++) gsram[j] = kp[j];
            gpar[0] = (uint32_t)bp_src;
            gpar[1] = (uint32_t)bp_dst;
            gpar[2] = (uint32_t)&bp_res[1][0];
            gpar[3] = (uint32_t)bpmail;
            /* v15: ONE test per kernel kick — the game's own idiom (it
             * re-kicks its kernel per frame and only chains within a kick,
             * and it demonstrably works seconds before this runs). Each of
             * the 7 tests becomes a "first blit", the only kind that has
             * ever landed in 14 probe rounds. */
            { int tt;
              for (tt = 0; tt < 7; tt++) {
                bpmail[2] = (uint32_t)tt;   /* v16: index via DRAM — 68k
                                               stores to GPU SRAM between
                                               kicks don't land on silicon */
                bpmail[0] = 0;
                *gpc   = 0xF03000u;
                *gctrl = 1;
                for (j = 0; j < 400000; j++) {
                    volatile uint32_t d2;
                    for (d2 = 0; d2 < 40; d2++) ;
                    if (bpmail[0] == 0x0A3DD05Eu) break;
                }
                *gctrl = 0;
                if (bpmail[0] != 0x0A3DD05Eu)
                    bp_res[1 + tt][0] = 0xDD;     /* this test timed out */
              } }
        }
        /* draw forever: 8 rows x 8 bytes x 8 bit-cells, plus a coarse
         * value-bar under each row as a sanity cross-check */
        for (;;) {
            uint8_t *fbp = (uint8_t *)video_backbuffer();
            int yy, xx;
            for (i = 0; i < RENDER_W * RENDER_H; i++) fbp[i] = 0;
            for (t = 0; t < 8; t++) {
                int y0 = 30 + t * 22;
                for (i = 0; i < 8; i++) {
                    int x0 = 8 + i * 38;
                    uint8_t v = bp_res[t][i];
                    for (b2 = 0; b2 < 8; b2++)
                        if (v & (0x80u >> b2))
                            for (yy = 0; yy < 8; yy++)
                                for (xx = 0; xx < 4; xx++)
                                    fbp[(y0+yy)*RENDER_W + x0 + b2*4 + xx] = 255;
                }
                for (xx = 0; xx < bp_res[t][7]; xx++)     /* byte7 as bar */
                    fbp[(y0+10)*RENDER_W + 8 + xx] = 255;
            }
            video_flip();
        }
        }
    }
}
#endif

#ifdef MV_SIDE
static void lara_sidestep(int side, const uint8_t **rsect, int roomCount)
{
    /* step perpendicular to facing: yaw +/- a quarter turn.  SINTAB is 256
       entries per full turn, so a quarter turn is 64 - not 256. */
    int sy   = (g_layaw + (side < 0 ? -64 : 64)) & 255;
    int sspd = WALK_SPEED / 2;
    int sx   = g_lax + (int)(((int32_t)SIN(sy)*sspd)>>16);
    int sz   = g_laz + (int)(((int32_t)COS(sy)*sspd)>>16);
    int sf;
    if (!room_wall_at(rsect[g_curroom], sx, g_laz) &&
        room_floor_mr(rsect, roomCount, sx, g_laz, &sf) &&
#if defined(MULTIROOM) && defined(ENTITIES)
        !ent_door_blocks(g_curroom, g_lax, g_laz, sx, g_laz) &&
#endif
        g_lafloor - sf <= LARA_STEPUP) g_lax = sx;
    if (!room_wall_at(rsect[g_curroom], g_lax, sz) &&
        room_floor_mr(rsect, roomCount, g_lax, sz, &sf) &&
#if defined(MULTIROOM) && defined(ENTITIES)
        !ent_door_blocks(g_curroom, g_lax, g_laz, g_lax, sz) &&
#endif
        g_lafloor - sf <= LARA_STEPUP) g_laz = sz;
}
#endif

/* ---- MENU TEXT (2026-08-01) --------------------------------------------
   Draw a string from font_load.bin into the 8bpp framebuffer. The loading
   screen open-codes this, but it drew glyph records in FILE ORDER, which only
   worked while the blob literally was "LOADING..."; the blob now carries a
   full alphabet, so text has to be resolved BY CHARACTER. Kept OUT OF LINE
   deliberately, like the ring bake below - main() is already a ~6,000
   instruction function and growing it is the known A10 boot-hang trigger.

   DIV/DIVY match the loading screen: the glyphs are authored for a 240-line
   screen, so under LOWRES we sample every other row and let the OP scaler
   restore the aspect. rec[3] is the glyph's top relative to the baseline -
   NEVER recompute it as (16-h), which lifts descenders 8 px. */
static int menu_find_glyph(const uint8_t *fl, int n, char c)
{
    int k;
    for (k = 0; k < n; k++) if (fl[4 + k * 8] == (uint8_t)c) return k;
    return -1;
}
/* ink width of one glyph (cells are 16 wide but the ink is 8..15, left-aligned) */
static int menu_ink_w(const uint8_t *fl, const uint8_t *rec, int div)
{
    int w = rec[1], h = rec[2], gx, gy, last = 0;
    uint32_t po = ((uint32_t)rec[4] << 24) | ((uint32_t)rec[5] << 16)
                | ((uint32_t)rec[6] << 8) | rec[7];
    for (gy = 0; gy < h; gy += div)
        for (gx = 0; gx < w; gx += div)
            if (fl[po + gy * w + gx] && gx / div + 1 > last) last = gx / div + 1;
    return last ? last : 4;
}
static int menu_text_width(const char *s, int div)
{
    extern const uint8_t font_load[];
    int n = (font_load[0] << 8) | font_load[1], i, tot = 0;
    for (i = 0; s[i]; i++) {
        int g;
        if (s[i] == ' ') { tot += 6 / div + 2; continue; }
        g = menu_find_glyph(font_load, n, s[i]);
        if (g < 0) continue;
        tot += menu_ink_w(font_load, font_load + 4 + g * 8, div) + 2;
    }
    return tot > 2 ? tot - 2 : 0;
}
static void menu_text(uint8_t *fb, int W, int H, const char *s,
                      int x, int y, int div, int divy, uint8_t idx)
{
    extern const uint8_t font_load[];
    int n = (font_load[0] << 8) | font_load[1], i, gx, gy;
    for (i = 0; s[i]; i++) {
        const uint8_t *rec; int w, h, yoff, iw; uint32_t po;
        int g;
        if (s[i] == ' ') { x += 6 / div + 2; continue; }
        g = menu_find_glyph(font_load, n, s[i]);
        if (g < 0) continue;
        rec = font_load + 4 + g * 8;
        w = rec[1]; h = rec[2]; yoff = (int)rec[3] / divy;
        po = ((uint32_t)rec[4] << 24) | ((uint32_t)rec[5] << 16)
           | ((uint32_t)rec[6] << 8) | rec[7];
        iw = menu_ink_w(font_load, rec, div);
        for (gy = 0; gy < h / divy; gy++)
            for (gx = 0; gx < iw; gx++) {
                int X = x + gx, Y = y + yoff + gy;
                if (fb[0] || 1) {                    /* (keep fb used) */
                    if (font_load[po + gy * divy * w + gx * div] &&
                        X >= 0 && X < W && Y >= 0 && Y < H)
                        fb[Y * W + X] = idx;
                }
            }
        x += iw + 2;
    }
}
/* 50% dim of the whole panel, so a page reads as an OVERLAY on the title -
   which is what the original does (the logo, ring and Lara's face all stay
   visible behind the Controls page, heavily darkened). A checkerboard to the
   reserved black index is the cheapest thing that works on a paletted buffer:
   the art is NOT ramp-aligned, so there is no k to OR in. */
static void menu_dim(uint8_t *fb, int W, int H, uint8_t blackidx)
{
    /* Keep 1 pixel in 4. A 50%% checkerboard left the title art bright enough
       to fight the text - measured against the reference, the original's page
       is MUCH darker than half. */
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if ((x & 1) || (y & 1)) fb[y * W + x] = blackidx;
}

/* TITLE RING VERTEX BAKE, deliberately OUT OF LINE.  Inlined in main() this
   loop - and especially the roll term added for the opened passport - churns
   register allocation across a 6,000-instruction function, and that is the
   trigger for the silicon boot hang in A10 (see OPEN_ISSUES.md; the sidestep
   needed exactly the same treatment).  Its own frame keeps main() alone. */
/* SINTAB lookups at 1/16-unit angle precision (4096 per turn), linear
   interpolation between adjacent table entries. The ring positions items
   with these: at the front spot one whole SINTAB unit moves an item ~6
   screen pixels, so the coarse table alone makes the carousel HOP. */
static int sin16q(int a16)
{
    int a = (a16 >> 4) & 255, fr = a16 & 15;
    int s0 = SIN(a), s1 = SINTAB[(a + 1) & 255];
    return s0 + (((s1 - s0) * fr) >> 4);
}
static int cos16q(int a16) { return sin16q(a16 + 1024); }

static void title_bake(const uint8_t *sb, uint8_t *db, int nv,
                       int yaw, int pitch, int roll, int tilt,
                       int px, int py, int pz)
    __attribute__((noinline));
static void title_bake(const uint8_t *sb, uint8_t *db, int nv,
                       int yaw, int pitch, int roll, int tilt,
                       int px, int py, int pz)
{
    fix cy2 = COS(yaw), sy2 = SIN(yaw);
    fix cp2 = COS(pitch), sp2 = SIN(pitch);
    fix cr2 = COS(roll), sr2 = SIN(roll);
    fix ct2 = COS(tilt), st2 = SIN(tilt);
    int vi;
    for (vi = 0; vi < nv; vi++) {
        const uint8_t *vp = sb + 16 + vi*8;
        int x=(int16_t)((vp[0]<<8)|vp[1]);
        int y=(int16_t)((vp[2]<<8)|vp[3]);
        int z=(int16_t)((vp[4]<<8)|vp[5]);
        /* ORDER CHANGE (2026-08-04, reference spin): PITCH FIRST stands the
           lying-authored model, THEN yaw spins it about the now-vertical
           axis - so the ring's continuous spin rotates items UPRIGHT like
           the original, instead of wobbling them diagonally. */
        int ly2 = y, lz0 = z;
        uint8_t *dp;
        int rx2, ry2, rz2;
        if (pitch) { int t2 = (ly2*cp2 - lz0*sp2)>>16;
                     lz0 = (ly2*sp2 + lz0*cp2)>>16;
                     ly2 = t2; }
        int lx2 = (x*cy2 + lz0*sy2)>>16;
        int lz2 = (lz0*cy2 - x*sy2)>>16;
        /* CAMERA-FACING TILT (2026-08-05): the fitted rest spot sits ~460
           units below the camera axis; without tipping the item toward the
           camera a flat booklet spins as a foreshortened SLIVER (the
           'diagonal stick'). Applied AFTER yaw so the spin axis stays
           vertical - the PS1 does the same. */
        if (tilt) { int t2 = (ly2*ct2 - lz2*st2)>>16;
                    lz2 = (ly2*st2 + lz2*ct2)>>16;
                    ly2 = t2; }
        /* ROLL in the image plane. These PSX inventory models are authored
           lying over and the ring bake only ever applied YAW - which is why
           yaw turned the passport EDGE-ON instead of standing it up.
           SIN()/COS() are 256 ENTRIES PER TURN, so 45 deg is 32, not 128. */
        if (roll) { int t2 = (lx2*cr2 - ly2*sr2)>>16;
                    ly2 = (lx2*sr2 + ly2*cr2)>>16;
                    lx2 = t2; }
        rx2 = lx2 + px;
        rz2 = lz2 + pz;
        ry2 = ly2 + py;
        dp = db + 16 + vi*8;
        dp[0]=(uint8_t)(rx2>>8); dp[1]=(uint8_t)rx2;
        dp[2]=(uint8_t)(ry2>>8); dp[3]=(uint8_t)ry2;
        dp[4]=(uint8_t)(rz2>>8); dp[5]=(uint8_t)rz2;
    }
}

int main(void)
{
    const MRHdr *mh = (const MRHdr *)rooms_data;
    fix cx, cy, cz;
    uint8_t yaw = 0, pitch = 0;
    int fctr = 0;                    /* frame counter for gentle SD polling */
    uint32_t gdpad = 0;              /* cached remote-input bitmask */
    int gpu_ok, r;

#ifdef BOOTCRUMBS
#define CRUMB(c) (*(volatile uint16_t *)0xF00058u = (c))
    CRUMB(0x001F);                   /* BLUE: main() reached */
#else
#define CRUMB(c) ((void)0)
#endif
#ifdef EARLYCON
    /* A10 diagnostic: bring the console up BEFORE video_init so it can report
       from INSIDE it. The console normally starts after video_init, which means
       a hang in video_init prints absolutely nothing - and that is exactly what
       a black build does (zero output, while a booting build prints its whole
       boot sequence). Without this reorder the silence is uninterpretable. */
    skunk_init();
    dbg_kv("early_con", 1);
#endif
    video_init();
    CRUMB(0xFFE0);                   /* YELLOW: video up */
    skunk_init();                    /* Skunkboard USB console (NOGD only) */
    video_dump_oplist();             /* dump OP list once for OP debugging  */
#ifndef NO_GAMEDRIVE
    gd_input_init();
#endif
    gpu_ok = gpu_init();             /* Tom drains the span list if up */
    CRUMB(0x07FF);                   /* CYAN: gpu_init done */
#ifdef BOOTVID
    /* EARLY JVDEC LOAD (2026-08-07, Tom campaign): loads into GPU SRAM
       verify CLEAN here (the same window where the game-entry probes read
       J11V0D1) and FAIL from inside the video block for reasons every
       bisect has exonerated one by one (music, disp240, retries). So:
       load the video kernel ONCE, HERE, in the proven-quiet boot window,
       verify it, and let the video block only ever KICK. Nothing else
       touches GPU SRAM before the title's kernel_select (post-clips).
       gpu_init's drain has already run; the game re-selects its kernel
       at title/game transitions as always. */
    if (gpu_ok) {
        extern uint32_t gpu_jvdec_verify(void);
        int ld9;
        for (ld9 = 0; ld9 < 8; ld9++) {
            gpu_jvdec_load();
            if (gpu_jvdec_verify() == 0) break;
        }
#ifdef VIDDIAG
        g_jv_vfy = gpu_jvdec_verify();
#endif
        g_jv_early = 1;               /* video block: skip its own load */
#ifdef VIDDIAG
        { extern uint32_t gpu_sram_test(void); g_sr[0] = gpu_sram_test(); }
#endif
    }
#endif
    { extern int jerry_init(void);
      gpu_geotex_setclip(0, 319, 0, RENDER_H-1);   /* clip = full screen */
      g_jerry_ok = jerry_init();
      CRUMB(0xF81F);                 /* MAGENTA: jerry_init done */
      g_sfx_ok = g_jerry_ok;
      /* constants -> Jerry local SRAM happens later, once skv is converted */
#ifdef SKUNK_CONSOLE
      dbg_kv("jerry_alive", g_jerry_ok);
#ifdef ASSETSUM
      /* DRAM-decay diagnostic (2026-07-20 streak hunt): checksum the
         embedded assets in place and print via skunk console. Compare
         against the host-side sum of the same .bin files; re-print every
         ~64 frames from the main loop to catch decay-over-time. */
      { extern const uint8_t mrt_atlas[], mrt_geom[], mrt_sect[];
        extern const uint16_t mrt_pal[];
        uint32_t s1=0,s2=0;
        const uint8_t *p;
        for(p=mrt_atlas;p<(const uint8_t*)mrt_pal;p++) s1+=*p; /* pad sums 0 */
        for(p=mrt_geom;p<mrt_sect;p++)  s2+=*p;
        dbg_kv("atlas_sum", (int)s1); dbg_kv("geom_sum", (int)s2); }
#endif
#endif
    }


#if defined(TEXTURED) && !defined(TEXROOM)
    /* Proof-of-concept: draw a texture-mapped quad from the embedded
     * 256x256 Caves atlas subset. If the Caves rock shows, textured
     * spans work on Tom. */
    {
        extern const uint8_t  tex_atlas[];
        extern const uint16_t tex_pal[];
        static uint32_t tq[17];
        uint32_t fctr2 = 0;
        for (;;) {
            uint16_t *fb = video_backbuffer();
            uint16_t bc = (fctr2++ & 1) ? 0xFFFF : 0x0000;
            int yy, xx;
            blit_band(fb, 0, RENDER_H, 0x0004);
            tq[0]  = 4;
            tq[1]  = 40;  tq[2]  = 30;  tq[3]  = 0;   tq[4]  = 0;
            tq[5]  = 279; tq[6]  = 30;  tq[7]  = 255; tq[8]  = 0;
            tq[9]  = 279; tq[10] = 209; tq[11] = 255; tq[12] = 255;
            tq[13] = 40;  tq[14] = 209; tq[15] = 0;   tq[16] = 255;
            if (gpu_ok)
                gpu_textured(tq, 1, fb, tex_atlas, 256, tex_pal);
            for (yy = 0; yy < 12; yy++)
                for (xx = 0; xx < 12; xx++)
                    fb[yy * RENDER_W + xx] = bc;
            video_flip();
            video_wait_vblank();
        }
    }
#endif

#ifdef TEXROOM
#ifdef STAGEDIET
#error TEXROOM uses legacy room0_tex (no plane prefixes) - incompatible with STAGEDIET
#endif
    /* Render room 0 (Caves) TEXTURED: project each vertex on the 68k, emit
     * {sx,sy,u,v} per face, Tom texture-maps the spans. Slowly orbits so the
     * whole room is visible. */
    {
        extern const uint8_t  room0_tex[];
        extern const uint8_t  room0_atlas[];
        extern const uint16_t room0_pal[];
        const uint8_t *hp = room0_tex;
        int vcount = (hp[0] << 8) | hp[1];
        int qcount = (hp[2] << 8) | hp[3];
        int tcount = (hp[4] << 8) | hp[5];
        int atlasW = (hp[6] << 8) | hp[7];
        int offX = (int16_t)(((uint16_t)hp[10] << 8) | hp[11]);
        int offZ = (int16_t)(((uint16_t)hp[14] << 8) | hp[15]);
        const uint8_t *vp = room0_tex + 16;
        const uint8_t *qp = vp + vcount * 8;
        const uint8_t *tp = qp + qcount * 24;
        static int   wx[512], wy[512], wz[512];
        static int   psx[512], psy[512];
        static uint8_t pbh[512];
        static uint32_t tpk[2][512 * 17];
        int curbuf = 0, primed = 0;
        int minx = 1 << 30, maxx = -(1 << 30), miny = 1 << 30, maxy = -(1 << 30);
        int minz = 1 << 30, maxz = -(1 << 30), i;
        int ccx, ccy, ccz, camx, camy, camz;
        uint8_t yaw = 0;
        uint32_t fc = 0;
        for (i = 0; i < vcount; i++) {
            const uint8_t *v = vp + i * 8;
            int x = (int16_t)(((uint16_t)v[0] << 8) | v[1]);
            int y = (int16_t)(((uint16_t)v[2] << 8) | v[3]);
            int z = (int16_t)(((uint16_t)v[4] << 8) | v[5]);
            wx[i] = x + (offX << 8); wy[i] = y; wz[i] = z + (offZ << 8);
            if (wx[i] < minx) minx = wx[i]; if (wx[i] > maxx) maxx = wx[i];
            if (wy[i] < miny) miny = wy[i]; if (wy[i] > maxy) maxy = wy[i];
            if (wz[i] < minz) minz = wz[i]; if (wz[i] > maxz) maxz = wz[i];
        }
        ccx = (minx + maxx) / 2; ccy = (miny + maxy) / 2; ccz = (minz + maxz) / 2;
        camx = ccx; camy = ccy - 300; camz = ccz;
#ifdef FB8
        video_set_clut(room0_pal);
        { volatile uint16_t *clut = (volatile uint16_t *)0xF00400u;
          clut[254] = 0x0000; clut[255] = 0xFFFF; }   /* blink reserved */
#endif
        for (;;) {
            fix cY = COS(yaw), sY = SIN(yaw), cP = COS(CAM_PITCH), sP = SIN(CAM_PITCH);
            fix cY4 = cY >> 4, sY4 = sY >> 4, cP4 = cP >> 4, sP4 = sP >> 4;
            uint32_t np = 0, nf = 0;
            uint32_t *pk = tpk[curbuf];
            fbpix *fb;
            int cx2 = ccx - (int)(((int32_t)sY * 4000) >> 16);
            int cz2 = ccz - (int)(((int32_t)cY * 4000) >> 16);
            camx = cx2; camz = cz2;
            for (i = 0; i < vcount; i++) {
                int dx = wx[i] - camx, dy = wy[i] - camy, dz = wz[i] - camz;
                int rx = (dx * cY4 - dz * sY4) >> 12;
                int rz = (dx * sY4 + dz * cY4) >> 12;
                int ry = (dy * cP4 - rz * sP4) >> 12;
                int rz2 = (dy * sP4 + rz * cP4) >> 12;
                if (rz2 < NEAR) { pbh[i] = 1; }
                else { pbh[i] = 0; psx[i] = CENTER_X + rx * FOCAL / rz2; psy[i] = CENTER_Y + ry * FOCAL_Y / rz2; }
            }
            for (i = 0; i < qcount; i++) {
                const uint8_t *q = qp + i * 24;
                int a = (q[0]<<8)|q[1], b = (q[2]<<8)|q[3], c = (q[4]<<8)|q[5], d = (q[6]<<8)|q[7];
                int ar, j;
                if (pbh[a] || pbh[b] || pbh[c] || pbh[d]) continue;
                ar = (psx[b]-psx[a])*(psy[c]-psy[a]) - (psx[c]-psx[a])*(psy[b]-psy[a]);
                if (ar <= 0) continue;
                pk[np++] = 4;
                {   int vi[4]; vi[0]=a; vi[1]=b; vi[2]=c; vi[3]=d;
                    for (j = 0; j < 4; j++) {
                        pk[np++] = (uint32_t)psx[vi[j]];
                        pk[np++] = (uint32_t)psy[vi[j]];
                        pk[np++] = (q[8 + j*4]<<8)|q[9 + j*4];      /* u */
                        pk[np++] = (q[10 + j*4]<<8)|q[11 + j*4];    /* v */
                    }
                }
                nf++;
            }
            for (i = 0; i < tcount; i++) {
                const uint8_t *t = tp + i * 18;
                int a = (t[0]<<8)|t[1], b = (t[2]<<8)|t[3], c = (t[4]<<8)|t[5];
                int ar, j, vi[3];
                if (pbh[a] || pbh[b] || pbh[c]) continue;
                ar = (psx[b]-psx[a])*(psy[c]-psy[a]) - (psx[c]-psx[a])*(psy[b]-psy[a]);
                if (ar <= 0) continue;
                pk[np++] = 3; vi[0]=a; vi[1]=b; vi[2]=c;
                for (j = 0; j < 3; j++) {
                    pk[np++] = (uint32_t)psx[vi[j]];
                    pk[np++] = (uint32_t)psy[vi[j]];
                    pk[np++] = (t[6 + j*4]<<8)|t[7 + j*4];
                    pk[np++] = (t[8 + j*4]<<8)|t[9 + j*4];
                }
                nf++;
            }
#ifdef OVERLAP
            /* async: sync + present the PREVIOUS frame, then fire this one
             * and continue building the next while the Blitter draws it. */
            if (primed) {
                fbpix *done; int yy, xx;
                fbpix bc; bc = (fc++ & 1) ? BLINK_ON : BLINK_OFF; /* jcc68k: no side effects in initializers */
                gpu_sync();
                done = video_backbuffer();
                for (yy = 0; yy < 10; yy++) for (xx = 0; xx < 10; xx++)
                    done[yy * RENDER_W + xx] = bc;
#ifdef CLUTGUARD
                /* CLUT diagnostic: rewrite the CLUT every frame (heals any
                   runtime clobber) and draw an index ramp strip on rows
                   190-198 (the CLUT's actual contents, visible on screen:
                   healthy = ordered dark->bright ramp bands). */
                video_set_clut(room0_pal);
                { volatile uint16_t *clutg = (volatile uint16_t *)0xF00400u;
                  clutg[254]=0x0000; clutg[255]=0xFFFF; }
                for (yy = 190; yy < 199; yy++)
                    for (xx = 0; xx < RENDER_W; xx++)
                        done[yy*RENDER_W+xx] = (fbpix)((xx*240)/RENDER_W);
#endif
                video_flip();
                video_wait_vblank();   /* pace the flip (LOWRES scaled obj needs it) */
            }
            fb = video_backbuffer();
            blit_band(fb, 0, RENDER_H, CLEAR_IDX);
            if (gpu_ok && nf)
                gpu_textured_kick(pk, nf, fb, room0_atlas, (uint32_t)atlasW, room0_pal);
            primed = 1; curbuf ^= 1;
#else
            fb = video_backbuffer();
            blit_band(fb, 0, RENDER_H, CLEAR_IDX);
            if (gpu_ok && nf)
                gpu_textured(pk, nf, fb, room0_atlas, (uint32_t)atlasW, room0_pal);
            {   fbpix bc; bc = (fc++ & 1) ? BLINK_ON : BLINK_OFF; /* jcc68k: no side effects in initializers */ int yy, xx;
                for (yy = 0; yy < 10; yy++) for (xx = 0; xx < 10; xx++)
                    fb[yy * RENDER_W + xx] = bc; }
            video_flip();
            video_wait_vblank();
#endif
            yaw += 1;
        }
    }
#endif

#ifdef GEOTEX
    /* Tom reads room-0 textured geometry from DRAM, transforms/projects/
     * culls per face, AND Blitter-textures - the 68k only orbits the camera
     * and kicks (zero per-vertex work). */
    {
        extern const uint8_t  room0_tex[];
        extern const uint8_t  room0_atlas[];
        extern const uint16_t room0_pal[];
        const uint8_t *hp = room0_tex;
        int vcount = (hp[0] << 8) | hp[1];
        int atlasW = (hp[6] << 8) | hp[7];
        int offX = (int16_t)(((uint16_t)hp[10] << 8) | hp[11]);
        int offZ = (int16_t)(((uint16_t)hp[14] << 8) | hp[15]);
        const uint8_t *vp = room0_tex + 16;
        static uint32_t camblk[8];
        static uint8_t lara_blob[LARA_BLOB_SZ] __attribute__((aligned(8)));
        int minx = 1<<30, maxx = -(1<<30), miny = 1<<30, maxy = -(1<<30);
        int minz = 1<<30, maxz = -(1<<30), i, ccx, ccy, ccz;
        uint32_t fc = 0;
        for (i = 0; i < vcount; i++) {
            const uint8_t *v = vp + i * 8;
            int x = (int16_t)(((uint16_t)v[0]<<8)|v[1]);
            int y = (int16_t)(((uint16_t)v[2]<<8)|v[3]);
            int z = (int16_t)(((uint16_t)v[4]<<8)|v[5]);
            int wx = x + (offX<<8), wy = y, wz = z + (offZ<<8);
            if (wx<minx) minx=wx; if (wx>maxx) maxx=wx;
            if (wy<miny) miny=wy; if (wy>maxy) maxy=wy;
            if (wz<minz) minz=wz; if (wz>maxz) maxz=wz;
        }
        ccx = (minx+maxx)/2; ccy = (miny+maxy)/2; ccz = (minz+maxz)/2;

        /* init Lara mesh (laramesh.bin, baked pose) */
        { const LHdr *lh = (const LHdr *)lara_data;
          if (lh->magic == 0x4C41) {
              g_lnv = lh->vcount; g_lnq = lh->qcount; g_lnt = lh->tcount;
              if (g_lnv > 512) g_lnv = 512;
              g_lnframes = (lara_data[8] << 8) | lara_data[9];
              if (g_lnframes < 1) g_lnframes = 1;
              g_lframe0 = lara_data + 12;
              g_lverts = (const LVert *)g_lframe0;
              g_lquads = (const RQuad *)(g_lframe0 + g_lnframes*lh->vcount*6);
              g_ltris  = (const RTri  *)((const uint8_t *)g_lquads + lh->qcount*12);
            g_lshades = (const uint8_t *)(g_ltris + g_lnt);
          } }
        /* Lara starts at room centre, on the floor, facing +Z */
        g_lax = ccx; g_laz = ccz; g_layaw = 0;
        { int fy; g_lafloor = room0_floor(g_lax, g_laz, &fy) ? fy : ccy; }

        video_set_clut(room0_pal);
        { volatile uint16_t *clut = (volatile uint16_t *)0xF00400u;
          clut[254] = 0x0000; clut[255] = 0xFFFF; }

#ifdef SKUNK_CONSOLE
        dbg_kv("bc_init_done", 1);
#endif
        for (;;) {
            uint32_t pad = joypad_read();  /* REAL controller only: gd_input_poll
                was an SD fopen+fread+fclose EVERY FRAME (2 failed dir searches
                when INPUT.BIN absent) — a huge hidden 68k cost in play builds */
            fix cY, sY, cP = COS(CAM_PITCH), sP = SIN(CAM_PITCH);
            int camx, camy, camz;
            fbpix *fb;

            /* Lara tank controls + floor-follow / per-axis wall collision */
            { int mv = 0, fy;
#ifdef TIMESTEP
              /* TURNING (retuned 2026-08-02 — user: "still too fast", twice).
                 Rate is now in EIGHTHS of a SINTAB unit per 30 Hz tick, so it
                 can be tuned finely; the old whole-unit scale could only step
                 126 -> 84 deg/s with nothing usable in between.
                   step = tr8 * ticks / 8,  ticks = g_tturn >> 1
                 TR1's own run turn is 4.27 u/tick (180 deg/s) and the previous
                 value was 3 (126 deg/s). At 7.5 fps a whole frame of turn lands
                 as one visible jump, so authentic-but-chunky reads worse than
                 slower-and-readable; these are deliberately below TR1.
                 Also EASE IN over TURN_RAMP ticks so the first frame of a turn
                 is not a snap — that is most of what "smoother" means here. */
              { int tr8 = (pad & PAD_C) ? TURN_WALK_TR1 : TURN_RUN_TR1;
                int turning = (pad & (PAD_LEFT|PAD_RIGHT)) ? 1 : 0;
                int ramp = (g_turnf < TURN_RAMP) ? (g_turnf + 1) : TURN_RAMP;
                int step = (tr8 * (g_tturn >> 1) * ramp) / (8 * TURN_RAMP);
                if (turning && step < 1) step = 1;   /* never stall completely */
                if (pad & PAD_LEFT)  g_layaw -= step;
                if (pad & PAD_RIGHT) g_layaw += step;
                /* count TICKS spent turning in place, for the FAST_TURN
                   escalation AND the ease-in above; any other input resets it */
                if (turning && !(pad & (PAD_UP|PAD_DOWN)))
                    g_turnf += g_ticks >> 1;
                else if (!turning) g_turnf = 0; }
#else
              if (pad & PAD_LEFT)  g_layaw -= 3;
              if (pad & PAD_RIGHT) g_layaw += 3;
#endif
              if (pad & PAD_UP)   mv = 1;
              if (pad & PAD_DOWN) mv = -1;
#if defined(MULTIROOM) && defined(ENTITIES) && defined(SWANIM)
              /* locked in the switch pull: she cannot walk out of it */
              if (g_swact > 0) { mv = 0; pad &= ~(PAD_LEFT|PAD_RIGHT); }
#endif
              if (mv) {
                  int nx = g_lax + (int)(((int32_t)SIN(g_layaw) * (WALK_SPEED*mv)) >> 16);
                  int nz = g_laz + (int)(((int32_t)COS(g_layaw) * (WALK_SPEED*mv)) >> 16);
                  if (room0_floor(nx, g_laz, 0)) g_lax = nx;
                  if (room0_floor(g_lax, nz, 0)) g_laz = nz;
              }
              if (room0_floor(g_lax, g_laz, &fy)) g_lafloor = fy;
              /* run-cycle: advance frames while moving, hold frame 0 when idle */
              if (mv) { g_lanim += LARA_ANIM_STEP;
                        while (g_lanim >= g_lnframes) g_lanim -= g_lnframes; }
              else g_lanim = 0;
              g_lverts = (const LVert *)(g_lframe0 + g_lanim * g_lnv * 6);
            }
            /* 3rd-person camera behind Lara, looking her way */
            cY = COS(g_layaw); sY = SIN(g_layaw);
            camx = g_lax - (int)(((int32_t)sY * CAMDIST) >> 16);
            camz = g_laz - (int)(((int32_t)cY * CAMDIST) >> 16);
            camy = g_lafloor - CAMHEIGHT;
            camblk[0]=(uint32_t)(cY>>4); camblk[1]=(uint32_t)(sY>>4);
            camblk[2]=(uint32_t)(cP>>4); camblk[3]=(uint32_t)(sP>>4);
            camblk[4]=(uint32_t)camx; camblk[5]=(uint32_t)camy;
            camblk[6]=(uint32_t)camz; camblk[7]=0;

            build_lara_blob(lara_blob, atlasW);

            fb = video_backbuffer();
            blit_band(fb, 0, RENDER_H, CLEAR_IDX);
            if (gpu_ok) {
                gpu_geotex(room0_tex, fb, camblk, room0_atlas, (uint32_t)atlasW);
                gpu_geotex(lara_blob, fb, camblk, room0_atlas, (uint32_t)atlasW);
            }
            {   fbpix bc; bc = (fc++ & 1) ? BLINK_ON : BLINK_OFF; /* jcc68k: no side effects in initializers */ int yy, xx;
                for (yy = 0; yy < 10; yy++) for (xx = 0; xx < 10; xx++)
                    fb[yy * RENDER_W + xx] = bc; }
            video_flip();
            video_wait_vblank();
        }
    }
#endif

#ifdef MULTIROOM
    /* Several portal-connected textured rooms sharing one atlas; Lara walks
     * between them (multi-room collision). Each room + Lara is one gpu_geotex
     * kick; rooms drawn far-first (painter). 68k does zero per-vertex work. */
    {
        extern const uint8_t  mrt_index[], mrt_geom[], mrt_sect[];
        extern const uint8_t  mrt_atlas[];
        extern const uint16_t mrt_pal[];
        extern const uint8_t  mrt_lara_[] __asm__("mrt_lara");
        extern const uint8_t  mrt_lskin_[] __asm__("mrt_lskin");
        extern const uint8_t  gym_index[], gym_geom[], gym_sect[];
        extern const uint8_t  gym_atlas[];
        extern const uint16_t gym_pal[];
        extern const uint8_t  gym_lara[], gym_lskin[];
        static uint32_t camblk[8];
        static uint8_t lara_blob[LARA_BLOB_SZ] __attribute__((aligned(8)));
        static uint8_t item_blob[640] __attribute__((aligned(8)));
        static uint8_t door_blob[640] __attribute__((aligned(8)));
        const uint8_t *rgeom[64]; const uint8_t *rsect[64];
        int rcx[64], rcz[64], order[64], rrad[64], rdepth[64];
        int prv[64], prx0[64], prx1[64], pry0[64], pry1[64]; /* room windows */
        static uint32_t displist[1+40*4];   /* dispatch: {room,cx,cy,cache};
                                               39 rooms + Lara; >8 entries
                                               render in sequential batches
                                               (NOVISCULL can request all) */
        static uint32_t jxlist[1+8*5];      /* Jerry room-transform list */
#ifdef JERRYX
        static uint32_t jcache[8][2244] __attribute__((aligned(8)));
#else
        /* Jerry room co-transform RETIRED (see 5717): jcache is never filled
           when JERRYX is off. Shrink it to a stub - reclaims 71.8KB of BSS
           for the 240-tall title framebuffers (task #4) + stack headroom. */
        static uint32_t jcache[1][4] __attribute__((aligned(8)));
#endif
        static const uint8_t *jxroom[8];    /* blob base per slot (for sort) */
        int njx;
        int roomCount, atlasW;
        /* ---- TR1 TITLE + SELECTION (real PSX art) ----
           Page 0 = title art (New Game), page 1 = mansion art (Lara's Home).
           LEFT/RIGHT flips pages passport-style; any fire button selects.
           (Lara's Home boots the caves until GYM.PSX is extracted.) */
        { extern const uint8_t  title_img[],  gymload_img[];
          extern const uint16_t title_pal[],  gymload_pal[];
          extern const uint8_t  pass_geom[], pass_atlas[];
          /* INV_PASSPORT (model 71), the OPENED spread. Model 81 (closed) is
             the ring item; selecting it opens THIS one, as in the original. */
          extern const uint8_t  pass2_geom[], pass2_atlas[];
          /* INV_CONTROLS (model 97) - the "Controls" ring item. */
          extern const uint8_t  ctrl_geom[], ctrl_atlas[];
          extern const uint8_t  photo_geom[], photo_atlas[];
          /* INV_SOUND (model 96) - the Sound ring item. */
          extern const uint8_t  sound_geom[], sound_atlas[];
          /* type 95 - "Screen Adjust" on the PSX title ring (the sunglasses). */
          extern const uint8_t  detail_geom[], detail_atlas[];
          static uint32_t tcam[8] __attribute__((aligned(16)));
          /* AUTHENTIC RING (OpenLara inventory.h:1706): items on a circle,
             selected at front, other across the ring (farther + higher);
             every item faces with the PI flip the PSX models are authored
             for. 68k rotates the tiny vert sets into scratch blobs. */
#ifdef STAGEDIET
          /* 3456: the Controls model (52 verts / 100 tris) stages to
             16 + 52*8 + 100*TREC = 3432 bytes. At the old 2304 it tripped the
             clamp below and rendered with ZERO faces - an invisible item, not
             an error. */
          /* 6144: sized to the LARGEST staged item (detail sunglasses,
             5792B expanded; the prism pad is only 2208B now). 10560 was the
             old scan-pad allowance and blew the 2MB budget once the prism's
             32KB atlas joined the image (__bss_end guard, 2026-08-04). */
          /* 7424: staged size is 16+(nv+nt)*8+(nq+nt)*36 since the tri
             promotion appends midpoint verts and promotes tris to quads -
             the max pad (180v/16q/112t) stages at 6960B and the 6144 slot's
             overflow clamp made it an INVISIBLE ITEM (the exact failure the
             old 2304-byte comment warned about). */
          static uint8_t rblob[6][8448] __attribute__((aligned(8)));
#else
          static uint8_t rblob[6][4096] __attribute__((aligned(8)));
#endif
          const uint8_t *rsrc[6]; const uint8_t *ratl[6];
          int rvcnt[6], rblen[6];
          int rnv0[6], rnq0[6], rnt0[6];  /* per-item counts for the
                                             post-bake midpoint fixup */
          /* PASSPORT OPEN: 0 = on the ring, 1..PASS_OPEN_TICKS = opening,
             PASS_OPEN_TICKS = fully open (the page view). */
          int popen = 0, prow = 0;   /* open-passport row: 0 Start, 1 Back */
          /* PAGE-FLIP (PSX ref 2026-08-06): the open book's LEAF (verts
             8..11 of the composed pass2 blob, see gen_openbook.py) turns
             about the spine when the row changes - panim eases toward
             prow*128. The source blob lives in RAM so the 68k can rewrite
             the leaf's model verts each frame before the bake. */
          int panim = 0;
          int introplay = 0;
          static uint8_t p2src[240] __attribute__((aligned(8)));
#ifdef STEADYREAD
          { static volatile int g_sr_init; g_sr_init = (SRGUARD); }
#endif
          /* CONTROLS PAGE state, mirroring popen: 0 = on the ring, 1 = open.
             CTRLOPEN=1 boots straight into it so the layout can be checked
             offline in jagemu without driving the menu. */
          int copen = 0;
          /* SOUND PAGE state, mirroring copen. */
          int sopen = 0, srow = 0;
          int mkick = 0;      /* unmute: re-prime the music voice (the queue
                                 alone cannot restart a fully-idle voice) */
          /* ring angle in 1/16 SINTAB units (4096 per turn). Rotation is a
             fixed-length SMOOTHSTEP slew from ringS over ringD (2026-08-06,
             user: "make the ring movement smoother") - the old quarter-gap
             exponential ease lurched on the first frame then crawled. */
#ifdef TITLESEL
          int ringA = (((TITLESEL) * 4096) / 5);
#else
          int ringA = 0;
#endif
          int ringS = ringA, ringD = 0, ringK = 3;
#define RING_SLEW_T 3                /* frames per one-item rotation; slew
                                        frames draw the WHOLE carousel (the
                                        PSX turns everything together, user
                                        capture 10-42-06) so each costs a
                                        full repaint - 3 steps reads as one
                                        continuous turn */
          /* DIRTY-RECT ring renderer (2026-08-06): the ring page was ~3fps
             because all 449 staged faces redrew every frame on a per-face-
             bound Tom. Only the MOVING items (front spinner; the swapping
             pair during a slew) are re-rendered now - static items' pixels
             persist in each of the two framebuffers, and the art behind a
             mover is restored with a Blitter sub-rect copy before redraw.
             fullpaint counts frames that must paint everything (both
             buffers at entry, after a slew lands, after a panel closes). */
#ifdef TITLESEL
          int movers[4] = { (TITLESEL), 0, 0, 0 }; int nmov = 1;
#else
          int movers[4] = { 0, 0, 0, 0 }; int nmov = 1;
#endif
          int fullpaint = 3, panelPrev = 0;
          int ringfbi = 0, ringfast = 0;
          void *fbp3[3] = { 0, 0, 0 };  /* video is TRIPLE buffered (fb0-2) */
          int mrx0[3] = {1,1,1}, mry0[3] = {1,1,1};
          int mrx1[3] = {0,0,0}, mry1[3] = {0,0,0}; /* x0>x1 = nothing to restore */
#define RING_LBL_Y0 196              /* ring label band, restored every frame */
#define RING_LBL_H  28
          int spin = 0;
#ifdef TITLESEL
          int page = (TITLESEL), shown = -1, armed = 0;
#else
          int page = 0, shown = -1, armed = 0;
#endif
          uint32_t praw = 0, stable = 0, sprev = 0;
#ifndef NO_GAMEDRIVE
          /* TITLE MUSIC: stream MUSIC.PCM (raw s8 @11025) from the SD
             card through a double buffer on voice 0 (footsteps own it
             in-game — no conflict). No seek in the GD BIOS: hold the
             handle, sequential reads, close+reopen to loop the theme. */
          static int8_t mbuf[2][12288] __attribute__((aligned(4)));
          /* 4KB = 0.37s per swap; halved from 8KB to protect the 68k STACK:
             sp starts at 0x200000 and grows DOWN into the top of BSS — the
             8KB buffers left only 208 BYTES of headroom (cold-boot crash,
             2026-07-12). Makefile now guards bss_end < 0x1FC000. */
          /* (was a static char* array with string-literal initializers —
             address constants are outside jcc68k's supported subset) */
          int mh = -1, mplay = 0;
          int msz = 0, mleft = 0;      /* gd_fread returns 0 on SUCCESS —
                                          track position via gd_fsize */
          int mlq[2]; mlq[0] = mlq[1] = 0;
#endif
#ifdef GYMTEST
          g_useset = 1; goto menu_done;   /* test: boot straight into the mansion */
#endif
#ifdef CAVETEST
          g_useset = 0; goto menu_done;   /* test: boot straight into the caves */
#endif
#if !defined(NO_GAMEDRIVE) && defined(GDPROBE)
          /* rig-only: measure gd_fread cost vs size ON SILICON. Reads
             INTRO.JV sequentially, 8 reps per size, draws field-count
             bars (2px per field) the capture rig reads off. Freezes. */
          { static uint8_t pb[24576] __attribute__((aligned(4)));
            extern volatile uint32_t frame_count;
            static const int SZ[5] = { 512, 2048, 6144, 12288, 24576 };
            int rez[5], si, rp, ph = -1, mi9;
            for (mi9 = 0; mi9 < 2 && ph < 0; mi9++)
                ph = gd_fopen(mi9 ? "/INTRO.JV" : "INTRO.JV",
                              GD_FOPEN_READ | GD_FOPEN_OPEN_EXISTING);
            if (ph >= 0) {
                (void)gd_fsize((unsigned)ph);
                for (si = 0; si < 5; si++) {
                    uint32_t p0 = frame_count;
                    for (rp = 0; rp < 8; rp++)
                        if (gd_fread((unsigned)ph, pb, (unsigned)SZ[si],
                                     GD_FREAD_CPU) != 0) break;
                    rez[si] = (int)(frame_count - p0);
                }
                gd_fclose((unsigned)ph);
                { fbpix *pf9 = (fbpix *)video_backbuffer();
                  volatile uint16_t *cl9 = (volatile uint16_t *)0xF00400u;
                  uint8_t *f9 = (uint8_t *)pf9; int x9, y9;
                  cl9[254] = 0x0000; cl9[255] = 0xFFFE;
                  for (y9 = 0; y9 < 240; y9++)
                      for (x9 = 0; x9 < 320; x9++) f9[y9*320+x9] = 254;
                  for (si = 0; si < 5; si++) {
                      int h9 = rez[si] * 2; if (h9 > 200) h9 = 200;
                      for (y9 = 0; y9 < h9; y9++)
                          for (x9 = 0; x9 < 30; x9++)
                              f9[(230 - y9)*320 + 20 + si*55 + x9] = 255;
                      /* exact value as a dot column: one 4px dot per field */
                      for (y9 = 0; y9 < rez[si] && y9 < 50; y9++)
                          for (x9 = 0; x9 < 4; x9++)
                              f9[(230 - y9*4)*320 + 20 + si*55 + 36 + x9] = 255;
                  }
                  video_flip(); }
                for (;;) ;         /* park: capture at leisure */
            } }
#endif
#if !defined(NO_GAMEDRIVE) && defined(BOOTVID)
          /* INTRO ON START GAME (2026-08-06): the PSX plays the intro
             cinematic on New Game, not at boot - the boot chain is
             EIDOS+CORE only now, and selecting Start Game jumps BACK
             here with introplay=1 to stream INTRO.JV, then falls
             through to the game (bv_done). */
          introplay = 0;
bootvid_entry:
          /* BOOT LOGOS (2026-08-05, user: "have this video load after the
             game is booted"): stream EIDOS.JV then CORE.JV (PS1 order) from
             the SD card and play them before the title. Frames are native
             320x240 8bpp PackBits RLE (tools/tr2jag_video.py); the payload
             buffer is rblob - 50KB free until the ring items stage below.
             Pacing rides the 60Hz VI clock, so a slow SD only stretches the
             clip. Any fire button skips the current clip; missing files
             (BigPEmu has no GameDrive) skip straight to the title. */
          /* Frames are 160x120, pixel-doubled to 320x240 by the decoder.
             The BIOS FREAD has a big per-call cost (the 320x240 first cut
             played ~6x slow at 2 calls/frame), so the stream is pulled in
             ~24KB chunks into a rolling buffer and frames are parsed out
             of it - one BIOS call per several frames. */
          { extern volatile uint32_t frame_count;
            uint8_t *vb  = (uint8_t *)rblob;         /* stream buffer      */
            uint8_t *cbk = (uint8_t *)rblob + 31488; /* 4KB VQ codebook    */
            /* TWO token stashes, ping-ponged per frame: the kernel re-applies
               the PREVIOUS frame's tokens to the stale back buffer, so the
               previous stash must survive the current copy (delta streams;
               kf-only passes prevlen 0 and never reads it). 7552B each is
               ~1.5x the measured worst frame; the encoder asserts the cap. */
            uint8_t *ptkA = (uint8_t *)rblob + 35584;
            uint8_t *ptkB = (uint8_t *)rblob + 43136;
            /* SHADOW frame: the ONE place delta semantics live. Painted by
               the 68k from the token stream, then block-copied whole into
               the display buffer every frame. */
            static uint8_t vshadow[320*240] __attribute__((aligned(16)));
            extern volatile uint32_t video_two_buf;
            int vc;
            /* JV04 (user: "the original videos from the disc"): NATIVE
               320x240 vector quantization - 4x4 blocks against a per-clip
               256-entry codebook, temporal skip tokens. Plays in the boot
               disp240 mode as-is. Tom copies codebook blocks into BOTH
               framebuffers (ping-pong coherent); the 68k streams sectors,
               queues audio chunks on the DSP, paces and flips. */
            if (gpu_ok) {
                /* Tom can be mid-frame when Start Game re-enters here -
                   gpu_jvdec_load stops the GPU (G_CTRL=0) and loading
                   over a live kernel wedges the kick protocol. Boot-time
                   this sync is a no-op. */
                extern int gpu_sync(void); gpu_sync();
                /* THE LOAD FIX (2026-08-07, context probe J11V0): the 68k's
                   copy into GPU SRAM fails while Jerry's I2S/queue engine
                   is streaming (bus-master collision) - the SAME load
                   verifies clean in the music-stopped window at game entry.
                   QUIESCE THE DSP VOICE for the ~50us of loading (the clip
                   replaces the music's audio anyway), then verify-retry
                   until the bytes hold. */
#ifdef VIDDIAG
                { extern uint32_t gpu_sram_test(void);
                  g_sr[introplay ? 2 : 1] = gpu_sram_test(); }
#endif
                /* ☠️ LOAD AND VERIFY HERE, EVERY TIME. The g_jv_early
                   experiment loaded the kernel once at boot and the block
                   then only ever KICKED - so anything that disturbed GPU
                   SRAM in between left Tom running garbage, the wait timed
                   out, and every frame fell back to the 68k token walk.
                   That is the "chugging, sounds like it's running off the
                   68k" the user heard (2026-08-08). vidrom loads+verifies
                   immediately before the clip and GATES the kick on it;
                   this now does the same. The load is ~50us. */
                {
                  volatile uint32_t *v0 = (volatile uint32_t *)0xF1C340u;
                  volatile uint32_t *nq = (volatile uint32_t *)0xF1C378u;
                  extern uint32_t gpu_jvdec_verify(void);
                  int ld;
                  *nq = 0; v0[0] = 0; *nq = 0; v0[0] = 0;
                  for (ld = 0; ld < 8; ld++) {
                      gpu_jvdec_load();
                      if (gpu_jvdec_verify() == 0) break;
                  }
                  g_jv_ok = (gpu_jvdec_verify() == 0);
#ifdef VIDDIAG
                  g_jv_vfy = gpu_jvdec_verify();
#endif
                }
            }
#ifdef FASTBOOT
            /* FASTBOOT: straight into the level.  No boot logos, no Start Game
               cinematic, no title splash, no loading art - a test roll should
               reach the area under test in seconds, not after two minutes of
               boot chain.  The level load itself is untouched. */
            for (vc = 3; vc < 3; vc++) {
#elif defined(NOBOOTCLIPS)
            for (vc = 3; vc < (introplay ? 4 : 3); vc++) {
#else
            for (vc = introplay ? 3 : 0; vc < (introplay ? 4 : 3); vc++) {
#endif
                int vh = -1, mi2, vw, vhh, vfps, vnf, fi, remain, have, pos;
                uint32_t t0, plen = 0;
                uint8_t *pcur = ptkA, *pprev = ptkB;
                { extern void video_pin_start(void);
                  video_pin_start(); } /* 2-buffer pin (less rotation churn;
                                          the shadow copy makes correctness
                                          independent of it) */
                { uint32_t *fw = (uint32_t *)vshadow, k9;
                  for (k9 = 0; k9 < (320u*240u)/4u; k9++) fw[k9] = 0; }
                /* boot flow (user 2026-08-05): EIDOS -> CORE -> the disc
                   intro cinematic (CAFE.FMV: snake eye / dig / cafe pitch)
                   -> title. A skips the current clip; A during the intro
                   lands on the main menu. */
                /* clips: boot = EIDOS, CORE, INTRO (the cafe pitch - the
                   PSX's attract intro); Start Game = CAVES.JV (SNOW.FMV,
                   the guide on the snowy approach - the PSX's pre-Caves
                   cinematic; user 2026-08-06: "there are two intros"). */
                for (mi2 = 0; mi2 < 2 && vh < 0; mi2++)
                    vh = gd_fopen(vc == 0 ? (mi2 ? "/EIDOS.JV" : "EIDOS.JV")
                                : vc == 1 ? (mi2 ? "/CORE.JV" : "CORE.JV")
                                : vc == 2 ? (mi2 ? "/INTRO.JV" : "INTRO.JV")
                                          : (mi2 ? "/CAVES.JV" : "CAVES.JV"),
                                  GD_FOPEN_READ | GD_FOPEN_OPEN_EXISTING);
                if (vh < 0) continue;
                /* SECTOR DISCIPLINE (silicon laws, 2026-08-05): 512-granular
                   reads at 512 positions only; 4-aligned destinations. */
                remain = gd_fsize((unsigned)vh);
                if (remain < 1024 + 4096 || (remain & 511) ||
                    gd_fread((unsigned)vh, vb, 1024, GD_FREAD_CPU) != 0 ||
                    vb[0] != 'J' || vb[1] != 'V' || vb[3] != '4') {
                    gd_fclose((unsigned)vh); continue;
                }
                remain -= 1024;
                vw   = (vb[4] << 8) | vb[5];
                vhh  = (vb[6] << 8) | vb[7];
                vfps = (vb[8] << 8) | vb[9];
                vnf  = (vb[10] << 8) | vb[11];
                { int kfonly = vb[12];   /* keyframe-only stream: every
                       frame full-repaints, so the prev-pass is vacuous -
                       the CORE block displacement showed the two-pass
                       phase invariant does not survive silicon */
                  if (kfonly) vnf |= 0x10000; }
                if (vw != 320 || vhh != 240 || vfps <= 0) vnf = 0;
                video_set_clut((const uint16_t *)(vb + 16));
#ifdef VIDCAD
                { volatile uint16_t *cl9=(volatile uint16_t*)0xF00400u;
                  cl9[254]=0x0000; cl9[255]=0xFFFE; }
#endif
                if (gd_fread((unsigned)vh, cbk, 4096, GD_FREAD_CPU) != 0) {
                    gd_fclose((unsigned)vh); continue;
                }
                remain -= 4096;
                have = 0; pos = 0;
#ifdef VIDPANEL
                { int z9; for (z9 = 0; z9 < 9; z9++) VPC[z9] = 0; }
#endif
                t0 = frame_count;
                { int dprev = 0;                  /* last display tick: min-
                                                     spacing scheduler, no
                                                     catch-up bursts */
                { uint32_t vpp = joypad_read();   /* pad held at clip entry
                                                     (e.g. the A that chose
                                                     Start Game) must not
                                                     skip - edges only */
                { /* AUDIO RING (2026-08-06, user: "all clips hitch"):
                     the old two-buffer scheme BLOCKED on the DSP queue
                     slot (up to ~350ms) every ~4KB batch - a rhythmic
                     stall in EVERY clip that the read de-judder could
                     not touch. Six 4KB slots in mbuf (24KB = 2.2s of
                     audio) filled ahead, queued NON-blocking whenever
                     the slot frees; the only blocking wait left is the
                     end-of-clip flush. */
                  int8_t *aring = (int8_t *)mbuf;
                  int acc = 0, wslot = 0, rslot = 0, pend = 0, astarted = 0;
                  int alen[6];
                { int kfonly = (vnf >> 16) & 1; vnf &= 0xFFFF;
                for (fi = 0; fi < vnf; fi++) {
                    uint32_t L, AL;
                    int need = 8;
                    for (;;) {
                        if (have - pos >= need) {
                            if (need > 8) break;
                            L  = ((uint32_t)vb[pos] << 24) | ((uint32_t)vb[pos+1] << 16)
                               | ((uint32_t)vb[pos+2] << 8) | vb[pos+3];
                            AL = ((uint32_t)vb[pos+4] << 24) | ((uint32_t)vb[pos+5] << 16)
                               | ((uint32_t)vb[pos+6] << 8) | vb[pos+7];
                            if (L == 0 || L > 24576u || AL > 4096u || (AL & 3)) {
                                VP_EXIT(2); fi = vnf; break;
                            }
                            need = 8 + (int)AL + (int)((L + 3u) & ~3u);
                            if (have - pos >= need) break;
                        }
                        if (pos) { int mv2 = stream_compact(vb, have, &pos);
                            have = mv2; }
                        { int want = (31488 - have) & ~511;   /* sector-sized */
                          if (want > 24576) want = 24576;     /* >24KB cliffs */
                          if (want > remain) want = remain;
                          if (want <= 0) { VP_EXIT(4); fi = vnf; break; }
#ifdef VIDPANEL
                          { uint32_t ta = vp_tick();
                            int rr9 = gd_fread((unsigned)vh, vb + have,
                                               (unsigned)want, GD_FREAD_CPU);
                            vp_read += vp_tick() - ta;
                            if (rr9 != 0) { VP_EXIT(3); fi = vnf; break; } }
#else
                          if (gd_fread((unsigned)vh, vb + have, (unsigned)want,
                                       GD_FREAD_CPU) != 0) { VP_EXIT(3); fi = vnf; break; }
#endif
                          have += want; remain -= want; }
                    }
                    if (fi >= vnf) break;
                    pos += 8;
                    /* audio -> DSP voice 0, BATCHED ~4KB per buffer (the
                       music engine's granularity): per-frame chunks left
                       only ~150ms of queue cover, and the ~125ms sector
                       refills clipped it - the sparse pops the user heard
                       (waveform-dated to refill cadence, 2026-08-05).
                       Deep buffers ride out any refill. First batch goes
                       early (~2 chunks) so the whoosh isn't late. */
                    if (AL) {
#ifdef VIDPANEL
                        uint32_t tau = vp_tick();
#endif
                        if (g_sfx_ok) {
                            /* close the slot when the next chunk won't
                               fit; a saturated ring drops its OLDEST
                               pending batch so fill never bleeds into a
                               queued slot */
                            if (acc + (int)AL > 4096) {
                                if (pend >= 5) { rslot = (rslot + 1) % 6; pend--; }
                                alen[wslot] = acc; pend++;
                                wslot = (wslot + 1) % 6; acc = 0;
                            }
                            /* the audio chunk is a bulk move too - hand it to
                               the Blitter whenever the phase allows, and only
                               walk the remainder on the 68k */
                            { int8_t *ab = aring + wslot*4096 + acc;
                              const uint8_t *asrc = vb + pos;
                              uint32_t mvd = blit_bytes(asrc, ab, AL);
                              const uint32_t *as2 =
                                  (const uint32_t *)(asrc + mvd);
                              uint32_t *ad = (uint32_t *)(ab + mvd);
                              uint32_t k2;
                              for (k2 = 0; k2 < (AL - mvd) >> 2; k2++)
                                  ad[k2] = as2[k2];
                              acc += (int)AL; }
                            /* NON-BLOCKING service: prime with two batches
                               (arm + queue together - the early-start buzz
                               fix), then push one whenever the DSP slot is
                               free. Never spins. */
                            /* ☠️☠️ NEVER READ F1C378/F1C340 DIRECTLY. Those
                               are JERRY'S LOCAL SRAM, and the 68k cannot read
                               a running JRISC honestly - the same law that
                               forced Tom's jvdec handshake into DRAM. A
                               spurious zero here RE-ARMS the voice, so the
                               buffer restarts from the top every frame and
                               the clip comes out as a machine-gun stutter
                               (user, 2026-08-08). Jerry publishes both
                               counters into the DRAM mailbox; read those.
                               jerry_audio_stale() stamps them after every
                               write so we never act on a pre-write value. */
                            { extern void jerry_sfx_queue(const void*, uint32_t);
                              extern uint32_t jerry_v0_cnt(void);
                              extern uint32_t jerry_v0_ncnt(void);
                              extern void jerry_audio_stale(void);
                              if (!astarted) {
                                  if (pend >= 2) {
                                      jerry_sfx(0, aring + rslot*4096,
                                                (uint32_t)alen[rslot], 0);
                                      rslot = (rslot + 1) % 6; pend--;
                                      jerry_sfx_queue(aring + rslot*4096,
                                                      (uint32_t)alen[rslot]);
                                      rslot = (rslot + 1) % 6; pend--;
                                      astarted = 1;
                                      jerry_audio_stale();
                                  }
                              } else if (pend > 0 && jerry_v0_ncnt() == 0
                                         && jerry_v0_cnt() != 0xFFFFFFFFu) {
                                  /* ☠️ a queue NEVER restarts an idle
                                     voice (music-engine law): after any
                                     stall > the DSP's ~744ms of buffered
                                     audio (clip transitions!), the voice
                                     dies and stays dead - RE-ARM when
                                     V0_CNT reads 0 */
                                  if (jerry_v0_cnt() == 0)
                                      jerry_sfx(0, aring + rslot*4096,
                                                (uint32_t)alen[rslot], 0);
                                  else
                                      jerry_sfx_queue(aring + rslot*4096,
                                                      (uint32_t)alen[rslot]);
                                  rslot = (rslot + 1) % 6; pend--;
                                  jerry_audio_stale();
                              } }
                        }
                        pos += (int)AL;
#ifdef VIDPANEL
                        vp_audio += vp_tick() - tau;
#endif
                    }
                    { uint8_t *tk = vb + pos;
                      /* ☠️ take the back buffer AFTER the pending flip has
                         retired (vidrom order). Reading it while a flip is
                         still queued names the buffer that is about to go on
                         screen. */
                      uint8_t *bb = 0;
                      int gok = 0, kicked = 0;
                      pos += (int)((L + 3u) & ~3u);
                      /* BACK BUFFER ONLY (user: 'a lot of flashes' - the
                         old both-buffers write hit the DISPLAYED buffer
                         mid-scan). The back buffer is two frames stale,
                         so apply the PREVIOUS frame's tokens first, then
                         this frame's; the display only ever shows
                         completed frames. */
                      /* DE-JUDDER (2026-08-06, user: "a bit jumpy"): the
                         frame used to serialise read -> decode-wait ->
                         pace-wait, so any 24KB GD read (~125ms) blew two
                         15fps slots and the cadence lurched. Now the
                         tokens are STASHED first and the GPU decodes from
                         the stash ASYNCHRONOUSLY while the pace window
                         runs SMALL top-up reads (6KB ~= 2 fields) - the
                         stream buffer stays full without ever stalling a
                         frame slot. kf-only streams need no prev tokens,
                         so the stash doubles as the decode source; the
                         legacy two-pass path keeps the serial order. */
                      {
                          int tomok = 0;
#ifdef VIDPANEL
                          { uint32_t ta = vp_tick();
                            while (pending_fb)
                                ;
                            vp_pace += vp_tick() - ta; }
#else
                          while (pending_fb)
                              ;
#endif
                          bb = (uint8_t *)video_backbuffer();
                          /* TOM DECODE INTO THE SHADOW (2026-08-07, user:
                             "video runs off Tom"): with the DSP-quiesced
                             load fixed (context probe J11V0), Tom applies
                             this frame's tokens to the persistent shadow
                             frame; the 68k only block-copies the complete
                             image to the display buffer. Ghost-proof
                             display + GPU decode. The shadow persists, so
                             no prev re-apply is needed (prevlen 0). The
                             68k token walk below remains as the verified
                             fallback if Tom's wait ever fails. */
                          /* ☠️ do NOT gate the kick on g_jv_ok. The verify
                             is a READ of GPU SRAM, and reads are the
                             unreliable direction (writes land - the F03800
                             corruption proved that). Gating on a flaky read
                             can suppress a kick that would have worked; the
                             DRAM mailbox (tomok) is the honest verdict, and
                             the 68k walk is still there if it fails. */
                          if (gpu_ok) {
                              /* KICK STRAIGHT OFF THE STREAM BUFFER (vidrom,
                                 2026-08-08): the stash copy was for the ASYNC
                                 player, where the 68k refilled while Tom
                                 decoded. This kick is synchronous - wait()
                                 returns before anything can touch the buffer -
                                 so copying ~3.5KB a frame into pcur was pure
                                 68k errand-running. */
#ifdef VIDPANEL
                              { uint32_t ta = vp_tick();
                                gpu_jvdec_kick(tk, 0, tk, L, cbk, vshadow);
                                tomok = gpu_jvdec_wait();
                                vp_tom += vp_tick() - ta; }
#else
                              gpu_jvdec_kick(tk, 0, tk, L, cbk, vshadow);
                              tomok = gpu_jvdec_wait();
#endif
                          }
                          if (!tomok)
                          { const uint8_t *s = tk, *e = tk + L;
                            uint8_t *dA = vshadow;
                            int bx = 0, left = 4800;
                            while (s < e && left > 0) {
                                int t2 = *s++;
                                int n2 = (t2 < 128) ? t2 + 1 : t2 - 127;
                                while (n2-- && left > 0) {
                                    if (t2 < 128) {
                                        const uint32_t *cbe;
                                        uint32_t *w0;
                                        if (s >= e) { left = 0; break; }
                                        cbe = (const uint32_t *)
                                              (cbk + ((uint32_t)*s++ << 4));
                                        w0 = (uint32_t *)dA;
                                        w0[0] = cbe[0]; w0[80] = cbe[1];
                                        w0[160] = cbe[2]; w0[240] = cbe[3];
                                    }
                                    dA += 4; left--;
                                    if (++bx == 80) { bx = 0; dA += 960; }
                                }
                            } }
                          /* THE BLITTER MOVES THE FRAME, IN PHRASE MODE.
                             This 19200-long software copy measured 95ms a
                             frame on silicon - three times Tom's whole
                             decode. blit_copy (pixel mode) took it to 38ms;
                             blit_copy_phrase (XADDPHR, 8 bytes a step) to
                             5ms. Falls back to the pixel-mode copy if the
                             Blitter ever fails to report idle. */
#ifdef VIDPANEL
                          { uint32_t ta = vp_tick();
                            if (!blit_copy_phrase(vshadow, bb, 240))
                                blit_copy(vshadow, bb, 240);
                            vp_copy += vp_tick() - ta; }
#else
                          if (!blit_copy_phrase(vshadow, bb, 240))
                              blit_copy(vshadow, bb, 240);
#endif
                          gok = tomok; kicked = tomok;
                          (void)pprev;
                          /* STEADY-RATE refill (GDPROBE-measured, 2026-08:
                             gd_fread = ~3.5ms fixed + 193KB/s - small reads
                             are CHEAP; the '24KB-only' premise was false).
                             7KB every frame = 38ms, fits every 66.7ms slot
                             beside the ~20ms decode/parse, sustains 107KB/s
                             against the stream's 84KB/s - the buffer never
                             drains and the big stalls never happen. Runs
                             while the GPU decodes from the stash. */
#ifdef STEADYREAD
                          /* ☠️ EXPERIMENTAL - 13 boots straight BLACK with
                             this enabled (2026-08-06 night, offsets 0-1904);
                             mechanism unfound by inspection. Bisect with
                             border crumbs before trusting.
                             SRGUARD: volatile gate keeps the code in the
                             image (same layout) but never executes it -
                             boots=runtime fault, black=layout. */
                          /* condition on UNCONSUMED content, not raw fill -
                             the uncompacted `have` sits near-full and the
                             steady read almost never ran (150-184ms stall
                             cluster, sr_par histogram) */
                          if (g_sr_on && remain > 0 && have - pos < 24576) {
                              int want = 7168;
                              if (pos) { int mv = have - pos, k3;
                                  for (k3 = 0; k3 < mv; k3++)
                                      vb[k3] = vb[pos + k3];
                                  have = mv; pos = 0; }
                              if (want > remain) want = remain;
                              if (want > 0 &&
                                  gd_fread((unsigned)vh, vb + have,
                                           (unsigned)want,
                                           GD_FREAD_CPU) == 0) {
                                  have += want; remain -= want;
                              } else remain = 0;
                          }
#else
                          /* STEADY 4KB/frame refill (2026-08-07, user: the
                             kf-only/high-rate clips "chug"): the v8 low-water
                             shape read up to 24KB in one gd_fread = ~125ms =
                             TWO 15fps slots - a visible hiccup at every
                             burst. One 4KB sector-aligned read per frame
                             (~24ms) fits beside the ~25ms decode+copy and
                             sustains ~60KB/s - above every delta clip's
                             rate. The 24KB bulk read remains ONLY as the
                             critical-low rescue (start-up, seeks). */
                          /* ☠️☠️ THE GAME'S CHUG WAS HERE (2026-08-08).
                             The old shape ran its refill whenever
                             have-pos < 26624 - which in steady state is
                             EVERY FRAME - and each pass first compacted the
                             whole ~26KB remainder BYTE BY BYTE. That is a
                             26214-iteration indexed byte loop on a 68000
                             sharing the bus with Tom, Jerry and a 240-line
                             OP: hundreds of ms a frame, and it sat outside
                             every instrumented phase, which is exactly the
                             478ms of "other" the panel reported (1.94 fps in
                             the game vs 14.90 in vidrom, same decoder).
                             This is vidrom's v9 shape verbatim: refill only
                             when the buffer is actually running low (so the
                             compaction moves <=8KB, not 26KB), move LONGS,
                             and TIME the read. */
                          if (remain > 0 && have - pos < 8192) {
                              int want = (have - pos < 4096) ? 24576 : 4096;
                              if (pos) have = stream_compact(vb, have, &pos);
                              if (want > (int)((31488 - have) & ~511))
                                  want = (31488 - have) & ~511;
                              if (want > remain) want = remain;
                              if (want > 0) {
#ifdef VIDPANEL
                                  uint32_t ta = vp_tick();
                                  int rr8 = gd_fread((unsigned)vh, vb + have,
                                                     (unsigned)want,
                                                     GD_FREAD_CPU);
                                  vp_read += vp_tick() - ta;
#else
                                  int rr8 = gd_fread((unsigned)vh, vb + have,
                                                     (unsigned)want,
                                                     GD_FREAD_CPU);
#endif
                                  if (rr8 == 0) { have += want; remain -= want; }
                                  else remain = 0;
                              }
                          }
#endif
                          /* the SECOND wait per frame is gone (2026-08-08):
                             the kick is synchronous, so gpu_jvdec_wait()
                             already returned DONE above. It should return
                             at once, but if it ever missed the mailbox the
                             bound is 240000 polls - seconds of stall for a
                             value we already hold. */
                          (void)kicked;
#ifdef VIDCAD
                          /* cadence ground truth: 16x8 parity block the
                             capture rig reads - immune to clip content */
                          { uint8_t *cb8 = (uint8_t *)bb; int y8, x8;
                            uint8_t v8 = (fi & 1) ? 255 : 254;
                            for (y8 = 0; y8 < 8; y8++)
                                for (x8 = 0; x8 < 16; x8++)
                                    cb8[y8*320 + x8] = v8; }
#endif
                      }
                      (void)kfonly;   /* shadow path: every frame complete */
#ifdef VIDPANEL
                      /* THE CALIBRATED PANEL (2026-08-08). Replaces the 8px
                         VIDDIAG squares, which have no border and no
                         calibration word and produced two wrong diagnoses
                         when read off a capture. scratchpad/readout.py
                         decodes this identically to vidrom's. */
                      /* ☠️☠️ THE INSTRUMENT MUST NOT BE THE EXPERIMENT.
                         Painting this panel every frame cost 329ms a frame on
                         silicon - measured, and larger than every real phase
                         of the player put together (read 16, tom 20, copy 7).
                         Same paint costs 62ms in vidrom: identical code, 5.3x,
                         because the game keeps the bus hot and the 68000 is
                         starved in proportion.  So the frame loop now only
                         BUMPS COUNTERS, and the panel is painted ONCE on the
                         hold screen after the clip - where its cost is
                         nobody's frame time.  (User: the 68000 is for boot
                         code, nothing else - diagnostics included.) */
                      { extern volatile uint32_t frame_count;
                        if (!gok) vp_tomfail++;   /* gok = tomok, hoisted */
                        vp_frames++;
                        vp_fields = frame_count - t0; }
#endif
#ifdef VIDDIAG
                      /* DECODE-STATUS MARKER (2026-08-07 ghost hunt): 8x8 at
                         top-right. Bright white = Tom wrote DONE; dark grey =
                         wait TIMED OUT and the 68k fallback painted. If the
                         cinematic flashes dark-grey markers, the silicon
                         story is decode-timeout races, which the sim (full
                         budget, no contention) can never reproduce. */
                      { uint8_t *mb = (uint8_t *)bb; int my, mx;
                        /* v2: MID-BAND so the letterbox can't hide it, and
                           frame-alternating so a shown marker PROVES the
                           displayed buffer is the one we decoded into */
                        uint8_t hb = (fi & 1) ? 255 : 1;   /* heartbeat  */
                        uint8_t st = gok ? 255 : 64;       /* gok status */
                        /* third square: did the kernel STICK ITS HEAD UP?
                           (hello magic at PARAMS+36, written at kernel
                           entry before any decode). White = Tom started;
                           dark = the restart itself never ran. */
                        extern uint32_t gpu_jvdec_hello(void);
                        uint8_t he = (gpu_jvdec_hello() == 0x0A3D0001u)
                                     ? 255 : 64;
                        for (my = 100; my < 108; my++)
                            for (mx = 0; mx < 12; mx++)
                                mb[my*320 + 296 + mx] = hb;
                        for (my = 112; my < 120; my++)
                            for (mx = 0; mx < 12; mx++)
                                mb[my*320 + 296 + mx] = st;
                        for (my = 124; my < 132; my++)
                            for (mx = 0; mx < 12; mx++)
                                mb[my*320 + 296 + mx] = he;
                        /* square 4: SRAM verify (white = kernel bytes hold) */
                        { extern uint32_t gpu_pc_read(void);
                          uint8_t sv = (g_jv_vfy == 0) ? 255 : 64;
                          uint32_t pc = gpu_pc_read();
                          /* square 5: PC in SRAM range = Tom ran our code */
                          uint8_t pq = (pc >= 0xF03000u && pc < 0xF04000u)
                                       ? 255 : 64;
                          for (my = 136; my < 144; my++)
                              for (mx = 0; mx < 12; mx++)
                                  mb[my*320 + 296 + mx] = sv;
                          for (my = 148; my < 156; my++)
                              for (mx = 0; mx < 12; mx++)
                                  mb[my*320 + 296 + mx] = pq;
                          /* squares 6-7: SRAM r/w pattern test results for
                             post-init [0] and this video context [1 or 2] -
                             white = clean. If these are WHITE while the
                             kernel verify square is dark, the corruption is
                             in the ROM SOURCE reads (GD-served), not the
                             SRAM writes. */
                          /* ☠️ gok/hello/verify are ALL inside `if (gpu_ok)`,
                             so "all three dark" is ALSO exactly what
                             gpu_ok==0 looks like - and I read it as "Tom
                             won't start" without ever checking gpu_ok.
                             Square 8 = gpu_ok, square 9 = the verified load.
                             Measure the premise, not just the conclusion. */
                          { uint8_t g8 = gpu_ok ? 255 : 64;
                            uint8_t g9 = g_jv_ok ? 255 : 64;
                            for (my = 184; my < 192; my++)
                                for (mx = 0; mx < 12; mx++)
                                    mb[my*320 + 296 + mx] = g8;
                            for (my = 196; my < 204; my++)
                                for (mx = 0; mx < 12; mx++)
                                    mb[my*320 + 296 + mx] = g9; }
                          { uint8_t s6 = (g_sr[0] == 0) ? 255 : 64;
                            uint8_t s7 = (g_sr[introplay ? 2 : 1] == 0)
                                         ? 255 : 64;
                            for (my = 160; my < 168; my++)
                                for (mx = 0; mx < 12; mx++)
                                    mb[my*320 + 296 + mx] = s6;
                            for (my = 172; my < 180; my++)
                                for (mx = 0; mx < 12; mx++)
                                    mb[my*320 + 296 + mx] = s7; } } }
#endif
                      /* ping-pong: this frame's tokens become next frame's
                         prev (both paths copied the tokens into pcur) */
                      { uint8_t *sw = pprev; pprev = pcur; pcur = sw; }
                      plen = (L <= 7552) ? L : 0; }
                    { int tgt, nowr;
                      /* previous frame consumed AT ITS FIELD first, so the
                         schedule anchors to ACTUAL display times - v6
                         anchored to virtual time and burst at 33ms once
                         late (measured, cadence6) */
                      while (pending_fb)
                          ;
                      nowr = (int)(frame_count - t0);
                      tgt = ((fi + 1) * 60) / vfps;
                      if (tgt < dprev + 60 / vfps) tgt = dprev + 60 / vfps;
                      if (tgt < nowr + 1)          tgt = nowr + 1;
                      video_pend_at = t0 + (uint32_t)tgt;
                      dprev = tgt;
                      video_flip();
                      /* ☠️ AND WAIT FOR THE FIELD OURSELVES.  Handing the
                         schedule to the ISR via video_pend_at is not a
                         guarantee: not every compiled ISR path checks it (one
                         flips unconditionally), so the clip displayed as fast
                         as it decoded - measured 152 frames in 469 fields =
                         19.45 fps for a 15fps clip, a quarter too fast, with
                         the audio pushed along at the same rate.  vidrom
                         always waited on frame_count itself and always ran at
                         exactly 15.00; do that here too.  The deferred flip
                         still smooths the cadence, this only floors it. */
                      while ((int)(frame_count - t0) < tgt)
                          ; }
                    { uint32_t vp2 = joypad_read();
                      if (vp2 & ~vpp & (PAD_A | PAD_B | PAD_C)) { VP_EXIT(5); break; }
                      vpp = vp2; }
                }
                }
                /* end-of-clip flush: close the partial slot, then drain
                   the ring - blocking is fine here, the clip is over */
                if (g_sfx_ok) {
                    volatile uint32_t *ncnt = (volatile uint32_t *)0xF1C378u;
                    extern void jerry_sfx_queue(const void*, uint32_t);
                    if (acc && pend < 5) {
                        alen[wslot] = acc; pend++;
                        wslot = (wslot + 1) % 6; acc = 0;
                    }
                    while (pend > 0) {
                        if (!astarted) {
                            jerry_sfx(0, aring + rslot*4096,
                                      (uint32_t)alen[rslot], 0);
                            astarted = 1;
                        } else {
                            volatile uint32_t *vcnt =
                                (volatile uint32_t *)0xF1C340u;
                            uint32_t w2;
                            for (w2 = 0; w2 < 400000u && *ncnt; w2++)
                                ;
                            if (*ncnt) break;      /* DSP wedged: stop */
                            if (*vcnt == 0)
                                jerry_sfx(0, aring + rslot*4096,
                                          (uint32_t)alen[rslot], 0);
                            else
                                jerry_sfx_queue(aring + rslot*4096,
                                                (uint32_t)alen[rslot]);
                        }
                        rslot = (rslot + 1) % 6; pend--;
                    }
                }
#ifdef VIDPANEL
                /* ONE paint per clip, on a hold screen the capture can read at
                   leisure.  Whole-clip totals, so no single frame's noise can
                   masquerade as the average, and the exit code says whether
                   the clip PLAYED OUT or died on a bad record. */
                { uint8_t lamps[8]; uint32_t rows[12]; int q;
                  extern volatile uint32_t frame_count;
                  uint8_t *hb2 = (uint8_t *)video_backbuffer();
                  uint32_t h0;
                  for (q = 0; q < 12; q++) rows[q] = 0;
                  lamps[0] = (uint8_t)(vc & 1);
                  lamps[1] = (uint8_t)(gpu_ok != 0);
                  lamps[2] = (uint8_t)(g_jv_ok != 0);
                  lamps[3] = (uint8_t)(vp_tomfail == 0);
                  { extern uint32_t gpu_jvdec_hello(void);
                    lamps[4] = (uint8_t)(gpu_jvdec_hello() == 0x0A3D0001u); }
                  lamps[5] = (uint8_t)(vp_frames != 0);
                  { extern uint32_t gpu_pc_read(void); uint32_t pc9 = gpu_pc_read();
                    lamps[6] = (uint8_t)(pc9 >= 0xF03000u && pc9 < 0xF04000u);
                    rows[8] = pc9; }
                  lamps[7] = 1;
                  rows[1] = vp_frames;  rows[2] = vp_fields;
                  rows[3] = vp_read;    rows[4] = vp_tom;
                  rows[5] = vp_copy;
                  rows[6] = vp_pace;    rows[7] = vp_audio;
                  rows[9] = vp_tomfail;
                  rows[10] = 0x80000000u | (uint32_t)(vnf & 0xFFFF)
                                         | ((vp_exit & 0xFu) << 16);
                  rows[11] = (uint32_t)fi;
                  vp_clut();
                  vp_paint(hb2, lamps, rows);
                  video_pend_at = 0;          /* flip NOW, not on a schedule */
                  video_flip();
                  h0 = frame_count;
                  while (frame_count - h0 < 180u)   /* 3s: two capture stills */
                      ;
                  vp_exit = 0; }
#endif
                } } }
                gd_fclose((unsigned)vh);
            }
            video_pend_at = 0;     /* every later flip is immediate again */
            video_two_buf = 0;     /* release the fb0/fb1 pin */
            g_jv_early = 0;        /* boot pass done: later entries (Start
                                      Game) must load the kernel themselves */
            /* scrub ALL THREE framebuffers: video wrote 240-line frames and
               pinned to two buffers - stale rows/pixels otherwise ghost into
               the title and show as LINES below the game's 120-line window
               (user 2026-08-07). crash_fbs[] names every buffer. */
            /* ☠️☠️ SCRUBBING TO "BLACK" MEANT SCRUBBING TO WHITE (user,
               2026-08-09: "when we hit the end of the attract video there's a
               white flash").  The buffers are filled with palette index 0 -
               and index 0 is PURE WHITE in EIDOS, CORE and CAVES, near-white
               in INTRO, because the encoder had no reason to reserve it.  So
               every clip ended on a full-screen white frame.  Force entry 0
               black BEFORE the fill; the title/level set_clut rewrites
               0..253 straight after, so nothing else notices.
               The fill itself is 57,600 long stores across three buffers -
               the Blitter's job, not the 68000's. */
            { volatile uint16_t *cl0 = (volatile uint16_t *)0xF00400u;
              cl0[0] = 0x0000; }
            { extern uint8_t *const crash_fbs[3]; int b3;
              for (b3 = 0; b3 < 3; b3++)
                  blit_band(crash_fbs[b3], 0, 240, 0); }
            if (gpu_ok)
                gpu_jvdec_done();   /* restore the init-once kernel params
                                       (mailbox ptr) the video block used */
          }
          if (introplay) goto bv_done;   /* Start Game: intro played, go */
#endif
          /* PS1 ORDER: the disc shows its title splash with a filling
             progress bar between the attract videos and the title menu, and
             the ring staging below is exactly the gap it covers. */
#ifndef FASTBOOT
          show_title_load(120u);          /* ~2s, matching the reference */
#endif
          /* RING ORDER: 0 = Game (passport), 1 = Controls, 2 = Lara's Home.
             Slot 3 is the OPENED passport - staged like the rest but not a
             ring item. (Sound is deferred - user 2026-07-29.) */
          /* PSX title ring order (reference video 17-22-54): RIGHT of Game
             is Screen Adjust, LEFT is Lara's Home; Sound and Controls fill
             the far side. Slot RING_N = the opened passport spread. */
          rsrc[0]=pass_geom;   ratl[0]=pass_atlas;
          rsrc[1]=detail_geom; ratl[1]=detail_atlas;
          rsrc[2]=sound_geom;  ratl[2]=sound_atlas;
          rsrc[3]=ctrl_geom;   ratl[3]=ctrl_atlas;
          rsrc[4]=photo_geom;  ratl[4]=photo_atlas;
          { int c7; for (c7 = 0; c7 < 232; c7++) p2src[c7] = pass2_geom[c7]; }
          rsrc[5]=p2src;       ratl[5]=pass2_atlas;
          { int it2;
            for (it2=0; it2<6; it2++) {
              const uint8_t *sb=rsrc[it2];
              int nv=(sb[0]<<8)|sb[1], nq=(sb[2]<<8)|sb[3], nt=(sb[4]<<8)|sb[5];
#ifndef STAGEDIET
              int len=16+nv*8+nq*24+nt*18, i3;
              if (len > (int)sizeof(rblob[0])) len = sizeof(rblob[0]);
              for (i3=0;i3<len;i3++) rblob[it2][i3]=sb[i3];
              rvcnt[it2]=nv; rblen[it2]=len;
#else
              /* title assets are LEGACY format — copy record-wise, inserting
                 the dummy plane prefix the STAGEDIET kernel expects.
                 ☠️ TRIS ARE PROMOTED TO REPEATED-CORNER QUADS (2026-08-05):
                 the title path tears tri records on silicon (pad
                 checkerboard, Sound headphones holes) while every all-quad
                 item is solid — in-game tris are fine, so the fault sits in
                 this path's tri handling. Every tri becomes a quad with
                 v3=v2 / uv3=uv2: a degenerate fourth corner renders the same
                 triangle through the proven quad path. */
              /* the promoted quad's 4th corner must be a REAL vertex: a
                 repeated corner makes a zero-length edge the kernel
                 mishandles (still-broken headphones, the pad's notch). Each
                 tri gets an appended vertex at the MIDPOINT of its closing
                 edge - coverage is exactly the triangle, four distinct
                 verts. 68k is big-endian, so s16 blob fields read native. */
              int hdr=16+(nv+nt)*8, i3, f3;
              int len=hdr+(nq+nt)*QREC;
              uint16_t *w2; const uint16_t *s2;
              /* per-item world scale (256 = 1.0): the PS1 reference
                 (screencast 18-36-05) shows the closed passport at ~27%%
                 of screen height; ours measured ~15%% - the model is
                 authored small. 460/256 = x1.8. */
              static const uint16_t rscale[6] = {460,256,256,256,256,256};
              if (len > (int)sizeof(rblob[0])) { nq=0; nt=0; hdr=16+nv*8; len=hdr; }
              for (i3=0;i3<16+nv*8;i3++) rblob[it2][i3]=sb[i3];
              if (rscale[it2] != 256) {
                  int16_t *vv2 = (int16_t*)(rblob[it2]+16);
                  int k4;
                  for (k4 = 0; k4 < nv; k4++) {
                      vv2[k4*4+0] = (int16_t)(((int)vv2[k4*4+0]*rscale[it2])>>8);
                      vv2[k4*4+1] = (int16_t)(((int)vv2[k4*4+1]*rscale[it2])>>8);
                      vv2[k4*4+2] = (int16_t)(((int)vv2[k4*4+2]*rscale[it2])>>8);
                  }
              }
              { const int16_t *sv=(const int16_t*)(rblob[it2]+16);
                int16_t *dv=(int16_t*)(rblob[it2]+16+nv*8);
                const uint16_t *tf=(const uint16_t*)(sb+16+nv*8+nq*24);
                for (f3=0;f3<nt;f3++){
                    int tv0=tf[f3*9+0], tv2=tf[f3*9+2];
                    dv[f3*4+0]=(int16_t)((sv[tv0*4+0]+sv[tv2*4+0])>>1);
                    dv[f3*4+1]=(int16_t)((sv[tv0*4+1]+sv[tv2*4+1])>>1);
                    dv[f3*4+2]=(int16_t)((sv[tv0*4+2]+sv[tv2*4+2])>>1);
                    dv[f3*4+3]=255; } }
              /* staged header: every face is a quad, verts grew by nt */
              rblob[it2][0]=(uint8_t)((nv+nt)>>8);
              rblob[it2][1]=(uint8_t)(nv+nt);
              rblob[it2][2]=(uint8_t)((nq+nt)>>8);
              rblob[it2][3]=(uint8_t)(nq+nt);
              rblob[it2][4]=0; rblob[it2][5]=0;
              w2=(uint16_t*)(rblob[it2]+hdr); s2=(const uint16_t*)(sb+16+nv*8);
              for (f3=0;f3<nq;f3++){ EMIT_PLANE(w2);
                  for(i3=0;i3<12;i3++) w2[i3]=s2[i3]; w2+=12; s2+=12; }
              for (f3=0;f3<nt;f3++){ EMIT_PLANE(w2);
                  w2[0]=s2[0]; w2[1]=s2[1]; w2[2]=s2[2];
                  w2[3]=(uint16_t)(nv+f3);
                  w2[4]=s2[3]; w2[5]=s2[4];
                  w2[6]=s2[5]; w2[7]=s2[6];
                  w2[8]=s2[7]; w2[9]=s2[8];
                  w2[10]=(uint16_t)((s2[7]+s2[3])>>1);
                  w2[11]=(uint16_t)((s2[8]+s2[4])>>1);
                  w2+=12; s2+=9; }
              /* bake only the REAL verts each frame (the ROM source has no
                 midpoints); the appended verts are rebuilt post-bake from
                 the baked endpoints - midpoints commute with the affine
                 bake, so the result is exact. */
              rvcnt[it2]=nv; rnv0[it2]=nv; rnq0[it2]=nq; rnt0[it2]=nt;
              rblen[it2]=len;
#endif
            } }
#ifndef NO_GAMEDRIVE
          { int mi;
            for (mi = 0; mi < 2 && mh < 0; mi++)
                mh = gd_fopen(mi ? "/MUSIC.PCM" : "MUSIC.PCM",
                              GD_FOPEN_READ | GD_FOPEN_OPEN_EXISTING);
            if (mh >= 0) { msz = gd_fsize((unsigned)mh); mleft = msz; }
            if (mh >= 0 && msz > 0) {
                int n = mleft < (int)sizeof(mbuf[0]) ? mleft : (int)sizeof(mbuf[0]);
                if (n && gd_fread((unsigned)mh, mbuf[0], (unsigned)n, GD_FREAD_CPU) == 0)
                    { mlq[0] = n; mleft -= n; }
                n = mleft < (int)sizeof(mbuf[0]) ? mleft : (int)sizeof(mbuf[0]);
                if (n && gd_fread((unsigned)mh, mbuf[1], (unsigned)n, GD_FREAD_CPU) == 0)
                    { mlq[1] = n; mleft -= n; }
                if (g_sfx_ok && mlq[0]) {
                    extern void jerry_sfx_queue(const void*, uint32_t);
                    jerry_sfx(0, mbuf[0], (uint32_t)mlq[0], 0);
                    if (mlq[1]) jerry_sfx_queue(mbuf[1], (uint32_t)mlq[1]);
                    mplay = 1;           /* mbuf[1] = last handed to the DSP */
                }
            } }
#endif
          /* ---- task #4: TITLE MODE - native 240-line display + 240-line
             projection kernel. The game phase switches both back below. */
          { extern void video_set_disp240(int); extern void gpu_kernel_select(int);
            video_set_disp240(1); gpu_kernel_select(1); }
          for (;;) {
              /* PHYSICAL PAD ONLY, DEBOUNCED: a bit counts only when TWO
                 consecutive reads agree (single-frame pad glitches were
                 phantom-selecting page 0 right after the settle window). */
              uint32_t p = joypad_read();
              uint32_t edge;
#ifndef NO_GAMEDRIVE
              /* stream service: when the playing buffer drains, arm the
                 other IMMEDIATELY (audio first), then refill the drained
                 one from SD (~8KB = 0.74s of headroom per swap). EOF =
                 close+reopen: the theme loops. */
              if (mh >= 0 && g_sfx_ok && g_musvol && mkick) {
                  /* UNMUTE: both buffers drained during the mute - re-prime
                     exactly like boot (fill two, kick voice 0, queue one). */
                  int n0;
                  mkick = 0;
                  n0 = mleft < (int)sizeof(mbuf[0]) ? mleft : (int)sizeof(mbuf[0]);
                  mlq[0] = 0; mlq[1] = 0;
                  if (n0 && gd_fread((unsigned)mh, mbuf[0], (unsigned)n0, GD_FREAD_CPU) == 0)
                      { mlq[0] = n0; mleft -= n0; }
                  n0 = mleft < (int)sizeof(mbuf[0]) ? mleft : (int)sizeof(mbuf[0]);
                  if (n0 && gd_fread((unsigned)mh, mbuf[1], (unsigned)n0, GD_FREAD_CPU) == 0)
                      { mlq[1] = n0; mleft -= n0; }
                  if (mlq[0]) {
                      extern void jerry_sfx_queue(const void*, uint32_t);
                      jerry_sfx(0, mbuf[0], (uint32_t)mlq[0], 0);
                      if (mlq[1]) jerry_sfx_queue(mbuf[1], (uint32_t)mlq[1]);
                      mplay = 1;
                  }
              }
              else if (mh >= 0 && g_sfx_ok && g_musvol) {
                  /* gapless service: the pump promoted the queued buffer
                     (NCNT==0) -> refill the dead one and re-queue it.
                     CHUNKED (2026-08-06): the fill used to be one 8KB
                     CPU-mode GD read - under the faster ring renderer's bus
                     load that single call stalls the 68k for hundreds of ms,
                     which read as PAD LAG, and starves the DSP's sample
                     fetches, which read as CHOPPY MUSIC (both user). 2KB
                     per frame keeps every stall short; the queue headroom
                     is 0.74s and the spread fill finishes in ~4 frames. */
                  volatile uint32_t *ncnt = (volatile uint32_t *)0xF1C378u;
                  static int mfo = -1;               /* fill offset, -1 idle */
                  static int mfdead = 0, mfgoal = 0;
                  if (mfo < 0 && *ncnt == 0) {
                      if (mleft <= 0) {              /* EOF: reopen to loop */
                          int mi;
                          gd_fclose((unsigned)mh); mh = -1;
                          for (mi = 0; mi < 2 && mh < 0; mi++)
                              mh = gd_fopen(mi ? "/MUSIC.PCM" : "MUSIC.PCM",
                              GD_FOPEN_READ | GD_FOPEN_OPEN_EXISTING);
                          mleft = mh >= 0 ? msz : 0;
                      }
                      mfdead = mplay ^ 1;
                      mfgoal = mleft < (int)sizeof(mbuf[0]) ? mleft
                                                            : (int)sizeof(mbuf[0]);
                      mlq[mfdead] = 0;
                      mfo = (mh >= 0 && mfgoal > 0) ? 0 : -1;
                  }
                  if (mfo >= 0) {
                      int step = mfgoal - mfo;
                      if (step > 4096) step = 4096;
                      if (gd_fread((unsigned)mh, mbuf[mfdead] + mfo,
                                   (unsigned)step, GD_FREAD_CPU) == 0) {
                          mfo += step;
                          if (mfo >= mfgoal) {
                              extern void jerry_sfx_queue(const void*, uint32_t);
                              mlq[mfdead] = mfgoal; mleft -= mfgoal;
                              jerry_sfx_queue(mbuf[mfdead], (uint32_t)mfgoal);
                              mplay = mfdead; mfo = -1;
                          }
                      } else mfo = -1;               /* read fault: retry swap */
                  }
              }
#endif
              stable = p & praw;        /* 2-frame agreement */
              praw = p;
              if (armed < 6) {          /* fixed short blind window at boot:
                                           swallows single-frame pad phantoms
                                           WITHOUT punishing early presses
                                           (the old reset-on-activity made
                                           controls feel dead for seconds) */
                  armed++;
                  stable = 0;
              }
              edge = stable & ~sprev;
              sprev = stable;
#ifdef RINGDEMO
              /* rig-only: no remote pad injection exists, so tap RIGHT on a
                 timer to exercise the ring slew for capture. Never ship. */
              { static int rdt = 0;
                if (++rdt >= (RINGDEMO)) { rdt = 0; edge |= PAD_RIGHT; } }
#endif
              /* LIVE FRAME (3D passport spins over the art): repaint the
                 page art every frame (doubles as the clear), orbit-camera
                 the passport blob through the normal geotex kernel, flip. */
              { fbpix *tfb = (fbpix *)video_backbuffer();
                  const uint8_t *simg = title_img;   /* art constant; the
                                          3D relic (passport/photo) IS the page */
                  int i2, d, xx, yy;
                  (void)gymload_img; (void)gymload_pal;
                  if (shown != page)
                      video_set_clut(title_pal);
#ifdef LOWRES
                  /* art is 320x240: take every other row for the 120-line fb
                     (displayed full height again — by the Blitter line-double
                     under HALFRES, by the OP vertical scaler under LOWRES).
                     Was #ifdef HALFRES, which sent the plain LOWRES build down
                     the blit_copy path below: that copies RENDER_H rows, i.e.
                     only the TOP HALF of the 240-line art, which the scaler
                     then stretched over the whole screen — the mangled title
                     visible in any LOWRES filmstrip. Both half-height paths
                     want the same decimation; the full-240 build still takes
                     the straight copy.
                     (Native-240 composite tried 2026-07-12: too slow on the
                     68k — revisit with a Blitter composite, task #29.) */
                  /* task #4: the title displays plain-240 now - copy the
                     320x240 art 1:1 (it was decimated to 120 before). */
#define TITLE_ART_H 240
#else
#define TITLE_ART_H RENDER_H
#endif
                  /* panel pages re-dim every frame, so pixel persistence is
                     impossible there - they keep the full repaint. The ring
                     page repaints fully only on fullpaint frames; otherwise
                     it restores the art behind last frame's movers (this
                     buffer = 2 logical frames ago) plus the label band, and
                     the statics' pixels simply survive. */
                  { int fbi2 = -1, fi3;
                    int fastring2;
                    for (fi3 = 0; fi3 < 3; fi3++)
                        if (tfb == (fbpix *)fbp3[fi3]) fbi2 = fi3;
                    if (fbi2 < 0) {
                        for (fi3 = 0; fi3 < 3 && fbi2 < 0; fi3++)
                            if (!fbp3[fi3]) { fbp3[fi3] = (void *)tfb; fbi2 = fi3; }
                        if (fbi2 < 0) { fbp3[0] = (void *)tfb; fbi2 = 0;
                                        fullpaint = 3; }
                    }
                    ringfbi = fbi2;
                    /* a panel just closed: the ring page under it is stale */
                    { int pn2 = (popen || copen || sopen) ? 1 : 0;
                      if (panelPrev && !pn2) fullpaint = 3;
                      panelPrev = pn2; }
                    fastring2 = gpu_ok && !popen && !copen && !sopen &&
                                fullpaint == 0 && ringK >= RING_SLEW_T;
                    if (!fastring2) {
                        blit_copy(simg, tfb, TITLE_ART_H);
                        if (fullpaint && !popen && !copen && !sopen)
                            fullpaint--;
                    }
                    /* fast frames restore art sub-rects instead - but only
                       AFTER gpu_sync below: the geotex kernel drives the
                       Blitter from Tom, and a 68k Blitter poke mid-kernel
                       would interleave register state. */
                    ringfast = fastring2; }
                  shown = page;
                  /* the 3D passport (TITLE.PSX model 71), orbiting */
                  if (gpu_ok) {
                      /* SPLIT PLACEMENTS (per-model, per-state): the two
                         models are authored facing OPPOSITE ways and need
                         individual parking spots + yaws. phase 0 = passport
                         selected, 256 = photo selected; positions/yaws lerp.
                         {selx,sely,selz,selyaw, unx,uny,unz,unyaw} */
                      /* Overridable from the Makefile so the placement can be
                         swept offline in jagemu (the title is a valid oracle:
                         emulator vs silicon measured 61.3%% vs 63.8%% on the
                         old model).  Measure the model's footprint by DIFFING
                         a render against one with PASS_Z pushed to 9000 —
                         a white-pixel bbox reads the background art's bright
                         patch instead and says "z does nothing". */
/* Tuned 2026-07-29 against the original's title screen, measured by DIFF
   against a render with the passport hidden (z=9000) and taking the largest
   connected component - a plain bbox picks up the ring's highlights and the
   photo, and reads ~44%% for a booklet that is really 18%%.
   reference : 24.6%% of frame height, centre (46.4%%, 79.4%%)
   these     : 24.4%%,                 centre (46.6%%, 79.8%%)
   Previous values (-39, 319, 700) put it a third too far away. */
/* THE RING: 0 = Game (passport), 1 = Controls, 2 = Lara's Home.
   Sound is deferred to last (user 2026-07-29) and is NOT on the ring yet. */
#define RING_N 5
/* Controls item orientation - NOT yet tuned against the reference. Use the
   offline projection search (rank by hull area) the way the passport was. */
#ifndef CTRL_YAW
#define CTRL_YAW 0
#endif
#ifndef CTRL_PITCH
#define CTRL_PITCH 0
#endif
#ifndef CTRL_ROLL
#define CTRL_ROLL 0
#endif

#ifndef PASS_X
#define PASS_X (-45)
#endif
#ifndef PASS_Y
#define PASS_Y 297
#endif
#ifndef PASS_Z
#define PASS_Z 719
#endif
/* The OPEN spread (model 71). Reference: it fills the middle of the screen,
   ~60%% of frame height, centred (45%%, 75%%), running off the bottom edge. */
/* Tuned 2026-07-29 against the reference spread, measured with image moments
   (bbox alone cannot see TILT, and tilt was the visible fault):
     reference : top 40.4%  centre (42.7%, 68.2%)  width 57.6%  tilt -2.2 deg
     these     : top 40.3%  centre (41.2%, 71.8%)  width 56.6%  tilt -4.2 deg
   Both are clipped by the bottom edge, as the original is. */
#ifndef PASS_OPEN_X
#define PASS_OPEN_X 0
#endif
#ifndef PASS_OPEN_Y
#define PASS_OPEN_Y 32
#endif
#ifndef PASS_OPEN_Z
#define PASS_OPEN_Z 205
#endif
/* yaw 218 / pitch 240 / roll 48: a LEVEL, symmetric, face-on two-page spread
   with the centre fold, as in the reference.
   ☠️ Found only after adding PROJECTED AREA to the offline search. Scoring the
   silhouette alone (aspect + axis angle) cannot tell FACE-ON from EDGE-ON - an
   edge-on book is also wide and level - and the top-ranked "level, correct
   proportion" candidates were all slivers. A flat object's projected area is
   maximal when it faces the camera, so ranking by hull area picks the right
   one. Same class of mistake as scoring a bbox instead of principal extents. */
#ifndef PASS_OPEN_YAW
/* 2026-08-04: bake order changed to PITCH-first (d26dcd4) which INVALIDATED
   the 07-29 tuned open pose (218/240/48, old YAW-first order) - the spread
   came in rolled ~40 deg. These are the EXACT ZYX re-decomposition of that
   verified rotation matrix under the new order (Rz67*Ry222*Rx231 == old
   Rz48*Rx240*Ry218, max element err 0.012). Yaw is written -34 (== 222 mod
   256) so the open lerp takes the SHORT arc - the long sweep read as
   "opens sideways" (user 2026-08-04). */
/* RE-POSED 2026-08-06 (user ref 11-42-22): the old -34/231/67 pose was
   fitted while the spread rendered as a WHITE OUTLINE (single-sided
   extraction culled the page faces) - it matched silhouette, not
   content. With PASS_DOUBLE pages visible: yaw 128 shows the leaf
   side the PSX shows (PLAIN pale pages - the stamps/photo spread is
   the other side, ref 12-23-17 vs 11-42-22), roll 64 = straight-on. */
#define PASS_OPEN_YAW 0
#endif
/* The CLOSED ring booklet keeps roll 0: 32/-32/224 were all tried offline and
   every one looked WORSE than what ships (it lies flatter, not more upright).
   The roll term is still what the open spread needs - see PASS_OPEN_ROLL. */
/* CLOSED RING BOOKLET ORIENTATION (2026-07-29).
   The ring yaw was tuned when this slot held model 71 (the OPENED passport)
   and was never re-tuned after the switch to 81 - so the booklet lay FLAT and
   showed its gold-speckled inside pages instead of standing up on its navy
   cover. Found by projecting the model's 16 verts offline over a yaw x pitch x
   roll grid and scoring against the reference (portrait, aspect 1.53, long
   axis vertical) - a few milliseconds per candidate instead of ~70 s per
   emulator render.
   ☠️ TWO MEASUREMENT TRAPS, both of which sent me the wrong way first:
   (1) The render buffer is 320x119 representing 320x240, so it is VERTICALLY
       COMPRESSED 2x. Any aspect or angle read off it must be corrected before
       comparing with a display-space reference - uncorrected, an aspect of
       1.00 in the buffer is really 2.00 on screen.
   (2) A BOUNDING BOX cannot tell a solid rectangle from a thin diagonal bar -
       both have a big bbox. Score the PRINCIPAL EXTENTS (eigenvalues of the
       second-moment matrix), not the bbox: the first search maximised bbox
       aspect and produced an edge-on sliver that looked plausible only because
       I judged it from a magnified crop.
   The offline projection must include the CAMERA's own pitch (tcam uses
   COS(6)/SIN(6)) or its predictions do not match the kernel; calibrate it
   against real measured renders before trusting a search over it. */
#ifndef PASS_YAW
/* 128 = 180 in SINTAB-effective units: the PSX greet lands with the
   gold crest toward the viewer (ref 11-16-51); 0 greeted back-first. */
#define PASS_YAW 128
#endif
/* 118 = 246 flipped 180 deg about Y. The user spotted that the ring was
   showing the BACK of the passport: the silhouette is identical either way, so
   the orientation search could not tell them apart - only the TEXTURE does.
   The back is a plain dark panel; the front carries the crest and lettering. */
#ifndef PASS_PITCH
#define PASS_PITCH 256
#endif
#ifndef PASS_OPEN_PITCH
#define PASS_OPEN_PITCH 0
#endif
#ifndef PASS_ROLL
#define PASS_ROLL 0
#endif
/* Screen Adjust sunglasses (model 95): lying-authored, stand with pitch. */
#ifndef SA_YAW
#define SA_YAW 0
#endif
#ifndef SA_PITCH
#define SA_PITCH 224
#endif
#ifndef SA_ROLL
#define SA_ROLL 0
#endif
/* Sound cassette (model 96): same lying-authored convention as the others -
   stand it up with pitch, no yaw/roll until tuned against the reference. */
#ifndef SND_YAW
/* SINTAB is 256/turn and title_bake masks &255: 512 ALIASED TO 0 (the
   morning's "flip" was a no-op). 128 = the real 180 - cassette door to
   the camera (user 2026-08-06). SND_ROLL 512 was the same alias = 0. */
#define SND_YAW 128
#endif
#ifndef SND_PITCH
#define SND_PITCH 0
#endif
#ifndef SND_ROLL
#define SND_ROLL 0
#endif
#ifndef PASS_OPEN_ROLL
#define PASS_OPEN_ROLL 0
#endif
#define PASS_OPEN_TICKS 6
/* lazy-susan constants: least-squares fit to reference 11-03-33 item
   centroids. Sel world pos comes out (178,309,719) - PASS_X/Y/Z below match
   so the passport-open lerp starts where the item actually sits. */
#ifndef RING_CX2
#define RING_CX2 (-206)
#endif
#ifndef RING_CY2
#define RING_CY2 300
#endif
#ifndef RING_RX
#define RING_RX 948
#endif
#ifndef RING_RZ
#define RING_RZ 1409
#endif
#ifndef RING_TY
#define RING_TY 171
#endif
#ifndef RING_TH0
#define RING_TH0 7
#endif
                      /* THE RING (2026-07-29): three items - 0 Game
                         (passport), 1 Controls, 2 Lara's Home. Sound is
                         deferred. Each row is {selx,sely,selz,selyaw,
                         unx,uny,unz,unyaw}: the SELECTED spot is the front of
                         the ring (whichever item is chosen sits there, as in
                         the original) and the unselected spot is off to the
                         side. Only the selected placement was ever tuned
                         against the reference; the other two spots are the
                         ring positions the original sweeps through. */
                      /* positions come from the dial circle now; these rows
                         carry only the FACING anchors {selyaw, unselyaw}. */
                      static const int16_t mp2[RING_N][8] = {
                        { PASS_X, PASS_Y, PASS_Z, PASS_YAW,  0, 0, 0, 24  },
                        { PASS_X, PASS_Y, PASS_Z, SA_YAW,    0, 0, 0, 24  },
                        { PASS_X, PASS_Y, PASS_Z, SND_YAW,   0, 0, 0, 24  },
                        { PASS_X, PASS_Y, PASS_Z, CTRL_YAW,  0, 0, 0, 24  },
                        { PASS_X, PASS_Y, PASS_Z, 0,         0, 0, 0, 24  },
                      };
                      static const int16_t mrot[RING_N][2] = {   /* pitch, roll */
                        { PASS_PITCH, PASS_ROLL },
                        { SA_PITCH, SA_ROLL },
                        { SND_PITCH, SND_ROLL },
                        { CTRL_PITCH, CTRL_ROLL },
                        { 0, 0 },
                      };
                      int it2, ord2, zi[6], px[6], py[6], pz[6], yw[6], rl[6], pt[6];
                      int rord[RING_N];
                      /* fixed-length smoothstep slew: velocity ramps up AND
                         down (bell curve), no first-frame lurch, no 1-unit
                         tail crawl. s = 3f^2 - 2f^3 in 0..256. */
                      if (ringK < RING_SLEW_T) {
                          int f6 = (++ringK * 256) / RING_SLEW_T;
                          int s6 = (f6 * f6 * (768 - 2*f6)) >> 16;
                          ringA = (ringS + ((ringD * s6) >> 8)) & 4095;
                          if (ringK == RING_SLEW_T) {
                              /* landed: statics froze at their pre-slew spots
                                 during the motion - repaint both buffers so
                                 the whole ring settles onto the new angle */
                              movers[0] = page; nmov = 1;
                              fullpaint = 3;
                          }
                      }
                      spin = (spin + 3) & 1023;   /* reference 11-03-33: ~3s/turn,
                                             direction re-matched */
                      for (it2 = 0; it2 < RING_N; it2++) {
                          /* angular distance of this item from the FRONT, as
                             0 (selected) .. 256 (opposite side of the ring),
                             in 1/16-unit ring precision */
                          int th16 = ((it2*4096)/RING_N - ringA) & 4095;
                          int f  = (th16 < 2048 ? th16 : 4096 - th16) >> 3;
                          /* LAZY SUSAN (reference video 11-03-33, 2026-08-05):
                             the PS1 ring is a FLAT CAROUSEL in depth, not a
                             vertical wheel - all items share world height;
                             perspective alone makes far items smaller and
                             higher on screen (they drift up toward the dial
                             centre exactly as in the reference). Selected sits
                             at the tuned front spot; adjacent spread wide. */
                          int ths = (((it2*4096)/RING_N - ringA + 2048) & 4095) - 2048;
                          int phi = ths + (RING_TH0 << 4);
                          const int16_t *m = mp2[it2];
                          if (f > 256) f = 256;
                          /* constants least-squares FITTED to the measured
                             item centroids of reference 11-03-33 (four
                             points, ~10px residual): big deep carousel,
                             tilted plane (far side higher), selected ~10deg
                             past front. */
                          px[it2] = RING_CX2 + (int)((RING_RX * sin16q(phi)) >> 16);
                          py[it2] = RING_CY2 - (int)((RING_TY *
                                    (65536 - cos16q(phi))) >> 16);
                          pz[it2] = 700 + (int)((RING_RZ *
                                    (65536 - cos16q(phi))) >> 16);
                          /* per-item PERSPECTIVE scale (x256): the bake
                             re-reads ROM verts every frame so staged-vert
                             scaling is dead - nearer pz does it, px/py
                             scaled alike so screen position (and the py/pz
                             camera-tilt ratio) hold. Fitted against the
                             PS1 references: passport 18-36-05 (~27% vs our
                             13%), walkman 22-42-11 (~26% vs our 16.5%). */
                          { static const uint16_t iscale[5] =
                                { 460, 256, 404, 256, 256 };
                            if (iscale[it2] != 256) {
                                px[it2] = (px[it2] * 256) / iscale[it2];
                                py[it2] = (py[it2] * 256) / iscale[it2];
                                pz[it2] = (pz[it2] * 256) / iscale[it2];
                            } }
                          /* the ORIGINAL spins the selected item a full 360
                             clockwise, showing front AND back (user reference
                             2026-08-04). Unselected items hold still. */
                          yw[it2] = (m[3] + ((m[7]-m[3])*f>>8)
                                     + ((f<64) ? spin : 0)) & 1023;
                          zi[it2] = pz[it2];
                          pt[it2] = mrot[it2][0];
                          rl[it2] = mrot[it2][1];
                      }
                      tcam[0]=(uint32_t)(COS(0)>>4); tcam[1]=(uint32_t)(SIN(0)>>4);
                      tcam[2]=(uint32_t)(COS(6)>>4); tcam[3]=(uint32_t)(SIN(6)>>4);
                      tcam[4]=0; tcam[5]=(uint32_t)(-20); tcam[6]=0; tcam[7]=0;
                      gpu_geotex_setclip(0, 319, 0, 239);  /* task #4: 240-line title */
#ifdef RINGBG
                      /* DIAGNOSTIC BACKDROP: bright panel behind the select
                         slot so DROPPED FACES are objectively visible in a
                         capture. Holes over the dial's dark centre are
                         black-on-black and invisible to the capture loop -
                         which is exactly how five "fixed" runs passed the
                         probe and failed the user's eye (2026-08-05). */
                      { int by5, bx5; uint8_t *bf5=(uint8_t *)tfb;
                        for (by5=120; by5<220; by5++)
                            for (bx5=110; bx5<230; bx5++)
                                bf5[by5*RENDER_W+bx5]=6;   /* bright gold */
                      }
#endif
                      if (popen) {
                          /* OPENING / OPEN: the closed booklet is replaced by the
                             opened spread (slot RING_N), swung up to the camera
                             over PASS_OPEN_TICKS ticks. The ring items are not
                             drawn - in the original the spread covers them.
                             Model 71's anim 0 is a SINGLE frame in this data, so
                             the pages cannot spread themselves; the move IS the
                             opening. */
                          int f2 = (popen * 256) / PASS_OPEN_TICKS;
                          if (f2 > 256) f2 = 256;
                          px[RING_N] = PASS_X + ((PASS_OPEN_X - PASS_X)*f2 >> 8);
                          py[RING_N] = PASS_Y + ((PASS_OPEN_Y - PASS_Y)*f2 >> 8);
                          pz[RING_N] = PASS_Z + ((PASS_OPEN_Z - PASS_Z)*f2 >> 8);
                          yw[RING_N] = (PASS_YAW + ((PASS_OPEN_YAW - PASS_YAW)*f2 >> 8)) & 1023;
                          zi[RING_N] = pz[RING_N];
                          /* ease the leaf toward its row, then rewrite its
                             four model verts: piecewise lerp lying-right ->
                             standing -> lying-left about the spine hinge */
                          { int tgt7 = prow * 128, i7, k7;
                            static const int16_t LR[4][3] = {   /* lying right */
                              {4,-60,6},{94,-60,-10},{94,60,-10},{4,60,6} };
                            static const int16_t LU[4][3] = {   /* standing */
                              {0,-60,6},{0,-60,-84},{0,60,-84},{0,60,6} };
                            static const int16_t LL[4][3] = {   /* lying left */
                              {-4,-60,6},{-94,-60,-10},{-94,60,-10},{-4,60,6} };
                            if (panim < tgt7) { panim += 16; if (panim > tgt7) panim = tgt7; }
                            else if (panim > tgt7) { panim -= 16; if (panim < tgt7) panim = tgt7; }
                            for (i7 = 0; i7 < 4; i7++) {
                                const int16_t *pa7; const int16_t *pb7; int t7;
                                if (panim <= 64) { pa7 = LR[i7]; pb7 = LU[i7]; t7 = panim * 4; }
                                else             { pa7 = LU[i7]; pb7 = LL[i7]; t7 = (panim - 64) * 4; }
                                for (k7 = 0; k7 < 3; k7++) {
                                    int v7 = pa7[k7] + (((pb7[k7] - pa7[k7]) * t7) >> 8);
                                    p2src[16 + (8 + i7) * 8 + k7 * 2]     = (uint8_t)(v7 >> 8);
                                    p2src[16 + (8 + i7) * 8 + k7 * 2 + 1] = (uint8_t)v7;
                                } } }
                          rl[RING_N] = PASS_ROLL + ((PASS_OPEN_ROLL - PASS_ROLL)*f2 >> 8);
                          pt[RING_N] = PASS_PITCH + ((PASS_OPEN_PITCH - PASS_PITCH)*f2 >> 8);
                      }
                      /* painter order: farthest first (no depth buffer) */
                      { int a2, b2;
                        for (a2 = 0; a2 < RING_N; a2++) rord[a2] = a2;
                        for (a2 = 1; a2 < RING_N; a2++) {
                            int k2 = rord[a2];
                            for (b2 = a2; b2 > 0 && zi[rord[b2-1]] < zi[k2]; b2--)
                                rord[b2] = rord[b2-1];
                            rord[b2] = k2;
                        } }
                      /* ☠️ PIPELINE RACE (2026-08-04): with PIPELINE=1 Tom can
                         still be READING last frame's rblob[] when the 68k
                         re-bakes this frame's verts into the same buffers -
                         torn tris whose missing set CHANGES per frame (the
                         "carved out" pad; 166 tris = longest bake+read ever
                         staged, so it tore worst). The menu can afford one
                         sync per frame. */
                      { extern int gpu_sync(void); gpu_sync(); }
                      /* Tom idle: restore the art behind this buffer's last
                         movers, and the label band (text redraws below) */
                      if (ringfast) {
                          if (mrx0[ringfbi] <= mrx1[ringfbi])
                              blit_rect(simg, (void *)tfb,
                                        mrx0[ringfbi], mry0[ringfbi],
                                        mrx1[ringfbi] - mrx0[ringfbi] + 1,
                                        mry1[ringfbi] - mry0[ringfbi] + 1);
                          blit_rect(simg, (void *)tfb,
                                    0, RING_LBL_Y0, RENDER_W, RING_LBL_H);
                      }
                      /* this frame's mover screen bbox, accumulated below */
                      { int nbx0 = RENDER_W, nby0 = TITLE_ART_H;
                        int nbx1 = -1, nby1 = -1;
                      for (ord2 = 0; ord2 < (popen ? 1 : RING_N); ord2++) {
                        int ismov;
                        it2 = popen ? RING_N : rord[ord2];
                        { int mi7; ismov = 0;
                          for (mi7 = 0; mi7 < nmov; mi7++)
                              if (movers[mi7] == it2) ismov = 1; }
                        if (!popen && ringfast && !ismov) continue;
                        { /* 2/3 of the elevation: full compensation un-foreshortens
                             flat covers into a STRETCHED look (user) - the
                             PS1 keeps some perspective. */
                          int tl2 = -((27*(py[it2]-20))/pz[it2]);
                          /* the OPEN BOOK is presented flat-frontal (PSX);
                             the elevation tilt is a ring-spot compensation
                             and at the low open position it slews the book
                             ~10 deg into a diamond (silicon capture) */
                          if (popen && it2 == RING_N) tl2 = 0;
                          title_bake(rsrc[it2], rblob[it2], rvcnt[it2],
                                     yw[it2], pt[it2], rl[it2], tl2,
                                     px[it2], py[it2], pz[it2]);
                          /* mover screen extents via the kernel's projection
                             (camera (0,-20,0), pitch 6, focal 190, centre
                             160/120 - the kernel_sim contract). Midpoint
                             verts are averages and never extend the hull,
                             so the real verts suffice. */
                          if (!popen && ismov) {
                              const int16_t *pv7 =
                                  (const int16_t *)(rblob[it2]+16);
                              int v7, cp7 = COS(6), sp7 = SIN(6);
                              for (v7 = 0; v7 < rvcnt[it2]; v7++) {
                                  int X7 = pv7[v7*4+0];
                                  int Y7 = pv7[v7*4+1] - 20;
                                  int Z7 = pv7[v7*4+2];
                                  int Y2 = (Y7*cp7 - Z7*sp7) >> 16;
                                  int Z2 = (Y7*sp7 + Z7*cp7) >> 16;
                                  int sx7, sy7;
                                  if (Z2 < 16) Z2 = 16;
                                  sx7 = 160 + (X7*190)/Z2;
                                  sy7 = 120 + (Y2*190)/Z2;
                                  if (sx7 < nbx0) nbx0 = sx7;
                                  if (sx7 > nbx1) nbx1 = sx7;
                                  if (sy7 < nby0) nby0 = sy7;
                                  if (sy7 > nby1) nby1 = sy7;
                              }
                          }
                          /* midpoint fixup: appended 4th-corner verts =
                             average of their tri's baked v0/v2 (exact -
                             midpoints commute with the affine bake). */
                          { int f4, nvR=rnv0[it2], ntR=rnt0[it2];
                            const uint16_t *tf4=(const uint16_t *)
                                (rsrc[it2]+16+nvR*8+rnq0[it2]*24);
                            int16_t *bv=(int16_t *)(rblob[it2]+16);
                            for (f4=0; f4<ntR; f4++) {
                                int a4=tf4[f4*9+0]*4, c4=tf4[f4*9+2]*4;
                                int16_t *mp=bv+(nvR+f4)*4;
                                mp[0]=(int16_t)((bv[a4+0]+bv[c4+0])>>1);
                                mp[1]=(int16_t)((bv[a4+1]+bv[c4+1])>>1);
                                mp[2]=(int16_t)((bv[a4+2]+bv[c4+2])>>1);
                                mp[3]=255;
                            } }
#ifdef STAGEDIET
                          /* PAINTER'S SORT (2026-08-06): the kernel draws
                             faces in DATA order with no depth test, so any
                             concave or double-sided item shows its far side
                             through its near side at half the spin angles -
                             first seen on the walkman's cups (capture
                             07-47-27), then the sunglasses' arms folding
                             through the lenses. Every staged item is sorted
                             back-to-front each frame now; movers are the
                             only items baked per frame, so the cost stays
                             one or two sorts. Records are uniform QREC here
                             and the permute is PHYSICAL, so between frames
                             of a slow spin the records stay nearly sorted
                             and the insertion pass is close to linear.
                             ONLY the concave/double-sided items (sunglasses,
                             walkman): the passport's near-coplanar covers
                             flickered when noisy centroid keys reordered
                             them (user capture 10-42-56) - approved items
                             keep their authored order. The OPEN book
                             (RING_N) is a fanned multi-leaf: its doubled
                             white page-block face draws LAST in data order
                             and painted over the textured pages - the
                             white slab of captures 12-23-17/12-34-24. */
                          if (it2 == 1 || it2 == 2 || it2 == RING_N) {
                              int nf5 = rnq0[it2] + rnt0[it2];
                              static uint8_t fscr[160*QREC];
                              static int16_t fkey[160];
                              static uint8_t ford[160];
                              if (nf5 <= 160) {
                                  uint8_t *fb5 = rblob[it2] + 16 +
                                                 (rnv0[it2]+rnt0[it2])*8;
                                  const int16_t *bv5 =
                                      (const int16_t *)(rblob[it2]+16);
                                  int f5, g5;
                                  for (f5 = 0; f5 < nf5; f5++) {
                                      const uint16_t *fw5 = (const uint16_t *)
                                          (fb5 + f5*QREC) + LPLANE_PREFIX/2;
                                      fkey[f5] = (int16_t)
                                          ((bv5[fw5[0]*4+2] + bv5[fw5[1]*4+2] +
                                            bv5[fw5[2]*4+2] + bv5[fw5[3]*4+2]) >> 2);
                                      ford[f5] = (uint8_t)f5;
                                  }
                                  for (f5 = 1; f5 < nf5; f5++) {
                                      uint8_t k5 = ford[f5];
                                      int16_t kk5 = fkey[k5];
                                      for (g5 = f5; g5 > 0 &&
                                           fkey[ford[g5-1]] < kk5; g5--)
                                          ford[g5] = ford[g5-1];
                                      ford[g5] = k5;
                                  }
                                  for (f5 = 0; f5 < nf5; f5++) {
                                      const uint16_t *sp5 = (const uint16_t *)
                                          (fb5 + ford[f5]*QREC);
                                      uint16_t *dp5 = (uint16_t *)
                                          (fscr + f5*QREC);
                                      for (g5 = 0; g5 < QREC/2; g5++)
                                          dp5[g5] = sp5[g5];
                                  }
                                  { uint16_t *d5 = (uint16_t *)fb5;
                                    const uint16_t *s5 = (const uint16_t *)fscr;
                                    for (f5 = 0; f5 < nf5*(QREC/2); f5++)
                                        d5[f5] = s5[f5]; }
                              }
                          }
#endif
                          gpu_geotex(rblob[it2], tfb, tcam, ratl[it2], 256u);
                        }
                      }
                      /* remember where this buffer's movers landed (pad for
                         interpolation slack); restored before next reuse */
                      if (!popen) {
                          if (nbx1 >= nbx0) {
                              /* 16px: the 68k-side projection is the
                                 kernel_sim approximation of the kernel's -
                                 8px left a white ghost sliver at the
                                 polaroid's bottom edge (user capture
                                 11-04-11) */
                              nbx0 -= 16; nby0 -= 16; nbx1 += 16; nby1 += 16;
                              if (nbx0 < 0) nbx0 = 0;
                              if (nby0 < 0) nby0 = 0;
                              if (nbx1 >= RENDER_W)     nbx1 = RENDER_W - 1;
                              if (nby1 >= TITLE_ART_H)  nby1 = TITLE_ART_H - 1;
                              mrx0[ringfbi] = nbx0; mry0[ringfbi] = nby0;
                              mrx1[ringfbi] = nbx1; mry1[ringfbi] = nby1;
                          } else {
                              mrx0[ringfbi] = 1; mrx1[ringfbi] = 0;
                          }
                      } }
                  }
                  /* RING LABEL + SELECT PROMPT (reference 2026-08-04): the
                     original shows the item's name bottom-centre and a
                     "Select" prompt bottom-left. Jaguar wording, TR font. */
                  if (popen >= PASS_OPEN_TICKS) {
                      /* PSX rows (user 2026-08-06): the photo spread is
                         the LOAD GAME page (not implemented yet - A is a
                         no-op there), the plain spread is START GAME. B
                         closes, hinted bottom-right mirroring A Select. */
                      static const char *const PROW[2] = { "Load Game", "Start Game" };
                      const char *lb = PROW[prow];
                      int ln = 0; while (lb[ln]) ln++;
                      menu_text((fbpix *)tfb, RENDER_W, 240,
                                lb, (320 - ln*8)/2, 200, 2, 2, 0);
                      menu_text((fbpix *)tfb, RENDER_W, 240,
                                "A Select", 10, 200, 2, 2, 0);
                      menu_text((fbpix *)tfb, RENDER_W, 240,
                                "B Back", 320 - 10 - 6*8, 200, 2, 2, 0);
                  }
                  if (!copen && !sopen && !popen) {
                      static const char *const RLBL[5] =
                          { "Game", "Screen Adjust", "Sound", "Controls",
                            "Laras Home" };
                      /* the '<3' clamp was the 3-item ring's - it made pages 3/4 label
                         themselves "Game" and masked a working TITLESEL for two
                         whole diagnostic loops (2026-08-04). */
                      const char *lb = RLBL[page];
                      int ln = 0; while (lb[ln]) ln++;
                      menu_text((fbpix *)tfb, RENDER_W, 240,
                                lb, (320 - ln*8)/2, 200, 2, 2, 0);   /* 213 collided with the art's baked TM line */
                      menu_text((fbpix *)tfb, RENDER_W, 240,
                                "A Select", 10, 200, 2, 2, 0);
                  }
                  /* CONTROLS PAGE (2026-08-01) - an OVERLAY on the dimmed
                     title, which is exactly what the original does: the logo,
                     the ring and Lara's face all stay visible behind it. Drawn
                     LAST so it sits over the art and the 3D ring items.
                     Layout copied from the reference at t=250 in
                     res/...Part 1 (see REFERENCE_VIDEOS.md): a centred header
                     with arrows, then rows whose LEFT label is right-aligned
                     and RIGHT label left-aligned, and the shared
                     Select / Go Back footer. The PS1 pad glyphs are
                     deliberately NOT drawn - this machine has a Jaguar pad
                     (task #6), and inventing PS1 shapes here would be wrong. */
                  if (copen) {
                      volatile uint16_t *cl = (volatile uint16_t *)0xF00400u;
                      const int LHc = RENDER_H;
#if defined(LOWRES) && !defined(HALFRES)
                      const int DVY = 2;
#else
                      const int DVY = 1;
#endif
                      /* DIV=2: the glyphs are authored 16 px wide for a screen
                         where that is a SMALL font. Measured against the
                         reference, the original's Controls text is ~10 px wide
                         in a 320-wide buffer; drawing ours at native 16 made
                         "Control Method 1" span the whole screen. Halve it. */
                      const int DVX = 2;
                      static const char *const CROW_L[] = {
                          "Step Left", "Look", "Draw Weapon", "Jump", "" };
                      static const char *const CROW_R[] = {
                          "Step Right", "Walk", "", "Roll", "Action" };
                      int ci, ty, mid = RENDER_W / 2;
                      int gh = 16 / DVY;             /* drawn glyph height */
                      uint8_t *cfb = (uint8_t *)tfb;
                      cl[254] = 0x0000;              /* reserved black */
                      cl[255] = 0xFA34;              /* TR gold */
                      menu_dim(cfb, RENDER_W, LHc, 254);
                      ty = gh / 2;
                      menu_text(cfb, RENDER_W, LHc, "Control Method 1",
                                mid - menu_text_width("Control Method 1", DVX) / 2,
                                ty, DVX, DVY, 255);
                      ty += gh + gh / 2;
                      for (ci = 0; ci < 5; ci++) {
                          if (CROW_L[ci][0])
                              menu_text(cfb, RENDER_W, LHc, CROW_L[ci],
                                        mid - 6 - menu_text_width(CROW_L[ci], DVX),
                                        ty, DVX, DVY, 255);
                          if (CROW_R[ci][0])
                              menu_text(cfb, RENDER_W, LHc, CROW_R[ci],
                                        mid + 6, ty, DVX, DVY, 255);
                          ty += gh + 2;
                      }
                      /* footer: Select / Go Back on one row, the page title
                         under it - both measured UP from the bottom so they
                         cannot clip off the panel (the first cut "Controls"
                         in half). */
                      menu_text(cfb, RENDER_W, LHc, "Select", 6,
                                LHc - 2 * gh - 4, DVX, DVY, 255);
                      menu_text(cfb, RENDER_W, LHc, "Go Back",
                                RENDER_W - 6 - menu_text_width("Go Back", DVX),
                                LHc - 2 * gh - 4, DVX, DVY, 255);
                      menu_text(cfb, RENDER_W, LHc, "Controls",
                                mid - menu_text_width("Controls", DVX) / 2,
                                LHc - gh - 2, DVX, DVY, 255);
                  }
                  if (sopen) {
                      volatile uint16_t *cl = (volatile uint16_t *)0xF00400u;
                      const int LHs = RENDER_H;
#if defined(LOWRES) && !defined(HALFRES)
                      const int DVY = 2;
#else
                      const int DVY = 1;
#endif
                      const int DVX = 2;
                      int mid = RENDER_W / 2, gh = 16 / DVY, ty, ri, bi;
                      uint8_t *cfb = (uint8_t *)tfb;
                      cl[254] = 0x0000; cl[255] = 0xFA34;   /* black / TR gold */
                      menu_dim(cfb, RENDER_W, LHs, 254);
                      ty = gh / 2;
                      menu_text(cfb, RENDER_W, LHs, "Sound",
                                mid - menu_text_width("Sound", DVX) / 2,
                                ty, DVX, DVY, 255);
                      ty += gh + gh / 2;
                      for (ri = 0; ri < 2; ri++) {
                          const char *lb2 = ri ? "Effects" : "Music";
                          int vol = ri ? g_sfxvol : g_musvol;
                          int bx = mid - 20, by = ty + 2, yy2, xx2;
                          menu_text(cfb, RENDER_W, LHs, lb2,
                                    mid - 30 - menu_text_width(lb2, DVX),
                                    ty, DVX, DVY, 255);
                          if (ri == srow)      /* selection arrow */
                              menu_text(cfb, RENDER_W, LHs, ">",
                                        mid - 42 - menu_text_width(lb2, DVX),
                                        ty, DVX, DVY, 255);
                          /* 10-cell slider bar, filled to vol */
                          for (bi = 0; bi < 10; bi++)
                              for (yy2 = 0; yy2 < gh - 2; yy2++)
                                  for (xx2 = 0; xx2 < 5; xx2++)
                                      if (bi < vol || yy2 == 0 || yy2 == gh-3)
                                          cfb[(by + yy2) * RENDER_W
                                              + bx + bi * 7 + xx2] = 255;
                          ty += gh + gh / 2;
                      }
                      menu_text(cfb, RENDER_W, LHs, "Go Back",
                                RENDER_W - 6 - menu_text_width("Go Back", DVX),
                                LHs - 2 * gh - 4, DVX, DVY, 255);
                  }
                  /* PACE to ~10fps (2026-08-06): the dirty-rect renderer let
                     fast frames through at 12-15fps, which RAISED total
                     Tom+Blitter bus throughput per second and starved the
                     DSP's sample fetches - the user heard it as choppy
                     music. 6 fields = a 100ms floor; slow frames (slews,
                     landing repaints) already exceed it and pay nothing.
                     BOUNDED: frame_count only advances if the vblank ISR
                     lives (the A10 failure mode), so cap the spin. */
                  { static uint32_t pacef = 0;
                    uint32_t pg = 0;
                    while (frame_count - pacef < 6 && ++pg < 400000u) ;
                    pacef = frame_count; }
                  video_flip();
                  video_wait_vblank();
              }
#ifdef AUTOSTART
              /* headless HW/emulator profiling: auto-select New Game after a
                 short delay so no physical A press is needed to reach the level */
              { static int _as_ctr = 0; if (++_as_ctr > 20) { g_useset = 0; page = 0; break; } }
#endif
#ifdef ASVID
              /* Tom campaign self-test (2026-08-07): take the EXACT Start
                 Game path (intro clip -> game) with no pad press - the
                 menu-exit context is where kernel loads provably work, so
                 this is experiment #1 running itself. */
              { static int _av = 0; if (++_av > 20) {
#ifndef NO_GAMEDRIVE
                    if (mh >= 0) { gd_fclose((unsigned)mh); mh = -1; }
#endif
                    introplay = 1; goto bootvid_entry; } }
#endif
#ifdef CTRLOPEN
              /* CTRLOPEN=N: open the page after N title frames. Deliberately
                 NOT an initial value - a build that starts in the page cannot
                 tell "the page hangs" from "this binary lost the A10 layout
                 lottery", because it never renders a good frame either way.
                 Opening it late makes the transition itself the evidence. */
              { static int _co = 0; if (++_co == (CTRLOPEN) && !copen) copen = 1; }
#endif
#ifdef PASSOPEN
              /* PASSOPEN=N: rig-only - open the passport after N title
                 frames so the open-book pose can be captured hands-off
                 (same rationale as CTRLOPEN above). */
              { static int _po = 0;
                if (++_po == (PASSOPEN) && !popen) { popen = 1; prow = 1; panim = 128; } }
#endif
#ifdef PASSSWEEP
              /* rig-only: while the book is open, step g_psweep every 30
                 frames; the draw code maps it to a pose candidate and the
                 index is drawn on screen so captures self-identify. */
              { static int _ps = 0;
                if (popen && ++_ps >= 30) { _ps = 0; prow ^= 1; } }
#endif
#ifdef PASSTART
              /* rig-only: N frames after the book is open, press A - with
                 the open default on the Start Game row this drives the
                 full start flow (intro video, loading art, level) for
                 hands-off capture. */
              { static int _pa = 0;
                if (popen >= PASS_OPEN_TICKS && _pa < (PASSTART) &&
                    ++_pa == (PASSTART)) edge |= PAD_A; }
#endif
              if (copen) {
                  /* Controls page: B returns to the ring, as "Go Back" says. */
                  if (edge & PAD_B) { copen = 0; sfx_play(1, SFX_MENU_SPIN); }
              }
              else if (sopen) {
                  int *vp = srow ? &g_sfxvol : &g_musvol;
                  if (edge & PAD_B) { sopen = 0; sfx_play(1, SFX_MENU_SPIN); }
                  else if (edge & PAD_UP)   { srow = 0; }
                  else if (edge & PAD_DOWN) { srow = 1; }
                  else if ((edge & PAD_LEFT)  && *vp > 0)  { (*vp)--;
                      sfx_play(1, SFX_MENU_SPIN); }
                  else if ((edge & PAD_RIGHT) && *vp < 10) {
                      if (*vp == 0 && !srow) mkick = 1;   /* music unmute */
                      (*vp)++;
                      sfx_play(1, SFX_MENU_SPIN); }
              }
              else if (popen && popen < PASS_OPEN_TICKS) popen++;   /* run the opening */
              if (!copen && popen) {
                  /* PAGE VIEW (PS1 ref 22-28-59): rows navigated LEFT/RIGHT
                     with the row label bottom-centre - Start Game / Go Back
                     (no fake Load Game: there is no save system yet). A
                     activates the row, B always closes. */
                  if (edge & PAD_B) { popen = 0; sfx_play(1, SFX_MENU_SPIN); }
                  else if ((edge & (PAD_LEFT | PAD_RIGHT)) &&
                           popen >= PASS_OPEN_TICKS) {
                      prow ^= 1; sfx_play(1, SFX_MENU_SPIN);
                  }
                  else if ((edge & PAD_A) && popen >= PASS_OPEN_TICKS) {
                      if (prow == 1) {
                          g_useset = 0; sfx_play(1, SFX_MENU_SHOW);
#if !defined(NO_GAMEDRIVE) && defined(BOOTVID)
                          /* the caves intro cinematic plays between the
                             menu and the level, like the PSX (user
                             2026-08-06). A skips it, as at boot.
                             ☠️ CLOSE THE MUSIC STREAM FIRST: the GD BIOS
                             serves ONE open file - with MUSIC.PCM still
                             open, the video open/reads fail silently and
                             the flow skips straight to the level (the
                             'video never plays' of 16:0x). Same law for
                             the loading art below. */
                          if (mh >= 0) { gd_fclose((unsigned)mh); mh = -1; }
                          introplay = 1; goto bootvid_entry;
#else
                          break;
#endif
                      }
                      else sfx_play(1, SFX_MENU_SPIN);   /* Load Game: no
                                 saves yet - acknowledge, stay open */
                  }
              }
              else if (!copen && !sopen && (edge & (PAD_LEFT|PAD_RIGHT))) {
                  { int prevp = page, mi6, have6;
                    page = (edge & PAD_RIGHT) ? (page + 1) : (page + RING_N - 1);
                    if (page >= RING_N) page -= RING_N;
                    /* retarget the slew from wherever the ring is NOW, short
                       way round; a tap mid-flight restarts the curve there */
                    { int dd6 = ((((page * 4096) / RING_N) - ringA + 2048)
                                 & 4095) - 2048;
                      if (dd6) { ringS = ringA; ringD = dd6; ringK = 0; } }
                    /* mover set: the outgoing front joins the incoming one.
                       Mid-flight taps accumulate (an item mid-arc must keep
                       animating or it would vanish); overflow falls back to
                       full repaints for the whole slew. */
                    if (nmov == 1 && movers[0] == prevp) {
                        movers[0] = prevp; movers[1] = page; nmov = 2;
                    } else {
                        have6 = 0;
                        for (mi6 = 0; mi6 < nmov; mi6++)
                            if (movers[mi6] == page) have6 = 1;
                        if (!have6) {
                            if (nmov < 4) movers[nmov++] = page;
                            else fullpaint = RING_SLEW_T;
                        }
                    } }
                  spin = 0;   /* the newly selected item greets FACE-ON,
                                 then starts its turn (reference behavior) */
                  sfx_play(1, SFX_MENU_SPIN);
              }
              else if (!copen && !sopen && (edge & PAD_A)) {
                  if (page == 0) { popen = 1; prow = 1; panim = 128; /* opens on START GAME */
                                   sfx_play(1, SFX_MENU_SHOW); }
                  else if (page == 1) { /* Screen Adjust: page later */
                                        sfx_play(1, SFX_MENU_SHOW); }
                  else if (page == 2) { sopen = 1; srow = 0;   /* Sound page */
                                        sfx_play(1, SFX_MENU_SHOW); }
                  else if (page == 3) { copen = 1;     /* Controls */
                                        sfx_play(1, SFX_MENU_SHOW); }
                  else if (page == 4) { g_useset = 1;  /* Lara's Home */
                                        sfx_play(1, SFX_MENU_SHOW); break; }
              }
          }
#if !defined(NO_GAMEDRIVE) && defined(BOOTVID)
          bv_done: ;                   /* Start Game lands here after the intro */
#endif
#ifndef NO_GAMEDRIVE
          /* one-open-file law: the loading art streams next */
          if (mh >= 0) { gd_fclose((unsigned)mh); mh = -1; }
#endif
          /* task #4: leave TITLE mode - game kernel + scaled 120-line display */
          { extern void video_set_disp240(int); extern void gpu_kernel_select(int);
            video_set_disp240(0); gpu_kernel_select(0); }
#ifdef VIDDIAG
/* CONTEXT PROBE v3 (lean): ONLY the SRAM reliability test -
             v2's kernel/display experiments wedged the entry. Bounded
             ops, no kernel touches, no mode flips. */
          { extern int gpu_sync(void);
            extern uint32_t gpu_sram_test(void);
            gpu_sync();
            g_sr[3] = gpu_sram_test();
            g_jvmin_game = 3; }   /* 3 = lean probe ran (no J run/fail) */
#endif
#if defined(GYMTEST) || defined(CAVETEST)
          menu_done:
          { volatile uint32_t *v0 = (volatile uint32_t *)0xF1C340u;
            volatile uint32_t *nq = (volatile uint32_t *)0xF1C378u;
            *nq = 0; v0[0] = 0; *nq = 0; v0[0] = 0; }  /* stop music+queue */
          CRUMB(0x003E);               /* GREEN: menu_done DSP stores ok */
#ifndef NO_GAMEDRIVE
          if (mh >= 0) { gd_fclose((unsigned)mh); mh = -1; }
#endif
#endif
        }

        /* LOADING PANEL: shown immediately on selection, covers the whole
           room-parse + pose-setup gap before the first game frame. Direct
           CLUT pokes (254=black,255=white) so it works whatever palette the
           menu left; the level's video_set_clut later resets 0..253 and
           re-pokes 254/255 to the same values, so no flash. */
        { volatile uint16_t *clut=(volatile uint16_t*)0xF00400u;
          extern const uint8_t font_load[];
          int n=(font_load[0]<<8)|font_load[1];
          int palOff=(font_load[2]<<8)|font_load[3];
          const int DIV=1;             /* native glyphs at 240 lines = small+sharp */
          /* DIVY (2026-07-25): VERTICAL decimation only. The glyphs are
             authored for a 240-line screen. Under LOWRES the panel is painted
             into the 120-line render buffer and the OP scaler doubles it, so
             drawing at native height made the word 2x too TALL. Sample every
             other glyph row (full width) and the scaler restores the correct
             aspect — the same trick the title art uses. HALFRES paints at a
             real 240 via the _hi path, and the full-res build is 240 natively,
             so both keep DIVY=1. */
#if defined(LOWRES) && !defined(HALFRES)
          const int DIVY=2;
#else
          const int DIVY=1;
#endif
          int totw=0, gi, px2, py2, gx, gy, sx2, sy2, ox, oy;
#ifdef HALFRES
          uint8_t *lfb=(uint8_t*)video_backbuffer_hi();
          const int LH=DISPLAY_H;      /* panel painted at native 240 */
#else
          uint8_t *lfb=(uint8_t*)video_backbuffer();
          const int LH=RENDER_H;
#endif
          /* gold-font mini palette -> CLUT 224..239 (level set_clut later
             overwrites; the panel has flipped away by then) */
          (void)palOff;                /* flat white text (user pref) */
          clut[255]=0xFFFE;
          clut[254]=0x0000;
          /* PS1 LOADING ART (user 2026-08-06, ref 15-40-02): the disc's
             region loading screens - AZTECLOA (Lara at the carved cave
             mouth, Peru) for the Caves, GYMLOAD for Lara's Home. The
             76800-byte images STREAM from SD (ROM had no 77KB left; the
             bss guard tripped) in 15360B chunks (48 rows each, 512-
             aligned), row-decimated into the panel buffer. Palettes are
             resident. Any failure falls through to the text panel. */
#if !defined(NO_GAMEDRIVE) && !defined(FASTBOOT)
          { extern const uint16_t cavesload_pal[], gymload_pal[];
            const uint16_t *lp = g_useset ? gymload_pal : cavesload_pal;
            /* 24 rows a chunk, not 48: this buffer is BSS that exists only
               while the loading art streams, and 7680 is still ten whole
               512-byte sectors, so the GD sector discipline is untouched.
               Halving it bought back the headroom that let DBGROOM build -
               the stack guard was 96 bytes short across every pad. */
            uint8_t *artbuf = g_artbuf;   /* file-scope: 24-row chunks, ten sectors */
            int lh2 = -1, mi8, k8, ck;
            for (mi8 = 0; mi8 < 2 && lh2 < 0; mi8++)
                lh2 = gd_fopen(g_useset ? (mi8 ? "/GYMLOAD.DAT" : "GYMLOAD.DAT")
                                        : (mi8 ? "/CAVSLOAD.DAT" : "CAVSLOAD.DAT"),
                               GD_FOPEN_READ | GD_FOPEN_OPEN_EXISTING);
            if (lh2 >= 0) {
                int okart = 1;
                /* size-before-read, like every working GD reader */
                if (gd_fsize((unsigned)lh2) != 76800) okart = 0;
                for (ck = 0; ck < 10 && okart; ck++) {
                    if (gd_fread((unsigned)lh2, artbuf, 7680u,
                                 GD_FREAD_CPU) != 0) { okart = 0;
                        break; }
                    for (py2 = 0; py2 < 24; py2++) {
                        int sy8 = ck*24 + py2;
                        int dy8 = sy8 * LH / 240;
                        const uint8_t *sr = artbuf + py2*320;
                        /* ☠️ THE 68000 WAS CARRYING THE PICTURE.  76,800 bytes
                           a load, one byte per instruction - and with LH=120
                           every second source row is immediately overwritten
                           by the next, so half of it was wasted as well.
                           Skip the doomed rows and let the Blitter move the
                           rest; blit_bytes is the silicon-verified phrase
                           mover and one row is exactly its 320-byte unit. */
                        if (LH < 240 && (sy8 & 1)) continue;
                        if (!blit_bytes(sr, lfb + dy8*RENDER_W, RENDER_W))
                            for (px2 = 0; px2 < RENDER_W; px2++)
                                lfb[dy8*RENDER_W+px2] = sr[px2];
                    }
                }
                gd_fclose((unsigned)lh2);
                if (okart) {
                    for (k8 = 0; k8 < 254; k8++) clut[k8] = lp[k8];
                    goto load_art_done;
                }
            } }
#endif
          /* glyph cells are 16 wide but the INK varies (8..15 cols, left-
             aligned) — advance by ink width + a constant gap so the word
             is evenly spaced. Two passes: measure, then draw. */
          /* LOOK GLYPHS UP BY CHARACTER (2026-08-01). This used to draw glyph
             records 0..n in file order, which worked only because the blob was
             built with FONT_CHARS="LOADING..." and so WAS the string. The blob
             now carries a full alphabet (A-Z, a-z, 0-9, punctuation) so the
             menus can use it, and drawing it in file order would print
             "ABCDEFGHIJKLMNOP". Resolve each character instead; a missing glyph
             is skipped rather than drawn as garbage. */
          { static const char LOADSTR[]="LOADING...";
            const uint8_t *grec[16]; int ng=0;
            for (gi=0; LOADSTR[gi] && ng<16; gi++) {
                int k; for (k=0; k<n; k++)
                    if (font_load[4+k*8]==(uint8_t)LOADSTR[gi]) {
                        grec[ng++]=font_load+4+k*8; break; }
            }
            n=ng;
          { int inkw[16];
            totw=0;
            for (gi=0; gi<n && gi<16; gi++) {
                const uint8_t *rec=grec[gi];
                int w=rec[1], h=rec[2], last=0;
                uint32_t po=((uint32_t)rec[4]<<24)|((uint32_t)rec[5]<<16)
                           |((uint32_t)rec[6]<<8)|rec[7];
                for (gy=0; gy<h; gy+=DIV)
                  for (gx=0; gx<w; gx+=DIV)
                      if (font_load[po+gy*w+gx] && gx/DIV+1 > last) last = gx/DIV+1;
                inkw[gi]=last ? last : 4;
                totw += inkw[gi] + 2;
            }
            totw -= 2;
            ox=RENDER_W-totw-10; oy=LH-16/DIVY-8/DIVY;   /* lower-right corner */
            for (gi=0; gi<n && gi<16; gi++) {
                const uint8_t *rec=grec[gi];
                int w=rec[1], h=rec[2];
                /* rec[3] is the glyph's TOP relative to the text baseline,
                   baked by tr2jag_font.py from the sprite record. Do NOT
                   recompute it as (16-h): every glyph shares the same top and
                   descenders are simply TALLER (p/g/y are 16x24 where a/e/o are
                   16x16), so bottom-aligning lifts every descender 8 px and
                   "Step Left" renders with a superscript p. Old blobs stored 0
                   here, which is the correct value for all the 16-tall glyphs
                   they contained. */
                int yoff=(int)rec[3]/DIVY;
                uint32_t po=((uint32_t)rec[4]<<24)|((uint32_t)rec[5]<<16)
                           |((uint32_t)rec[6]<<8)|rec[7];
                for (gy=0; gy<h/DIVY; gy++)
                  for (gx=0; gx<inkw[gi]; gx++) {
                      int v=font_load[po+gy*DIVY*w+gx*DIV];
                      int X=ox+gx, Y=oy+yoff+gy;
                      if (v && X>=0&&X<RENDER_W&&Y>=0&&Y<LH)
                          lfb[Y*RENDER_W+X]=255;   /* flat white */
                  }
                ox += inkw[gi] + 2;
            } } }
          load_art_done: ;
          /* PROGRESS BAR (2026-07-25, user request): an empty outlined bar
             along the bottom. It is filled by load_prog() as the load runs.
             Geometry is derived from LH so it is correct in all three display
             configs (LOWRES 120 + OP scaler, HALFRES 240 via _hi, full 240). */
          /* Height matters more than it looks: the frame costs 2 rows, so a
             3-row bar at LH=120 left ONE row of fill and read as a solid line
             whatever the progress was.  8 rows (LOWRES) / 14 (240) leaves a
             readable interior in both. */
          /* SAME BAR AS THE TITLE SPLASH (user, 2026-08-09: "can we get the
             same progress bar from the game?").  The disc draws no outline at
             all - just a solid red bar growing left to right - so the white
             box-and-caps is gone and the geometry is the title's, scaled from
             the 240-line art space into whatever height this panel is
             painted at (LOWRES paints 120 and the OP scaler doubles it).
             The cave art has NO red anywhere near the disc's rgb(157,16,1) -
             its reddest entry is rgb(8,0,0) - but indices 250..253 are unused
             by both loading images, so 253 is reserved for the bar and poked
             straight into the CLUT after the art's palette lands. */
          { int by = TL_Y0 * LH / 240, bh = (TL_Y1 - TL_Y0) * LH / 240;
            int bx0 = TL_X0, bx1 = TL_X1;
            volatile uint16_t *clut2 = (volatile uint16_t *)0xF00400u;
            if (bh < 3) bh = 3;                /* stays legible at LH=120 */
            clut2[LOADBAR_IDX] = LOADBAR_RGB;
            g_loadfb = lfb; g_loadlh = LH;
            g_loadbx0 = bx0; g_loadbx1 = bx1; g_loadby = by; g_loadbh = bh;
            (void)py2; }
          CRUMB(0xFFFE);               /* WHITE: panel painted */
#ifdef HALFRES
          video_flip_hi(lfb);
#else
          video_flip();
#endif
          CRUMB(0xFFC0);               /* MAGENTA(r+b): flip queued */
          video_wait_vblank();
          CRUMB(0x07FE);               /* CYAN(b+g): vblank arrived */
        }


        if (g_useset) {
            S_index=gym_index; S_geom=gym_geom; S_sect=gym_sect;
            S_atlas=(uint8_t*)gym_atlas; S_pal=gym_pal;
            S_lara=gym_lara; S_lskin=gym_lskin; S_adj=gym_adjgen;
            S_portalv=gym_portalv; S_portal_ofs=gym_portal_ofs;
        } else {
            S_index=mrt_index; S_geom=mrt_geom; S_sect=mrt_sect;
            S_atlas=(uint8_t*)mrt_atlas; S_pal=mrt_pal;
            S_lara=mrt_lara_; S_lskin=mrt_lskin_; S_adj=mrt_adjgen;
            S_portalv=mrt_portalv; S_portal_ofs=mrt_portal_ofs;
        }
        roomCount = (S_index[0]<<8)|S_index[1];
        atlasW = (S_index[2]<<8)|S_index[3];
        g_swy = (((S_index[4]<<8)|S_index[5])) - 2*MRT_LARA_CELL;
        /* pick two palette slots this set doesn't use (entry == 0, idx > 0) */
        { int i2; g_pickidx = g_dooridx = 0;
          /* Slots 240/241 are RESERVED by the extractor for these pokes.
             (The old "find a zero entry" scan stole LEGITIMATE BLACKS that
             textures referenced: Lara's head 2026-07-12, then her shorts
             turned pickup-gold in the mansion 2026-07-13. Never guess.) */
          
          g_pickidx = 240;
          g_dooridx = 241; }
        int i;
        uint32_t fc = 0;
#ifdef PROFILE
        extern volatile uint32_t frame_count;   /* VI-ISR vblank clock (60Hz) */
        uint32_t pf68=0,pfrm=0,pfla=0,pftt=0,pfn=0,pfA=0,pfB=0,pfC=0,pfD=0;

        uint32_t g_gpuC=0, acc_gpu_rooms=0, acc_gpu_lara=0, prevtot=0;
        int bar68=0,barrm=0,barla=0,barwt=0,barfps=0;   /* on-screen profile bars (px) */
#ifdef ASSETSUM
        static uint32_t g_sum1=0,g_sum2=0;  /* asset sums, redrawn as bit-cells */
        static uint32_t g_blt=0;            /* Blitter self-test result sum */
        static uint8_t  g_bltbuf[640] __attribute__((aligned(8)));
#endif
#endif
        static uint8_t rwater[64];           /* per-room WATER flag */
        if (roomCount > 64) roomCount = 64;
        for (i = 0; i < roomCount; i++) {
            const uint8_t *e = S_index + 8 + i*8;
            uint32_t goff = ((uint32_t)e[0]<<24)|((uint32_t)e[1]<<16)|((uint32_t)e[2]<<8)|e[3];
            rwater[i] = (e[4] & 0x80) ? 1 : 0;   /* soff bit31 = WATER room */
            uint32_t soff = (((uint32_t)e[4]&0x7F)<<24)|((uint32_t)e[5]<<16)|((uint32_t)e[6]<<8)|e[7];
            const uint8_t *sp;
            rgeom[i] = S_geom + goff;
            rsect[i] = S_sect + soff;
            sp = rsect[i];
            { int xS=(sp[0]<<8)|sp[1], zS=(sp[2]<<8)|sp[3];
              int ix=(int)(((uint32_t)sp[4]<<24)|((uint32_t)sp[5]<<16)|((uint32_t)sp[6]<<8)|sp[7]);
              int iz=(int)(((uint32_t)sp[8]<<24)|((uint32_t)sp[9]<<16)|((uint32_t)sp[10]<<8)|sp[11]);
              /* room bounding radius (half-diagonal) so the cull is CONSERVATIVE:
                 a room only culls when ENTIRELY behind/beyond, never when you're
                 standing in it or it's partly in view (that caused black bg). */
              /* fast integer isqrt (bit-by-bit), 32-BIT form (jcc68k has no
                 64-bit long long): half-diagonal at 1/4 scale — h/4 <= 16384
                 for rooms up to 128 sectors, so (h/4)^2*2 fits uint32 — then
                 scale the root back (+4 keeps the radius CONSERVATIVE: too
                 big never culls a visible room). Replaces an O(sqrt) linear
                 scan (r reached ~70k per room) that dominated level-load. */
              uint32_t h4x=(uint32_t)xS*128u, h4z=(uint32_t)zS*128u;
              uint32_t r2=h4x*h4x+h4z*h4z;
              uint32_t root=0, bit=1u<<30;
              while (bit>r2) bit>>=2;
              while (bit){ if (r2>=root+bit){ r2-=root+bit; root=(root>>1)+bit; }
                          else root>>=1; bit>>=2; }
              int r=(int)(root*4u+4u); if (r<1) r=1;
              rcx[i] = ix + xS*512; rcz[i] = iz + zS*512; rrad[i] = r; }
            /* room table = the first 3/8 of the load, one step per room */
            load_prog(3*(i+1), 8*roomCount);
        }
        load_prog(3, 8);
        /* Lara mesh + start in room 0 (first in the set), on the floor */
        /* TEXTURED Lara from PSX (mrt_lara.bin): baked per-face UVs + posed
         * run-cycle frames, sharing the room atlas/palette. */
        { const uint8_t *mrt_lara = S_lara;
          g_lnv = (mrt_lara[0]<<8)|mrt_lara[1];
          g_lnq = (mrt_lara[2]<<8)|mrt_lara[3];
          g_lnt = (mrt_lara[4]<<8)|mrt_lara[5];
          g_lnframes = (mrt_lara[6]<<8)|mrt_lara[7];
          g_ltx_atH  = (mrt_lara[10]<<8)|mrt_lara[11];
          if (g_lnframes < 1) g_lnframes = 1;
          g_ltx_quads  = mrt_lara + 12;
          g_ltx_tris   = g_ltx_quads + g_lnq*24;
          g_ltx_frames = g_ltx_tris + g_lnt*18;
        }
        load_prog(5, 8);
        /* runtime-skinning skeleton (mrt_lskin.bin) */
        { const uint8_t *b = S_lskin;
          int mc = rd16(b), animc = rd16(b+8), framec = rd16(b+10);
          g_sk_mcount = mc; g_sk_framestride = rd16(b+12);
          g_sk_animcount = animc; g_sk_framecount = framec;
          g_sk_meshinfo  = b + 16;                       /* mcount*4          */
          g_sk_meshverts = g_sk_meshinfo  + mc*4;        /* vcount*6          */
          g_sk_nodes     = g_sk_meshverts + g_lnv*6;     /* (mcount-1)*8      */
          g_sk_frames    = g_sk_nodes     + (mc-1)*8;    /* framec*stride     */
          g_sk_anims     = g_sk_frames    + framec*g_sk_framestride;
          /* map each face to its mesh (by first vert index) for the depth sort */
          { int q,t,mm; const uint8_t *qb=g_ltx_quads, *tb=g_ltx_tris;
            for (q=0;q<g_lnq;q++){ int v0=rd16(qb+q*24);
              for (mm=0;mm<mc;mm++){ int vb=rd16(g_sk_meshinfo+mm*4), vl=rd16(g_sk_meshinfo+mm*4+2);
                if (v0>=vb && v0<vb+vl){ g_qmesh[q]=(uint8_t)mm; break; } } }
            for (t=0;t<g_lnt;t++){ int v0=rd16(tb+t*18);
              for (mm=0;mm<mc;mm++){ int vb=rd16(g_sk_meshinfo+mm*4), vl=rd16(g_sk_meshinfo+mm*4+2);
                if (v0>=vb && v0<vb+vl){ g_tmesh[t]=(uint8_t)mm; break; } } }
          }
          /* pose fast path: convert skeleton to native s16 arrays ONCE (the hot
             loop then does plain s16 loads, not byte assembly), and group faces
             by mesh into contiguous lists (no 15x375 scan per frame). */
          { int i2, q, t, n;
            for (i2=0;i2<mc;i2++){ skvb[i2]=(uint16_t)rd16(g_sk_meshinfo+i2*4);
                                   skvl[i2]=(uint16_t)rd16(g_sk_meshinfo+i2*4+2); }
            for (i2=0;i2<g_lnv*3;i2++) skv[i2]=(int16_t)rd16(g_sk_meshverts+i2*2);
            for (i2=0;i2<(mc-1)*4;i2++) sknode[i2]=(int16_t)rd16(g_sk_nodes+i2*2);
            n=0; for (i2=0;i2<mc;i2++){ mq_start[i2]=(uint16_t)n;
              for (q=0;q<g_lnq;q++) if (g_qmesh[q]==i2) mq_list[n++]=(uint16_t)q; }
            mq_start[mc]=(uint16_t)n;
            n=0; for (i2=0;i2<mc;i2++){ mt_start[i2]=(uint16_t)n;
              for (t=0;t<g_lnt;t++) if (g_tmesh[t]==i2) mt_list[n++]=(uint16_t)t; }
            mt_start[mc]=(uint16_t)n;
          }
          if (g_jerry_ok) {
              extern void jerry_pose_setup(const int16_t*,int,const int16_t*,int,const int32_t*);
              jerry_pose_setup(skv, (g_lnv*3*2+3)/4, sknode, ((mc-1)*4*2+3)/4, SINTAB);
          }
#if defined(JERRYPOSE) && defined(SKUNK_CONSOLE) && !defined(QUIETFPS)
          /* JERRY MATH SELF-TEST: pose frame 0 on BOTH processors, compare
             every vert. Definitive fixed-point verification, no eyes needed.
             QUIETFPS builds SKIP this: the rx_ leg still kicks CMD=2 (roomx,
             DELETED from dsp_pose 2026-07-20) and can wedge Jerry so the
             next pose sync hangs boot forever — hit on silicon (AB run 2
             froze at rx_j1; run 1 got lucky). Play builds never ran this. */
          if (g_jerry_ok) {
              extern void jerry_pose_kick(const void*,const void*,const void*,
                  const void*,const void*,void*,int32_t,int32_t,int32_t,
                  int32_t,int32_t,int32_t,int32_t,int32_t,uint32_t);
              extern int jerry_pose_sync(void);
              extern void jerry_pose_read(void*,int);
              static uint8_t refblob[12288] __attribute__((aligned(8)));
              static uint8_t jblob[4096] __attribute__((aligned(8)));
              const uint8_t *rfr = g_sk_frames;      /* frame 0 root */
              const uint8_t *ang = g_sk_frames + 6;  /* frame 0 angles */
              int mm2, bad = 0;
              g_lframe = 0; g_anim_start = 0;
              g_lax = 0; g_laz = 0; g_lay = 0; g_layaw = 0;
              build_lara_tex(refblob, atlasW);        /* 68k reference */
              jerry_pose_kick(skv, skvl, sknode, ang, (void*)0, jblob,
                  rd16(rfr), rd16(rfr+2), rd16(rfr+4),
                  COS(0)>>2, SIN(0)>>2, 0, 0, 0 + LARA_FEET,
                  (uint32_t)g_sk_mcount);
              jerry_pose_sync();
              /* Jerry now writes jblob DIRECTLY (params[6]); no copy-back */
              for (mm2 = 0; mm2 < g_lnv*8; mm2++)
                  if (refblob[16+mm2] != jblob[mm2]) bad++;
              dbg_kv("jerry_badbytes", bad);
              { const uint32_t *rp=(const uint32_t*)(refblob+16);
                const uint32_t *jp=(const uint32_t*)jblob;
                int fb = -1;
                for (mm2 = 0; mm2 < g_lnv*2; mm2++)
                    if (rp[mm2] != jp[mm2]) { fb = mm2; break; }
                dbg_kv("first_bad_long", fb);
                dbg_kv("ref_v0a", rp[0]); dbg_kv("jer_v0a", jp[0]);
                dbg_kv("ref_v0b", rp[1]); dbg_kv("jer_v0b", jp[1]);
                dbg_kv("ref_v50a", rp[100]); dbg_kv("jer_v50a", jp[100]); }
              /* DISCRIMINATOR: pose again with ALL angles zero on both sides.
                 zero-ang match -> the rotation core is the bug; still bad ->
                 extraction/translate path. */
              { static uint8_t zfr[64]; int zi;
                const uint8_t *savefr = g_sk_frames;
                for (zi = 0; zi < g_sk_framestride && zi < 64; zi++)
                    zfr[zi] = (zi < 6) ? savefr[zi] : 0;   /* keep root, zero angles */
                g_sk_frames = zfr; g_lframe = 0; g_anim_start = 0;
                build_lara_tex(refblob, atlasW);
                jerry_pose_kick(skv, skvl, sknode, zfr + 6, (void*)0, jblob,
                    rd16(zfr), rd16(zfr+2), rd16(zfr+4),
                    COS(0)>>2, SIN(0)>>2, 0, 0, 0 + LARA_FEET,
                    (uint32_t)g_sk_mcount);
                jerry_pose_sync();
                /* direct DRAM write: jblob already holds the verts */
                g_sk_frames = savefr;
                bad = 0;
                for (mm2 = 0; mm2 < g_lnv*8; mm2++)
                    if (refblob[16+mm2] != jblob[mm2]) bad++;
                dbg_kv("jerry_badbytes_zeroang", bad);
                { const uint32_t *rp=(const uint32_t*)(refblob+16);
                  const uint32_t *jp=(const uint32_t*)jblob;
                  dbg_kv("zref_v0a", rp[0]); dbg_kv("zjer_v0a", jp[0]);
                  dbg_kv("zref_v0b", rp[1]); dbg_kv("zjer_v0b", jp[1]); } }
              /* MATRIX PROBE: mesh 0 only (mcount=1). 68k computes the same
                 root matrix locally; Jerry's MBLK ($F1BE80) is read back raw.
                 Diverging entry pinpoints the rotation-core op. */
              { extern void jerry_read_at(uint32_t, void*, int);
                SkMat tm; uint32_t jm[12]; int zz;
                const uint8_t *a3 = ang;
                dbg_kv("ang0", a3[0]); dbg_kv("ang1", a3[1]); dbg_kv("ang2", a3[2]);
                sk_ident(&tm);
                sk_translate(&tm, rd16(rfr), rd16(rfr+2), rd16(rfr+4));
                sk_rot_yxz(&tm, a3[0], a3[1], a3[2]);
                jerry_pose_kick(skv, skvl, sknode, ang, (void*)0, jblob,
                    rd16(rfr), rd16(rfr+2), rd16(rfr+4),
                    COS(0)>>2, SIN(0)>>2, 0, 0, 0 + LARA_FEET, 1u);
                jerry_pose_sync();
                jerry_read_at(0xF1C060u, jm, 12);
                for (zz = 0; zz < 3; zz++) {
                    dbg_kv("refR0", (uint32_t)tm.R[zz][0]); dbg_kv("jerR0", jm[zz*3+0]);
                    dbg_kv("refR1", (uint32_t)tm.R[zz][1]); dbg_kv("jerR1", jm[zz*3+1]);
                    dbg_kv("refR2", (uint32_t)tm.R[zz][2]); dbg_kv("jerR2", jm[zz*3+2]);
                }
                dbg_kv("refT0", (uint32_t)tm.t[0]); dbg_kv("jerT0", jm[9]);
                dbg_kv("refT1", (uint32_t)tm.t[1]); dbg_kv("jerT1", jm[10]);
                dbg_kv("refT2", (uint32_t)tm.t[2]); dbg_kv("jerT2", jm[11]);
                /* rotation-core INPUTS as Jerry sees them */
                { uint32_t chk[3];
                  jerry_read_at(0xF1C280u, chk, 3);
                  dbg_kv("jang0", chk[0]); dbg_kv("jang1", chk[1]); dbg_kv("jang2", chk[2]);
                  jerry_read_at(0xF1CB00u + 69*4, chk, 1);
                  dbg_kv("jsin69", chk[0]); dbg_kv("rsin69", (uint32_t)SINTAB[69]);
                  jerry_read_at(0xF1CB00u + 5*4, chk, 1);
                  dbg_kv("jsin5", chk[0]); dbg_kv("rsin5", (uint32_t)SINTAB[5]);
                  jerry_read_at(0xF1CB00u + 251*4, chk, 1);
                  dbg_kv("jsin251", chk[0]); dbg_kv("rsin251", (uint32_t)SINTAB[251]); }
                /* ROOM-TRANSFORM SELF-TEST: Jerry transforms room 0's verts;
                 68k computes the same projection (kernel-mirror math) and
                 byte-compares. Real camera pose (spawn-ish, yaw 0). */
              { extern void jerry_roomx_kick(const void*, const void*);
                static uint32_t rxlist[8];
                static uint32_t rxcache[4+512*2+512] __attribute__((aligned(8)));
                /* +512: the roomx mode now also writes per-vert DEPTHS at
                   cacheptr+4096 (the OT) — undersized = .bss corruption */
                static uint32_t rxcam[8];
                const uint8_t *hp = rgeom[0];
                int rvc = (hp[0]<<8)|hp[1];
                int roffX = ((int16_t)((hp[10]<<8)|hp[11]))<<8;
                int roffZ = ((int16_t)((hp[14]<<8)|hp[15]))<<8;
                int tcamx = rcx[0], tcamy = -256, tcamz = rcz[0] - 2000;
                fix tcY = COS(0), tsY = SIN(0);
                fix tcP = COS(CAM_PITCH), tsP = SIN(CAM_PITCH);
                int vv, bad2 = 0;
                if (rvc > 512) rvc = 512;
                rxcam[0]=(uint32_t)(tcY>>4); rxcam[1]=(uint32_t)(tsY>>4);
                rxcam[2]=(uint32_t)(tcP>>4); rxcam[3]=(uint32_t)(tsP>>4);
                rxcam[4]=(uint32_t)tcamx; rxcam[5]=(uint32_t)tcamy;
                rxcam[6]=(uint32_t)tcamz;
                rxlist[0]=1;
                rxlist[1]=(uint32_t)(rgeom[0]+16);   /* verts */
                rxlist[2]=(uint32_t)rvc;
                rxlist[3]=(uint32_t)roffX;
                rxlist[4]=(uint32_t)roffZ;
                rxlist[5]=(uint32_t)&rxcache[4];     /* flag at [3] */
                rxcache[3]=1;
                jerry_roomx_kick(rxlist, rxcam);
                jerry_pose_sync();
                dbg_kv("rx_flag", rxcache[3]);
                for (vv = 0; vv < rvc; vv++) {
                    const uint8_t *vp = rgeom[0]+16+vv*8;
                    int32_t x=(int16_t)((vp[0]<<8)|vp[1]);
                    int32_t y=(int16_t)((vp[2]<<8)|vp[3]);
                    int32_t z=(int16_t)((vp[4]<<8)|vp[5]);
                    int32_t dx=x+roffX-tcamx, dy=y-tcamy, dz=z+roffZ-tcamz;
                    int32_t rx=(dx*(tcY>>4)-dz*(tsY>>4))>>12;
                    int32_t rz=(dx*(tsY>>4)+dz*(tcY>>4))>>12;
                    int32_t ry=(dy*(tcP>>4)-rz*(tsP>>4))>>12;
                    int32_t rz2=(dy*(tsP>>4)+rz*(tcP>>4))>>12;
                    uint32_t esx, esy;
                    if (rz2 < 64) { esx=0x07FFFFFFu; esy=rxcache[4+vv*2+1]; }
                    else { esx=(uint32_t)(160+rx*190/rz2);
                           esy=(uint32_t)(120+ry*190/rz2); }
                    if (rxcache[4+vv*2] != esx) bad2++;
                    if (rxcache[4+vv*2+1] != esy) bad2++;
                }
                dbg_kv("rx_badlongs", bad2);
                dbg_kv("rx_vc", rvc);
                dbg_kv("rx_j0", rxcache[4]); dbg_kv("rx_j1", rxcache[5]); }
              /* ISOLATED ROTATION TEST (kernel mcount==255): 90deg Y on
                   identity. Expect R00=0 R02=4096 R20=-4096 R22=0. */
                jerry_pose_kick(skv, skvl, sknode, ang, (void*)0, jblob,
                    0, 0, 0, COS(0)>>2, SIN(0)>>2, 0, 0, 0, 255u);
                jerry_pose_sync();
                jerry_read_at(0xF1C060u, jm, 12);
                dbg_kv("rtR00", jm[0]); dbg_kv("rtR02", jm[2]);
                dbg_kv("rtR20", jm[6]); dbg_kv("rtR22", jm[8]);
                dbg_kv("rtR11", jm[4]); }
          }
#endif
        }
#if defined(SKUNK_CONSOLE)
        /* DISPATCH SELF-TEST: walk 1/2/5-room lists with a NULL camera (all
           verts land behind NEAR -> every face skips -> pure list-walk
           liveness). disp1 fails = init/first-iteration bug; disp1 ok but
           disp2 fails = the done->room_next loop-back; all ok = the hang
           only happens in-game (concurrency/real-clip interaction). */
        { static uint32_t tl[24];
          static uint32_t zcam[8];   /* zero camera: rz2=0 < NEAR everywhere */
          void *tfb = video_backbuffer();
          int rr2;
          { int q3; for (q3=0;q3<5;q3++){
              tl[1+q3*4+0]=(uint32_t)rgeom[q3%roomCount];
              tl[1+q3*4+1]=319u;
              tl[1+q3*4+2]=(uint32_t)(RENDER_H-1);
              tl[1+q3*4+3]=0; } }
          /* legacy FIRST (clean-GPU control), then dispatch + progress token */
          gpu_geotex_setclip(0,319,0,RENDER_H-1);
          rr2 = gpu_geotex(rgeom[0], tfb, zcam, S_atlas, (uint32_t)atlasW);
          dbg_kv("legacy1", rr2);
          *(volatile uint32_t *)0xF03F6Cu = 0;
          tl[0]=1;
          gpu_geotex_dispatch(tl, tfb, zcam, S_atlas, (uint32_t)atlasW);
          rr2 = gpu_sync(); dbg_kv("disp1", rr2);
          dbg_kv("dtok1", *(volatile uint32_t *)0xF03F6Cu);
          tl[0]=2;
          gpu_geotex_dispatch(tl, tfb, zcam, S_atlas, (uint32_t)atlasW);
          rr2 = gpu_sync(); dbg_kv("disp2", rr2);
          dbg_kv("dtok2", *(volatile uint32_t *)0xF03F6Cu); }
        dbg_kv("bc_selftest_done", 1);
#endif
        /* spawn: Lara's entity from the level file (room/pos/yaw), grounded
           on the spawn room's OWN floor nearest the entity's Y (stacked
           rooms must not steal it) */
        { int sproom;
          if (g_useset) { g_lax=GYM_SPAWN_X; g_lay=GYM_SPAWN_Y; g_laz=GYM_SPAWN_Z;
                          g_layaw=GYM_SPAWN_YAW; sproom=GYM_SPAWN_ROOM; }
          else          { g_lax=MRT_SPAWN_X; g_lay=MRT_SPAWN_Y; g_laz=MRT_SPAWN_Z;
                          g_layaw=MRT_SPAWN_YAW; sproom=MRT_SPAWN_ROOM; }
#ifdef SPAWNAT_ROOM
          /* OFFLINE REPRO (2026-07-30): drop Lara straight onto a reported
             trouble spot so her exact view can be screenshotted in jagemu
             instead of reflashing. Coords are WORLD units (g_lax is world, not
             fixed-point - MRT_SPAWN_X is assigned to it directly), i.e. 256x
             the POSDUMP pd_x/pd_z values. Emulator builds only. */
          g_lax=SPAWNAT_X; g_lay=SPAWNAT_Y; g_laz=SPAWNAT_Z;
          g_layaw=SPAWNAT_YAW; sproom=SPAWNAT_ROOM;
#endif
          g_curroom = sproom;
#ifdef SWDEBUG
          g_swact = 99;                          /* DEBUG: play switch pull   */
#endif
          { const uint8_t *r0 = rsect[sproom]; int fy;
            g_flr_wy = g_lay;    /* entity Y as context: closest real floor */
            g_lafloor = room_floor_mr(&r0, 1, g_lax, g_laz, &fy) ? fy : g_lay;
            g_floorroom = sproom; } }
        g_lay = g_lafloor; g_lavy = 0;               /* start grounded */
        g_flr_wy = g_lay;
#ifdef SKUNK_CONSOLE
        dbg_kv("bc_spawn", g_lafloor);
#endif

        /* place the pickup a bit ahead of Lara, ON the floor like TR1 (a
           chest-height float made the pick-up kneel read as "mid-air") */
        g_itemx = g_lax; g_itemz = g_laz + 2200;
        { int fy; g_itemy = (room_floor_mr(rsect, roomCount, g_itemx, g_itemz, &fy)
                             ? fy : g_lafloor) - (ITEM_S + 20); }

#ifdef SKUNK_CONSOLE
        dbg_kv("bc_item", g_itemy);
#endif
        /* gameplay props LIVE in the caves only (Home has no key/door) */
        g_itemcollected = g_useset ? 1 : 0; g_pickups = 0; g_itemspin = 0;
        g_doorx = g_lax; g_doorz = g_laz + 4500;
        { int fy; g_doory0 = (room_floor_mr(rsect, roomCount, g_doorx, g_doorz, &fy)
                              ? fy : g_lafloor) - 720; }
        g_dooryoff = g_useset ? -1600 : 0;           /* door SHUT in caves only */
#ifdef ENTITIES
        /* bake every real door's floor Y once; all doors start SHUT.
           Lara's Home has no entity table of its own, so props stay off
           there exactly as the hand-placed door did. */
#ifdef ENEMIES
        g_health = 1000; g_kills = 0; g_firecd = 0;
#endif
        { int e; for (e = 0; e < MRT_ENTCOUNT; e++) {
              g_entact[e] = 0; g_swpull[e] = 0; g_dooff[e] = 0;
              g_pickgot[e] = 0;
#ifdef ENEMIES
              g_batinit = 0; g_batdead[e] = 0;
#endif
              g_doorbase[e] = 0;
              /* switches bake the same floor Y - the lever hangs off it */
              if (g_useset || !(ent_is_door(mrt_ent[e].type) ||
                                ent_is_switch(mrt_ent[e].type))) continue;
              { int fy;
                g_doorbase[e] = (int16_t)((room_floor_mr(rsect, roomCount,
                                    mrt_ent[e].x, mrt_ent[e].z, &fy)
                                    ? fy : mrt_ent[e].y) - 720); }
              if (!ent_is_door(mrt_ent[e].type)) continue;
              /* HINGE END: for a PAIR (rooms 14, 17 and 34 hold two doors
                 each) hinge on the end AWAY from the partner, so the two
                 panels open like the double doors in the PS1 footage. */
              { int alongX = (mrt_ent[e].yaw == 0 || mrt_ent[e].yaw == 128);
                int me = alongX ? mrt_ent[e].x : mrt_ent[e].z;
                int j, sp;
                g_doorhinge[e] = -1;
                for (j = 0; j < MRT_ENTCOUNT; j++) {
                    int d;
                    if (j == e || !ent_is_door(mrt_ent[j].type)) continue;
                    if (mrt_ent[j].room != mrt_ent[e].room) continue;
                    d = (alongX ? mrt_ent[j].x : mrt_ent[j].z) - me;
                    if (d && d > -1200 && d < 1200)
                        { g_doorhinge[e] = (int8_t)(d > 0 ? -1 : 1); break; }
                }
                /* SWING SIDE: AWAY from the corridor, into the open side.
                   In the PS1 footage (Part 2, 9m09-9m27) Lara runs a long
                   tunnel and the doors at the end stand open on the FAR
                   side - they swing out of the tunnel, not into it. */
                sp = (int)mrt_ent[e].room;
                g_doorsw[e] = 1;
                if (sp < roomCount) {
                    const uint8_t *rs = rsect[sp];
                    int ix = (int)(((uint32_t)rs[4]<<24)|((uint32_t)rs[5]<<16)
                                 | ((uint32_t)rs[6]<<8)|rs[7]);
                    int iz = (int)(((uint32_t)rs[8]<<24)|((uint32_t)rs[9]<<16)
                                 | ((uint32_t)rs[10]<<8)|rs[11]);
                    int sx = (mrt_ent[e].x - ix) >> 10;
                    int sz = (mrt_ent[e].z - iz) >> 10;
                    int dx0 = alongX ? 0 : 1, dz0 = alongX ? 1 : 0;
                    int op = ent_side_open(rs, sx, sz,  dx0,  dz0);
                    int om = ent_side_open(rs, sx, sz, -dx0, -dz0);
                    g_doorsw[e] = (op >= om) ? 1 : -1;
                }
              }
          } }
#endif
        /* poke pickup GOLD into swatch cell 0 and door WOOD into cell 1 + CLUT */
        { uint8_t *atl = S_atlas; int yy, xx;
          for (yy=0; yy<MRT_LARA_CELL; yy++)
            for (xx=0; xx<MRT_LARA_CELL; xx++) {
              atl[(g_swy+yy)*MRT_ATLAS_W + xx] = (uint8_t)g_pickidx;
              atl[(g_swy+yy)*MRT_ATLAS_W + MRT_LARA_CELL + xx] = (uint8_t)g_dooridx;
#ifdef ENTITIES
              /* the lever, in the free part of the band past Lara's cells */
              atl[(g_swy+yy)*MRT_ATLAS_W + SW_CELLX + xx] = (uint8_t)SW_IDX;
#endif
            } }

#ifdef SKUNK_CONSOLE
        dbg_kv("bc_pokes", 1);
#endif
        /* ☠️ THE PALETTE SWAP MUST NOT LAND ON THE LOADING ART (user,
           2026-08-09: "discoloration in between the loading screen and when
           we're actually in the game").  video_set_clut(S_pal) rewrites
           0..253 while the cave-mouth art is still the displayed image, so
           for the rest of the load the art is drawn in the LEVEL's palette -
           the discoloured frames the user sees.  So: finish the bar, let the
           finished screen stand for a beat (the user asked for 1-2s - the
           load is quick enough that the panel otherwise blinks past), blank
           to black, and only THEN change the palette. */
        load_prog(8, 8);                      /* bar reaches the end, in the
                                                 art's own palette */
#ifndef FASTBOOT
        { extern volatile uint32_t frame_count;
          uint32_t h0 = frame_count;
          while (frame_count - h0 < 96u)      /* ~1.6s at 60Hz */
              ;
        }
#endif
        /* black on every buffer, and index 0 IS black in the level palette
           (mrt_pal[0] = 0,0,0), so this stays black across the swap */
        { extern uint8_t *const crash_fbs[3]; int b4;
          volatile uint16_t *cl0 = (volatile uint16_t *)0xF00400u;
          cl0[0] = 0x0000;
          for (b4 = 0; b4 < 3; b4++)
              blit_band(crash_fbs[b4], 0, 240, 0);
          video_flip();
          { extern volatile uint32_t frame_count; uint32_t h1 = frame_count;
            while (frame_count - h1 < 2u) ; } }
        g_loadfb = 0;                         /* nothing may draw into the
                                                 panel buffer from here on */
        video_set_clut(S_pal);
        { volatile uint16_t *clut=(volatile uint16_t*)0xF00400u;
          clut[254]=0x0000; clut[255]=0xFFFF;
          clut[g_pickidx]=0xFA37;                   /* gold pickup (also low-health bar) */
          /* ☠️ 0x918D was authored in the WRONG CHANNEL ORDER and decoded to
             rgb(148,49,49) - a BRICK RED slab, not the "brown wood" the
             comment claimed.  Jaguar RGB16 is R<<11 | B<<6 | G<<1, so green
             and blue are swapped vs the usual 565 (the gold pickup above is
             correct, which is why only the door looked wrong).
             ☠️ 0xBB28 (straw) was then ALSO wrong: I sampled the lashed pole
             GATE at 9m41, but the footage has TWO door looks and six of our
             eight doors are the DOOR_1/DOOR_2 pairs, which are CARVED STONE
             panels (9m23-9m28, the tunnel Lara runs).  Sampled from the
             frames themselves: rgb(51,43,38)..rgb(75,57,58), lifted a little
             because this blob is drawn UNSHADED while the world around it
             takes the ramp falloff.  0x6296 = rgb(98,90,82).
             ⬜ The pole gate needs a SECOND colour - palette slot 240 (the
             "gold pickup") is free in this build because the pickup lives in
             DEMO_PROPS, which is never defined. */
          clut[g_dooridx]=0x6296; }                  /* carved stone door */
        /* (the bar was filled and the panel retired above, before the palette
           swap - doing it here meant the last thing the player saw of the
           loading screen was the art in the wrong palette) */


        for (;;) {
            uint32_t pad;
            HLP(0);
#ifdef BUSPROBE
            { uint32_t _m = vp_tick(); if (bp_m) bp_p4 += _m - bp_m; bp_m = _m; bp_s0 = _m; }
#endif
#ifdef PROFILE
            { uint32_t _fc = frame_count;
              if (pftop) pftt2 += _fc - pftop;
              pftop = _fc; }
#endif
#ifdef GOVERNOR
            /* control step: last frame's VBL span decides this frame's cap */
            { static uint32_t gov_last;
              uint32_t gov_now = frame_count, gov_span = gov_now - gov_last;
              gov_last = gov_now;
              if (!g_gov_on) {
                  if (gov_span > GOV_HI && g_gov_user > 1) {
                      g_gov_on = 1; g_gov_calm = 0; g_gov_trips++;
                      g_hopcap = 1; g_hop_cached_a = -1;
                  }
              } else if (gov_span <= GOV_LO) {
                  if (++g_gov_calm >= GOV_K) {
                      g_gov_on = 0;
                      g_hopcap = g_gov_user; g_hop_cached_a = -1;
                  }
              } else g_gov_calm = 0;
            }
#endif
#ifdef BOOTCRUMBS
            /* METRONOME: fire a footstep every second (audible pitch check;
               the live v0-drain bar below shows the service rate) */
            { extern volatile uint32_t frame_count;
              static uint32_t nextstep;
              if (frame_count >= nextstep) {
                  sfx_play(0, SFX_STEP);
                  nextstep = frame_count + 60;
              } }
#endif
#if defined(SKUNK_CONSOLE) && !defined(QUIETFPS)
            /* stream the raw probe counters every ~5s of vblanks.
               QUIETFPS builds silence this: every skunk print STALLS the
               68k, and the periodic streams throttled the game to 1.19fps
               (vs 4.0 silent) — poisoning fps A/B runs. */
            { extern volatile uint32_t frame_count;
              static uint32_t nextvb, lws, lvc;
              if (frame_count >= nextvb) {
                  uint32_t ws = *(volatile uint32_t *)0xF1C0ACu;
                  uint32_t vc = *(volatile uint32_t *)0xF1C0B4u;
                  dbg_kv("vb", (long)frame_count);
                  { static uint32_t lfa;
                    uint32_t fa = *(volatile uint32_t *)0xF1C388u;
                    dbg_kv("wsd", (long)(ws - lws));
                    dbg_kv("fald", (long)(fa - lfa));
                    lfa = fa; }
                  dbg_kv("vcd", (long)(vc - lvc));
                  lws = ws; lvc = lvc = vc;
                  nextvb = frame_count + 300;
              } }
#endif
#ifdef BOOTCRUMBS
            /* POLL-RATE TELEMETRY heartbeat: alternate phase (liveness)
               with a color encoding the DSP pump's poll-service rate.
               GREEN = ~11kHz (latch-poll works on HW), RED = 0 (latch not
               readable), YELLOW = something in between/over. */
            { static uint32_t hbn, lastc;
              uint32_t c = *(volatile uint32_t *)0xF1C0ACu;  /* poll counter */
              uint32_t d = c - lastc; lastc = c;             /* per game frame */
              hbn++;
              if (hbn & 8) CRUMB(0x07C0);                    /* phase: blue */
              else if (d == 0)             CRUMB(0xF800);    /* RED: dead   */
              else if (d > 400 && d < 4000) CRUMB(0x003E);   /* GREEN: ~11k @3-9fps */
              else                          CRUMB(0xF83E);   /* YELLOW: odd rate */
              /* DUAL BARS: row8 = SSTAT-edge rate (want ~21px @8fps),
                 row12 = VC-tick rate (want ~61px @8fps). 1px = 64/frame. */
              { extern void *video_fb_n(int); int b2, x2;
                extern volatile uint32_t frame_count;
                static uint32_t lastvc, lastvb;
                uint32_t vcc = *(volatile uint32_t *)0xF1C0B4u;   /* WAKE_D reused */
                uint32_t vb = frame_count, dvb = vb - lastvb; lastvb = vb;
                if (!dvb) dvb = 1;
                /* bar1 = LIVE v0 CNT (the metronome step drains 3752 ->
                   sawtooth hitting 0 between beats if servicing keeps up);
                   bar2 = v0 CNT>>5 fine view */
                uint32_t v0now = *(volatile uint32_t *)0xF1C340u;
                int lenB = (int)(((vcc - lastvc) / dvb) >> 3); lastvc = vcc;
                int len = (int)(v0now >> 5);
                (void)d;
                if (len > 200) len = 200;
                if (lenB > 200) lenB = 200;
                for (b2 = 0; b2 < 3; b2++) {
                    uint8_t *fb = (uint8_t *)video_fb_n(b2);
                    for (x2 = 0; x2 < 200; x2++) {
                        uint8_t v = (x2 < len) ? 255 : 254;
                        if (x2 == 23 || x2 == 46) v = 255;   /* 11k/22k per-vblank ticks */
                        fb[8*RENDER_W + 8 + x2] = v;
                        fb[9*RENDER_W + 8 + x2] = v;
                        v = (x2 < lenB) ? 255 : 254;
                        if (x2 == 65) v = 255;               /* 525/vblank tick */
                        fb[12*RENDER_W + 8 + x2] = v;
                        fb[13*RENDER_W + 8 + x2] = v;
                    }
                } }
            }
#endif
            pad = joypad_read();  /* no per-frame SD poll (see menu note) */
#ifdef GDPAD
            /* REMOTE CONTROL FOR TESTING (user, 2026-08-09: "you can control -
             * GameDrive has facilities for that").  gd_input reads INPUT.BIN
             * off the SD as a PAD_* mask; the host writes it with
             * `jaggd -wf INPUT.BIN`, which uses the control endpoint and does
             * NOT reset the running game.  It was pulled from play builds for
             * good reason - an SD open+read+close EVERY frame, plus two failed
             * directory searches when the file is absent - so this is a TEST
             * flag and it polls every 4th frame, not every frame.
             * ☠️ Never -wf while a build is streaming video or music from the
             * same cart: the write races its gd_freads and locks the console.
             * FASTBOOT builds stream neither. */
            { static uint32_t gdp, gdc;
              if ((gdc++ & 3u) == 0) gdp = gd_input_poll();
              pad |= gdp; }
#endif
#ifdef TIMESTEP
            { static uint32_t ts_last;
              uint32_t f = frame_count;      /* 60 Hz, incremented by the ISR */
              uint32_t d = f - ts_last;
              /* JITTER FIX (2026-08-02): this used to round elapsed time to
               * INTEGER 30 Hz ticks clamped 1..4.  At ~7.5 fps a frame is ~8
               * fields = exactly 4 ticks, but any frame landing on 7 or 9
               * fields gave 3 or 4 - so her step length swung +-25% frame to
               * frame and read as JITTER.  Keep the exact FIELD count instead
               * and scale at the use sites, so motion tracks real elapsed
               * time with no rounding.  g_ticks is now in FIELDS (2 per 30 Hz
               * tick), so every consumer divides by 2.
               * Clamp 2..8 fields = 1..4 ticks, same tunnelling bound. */
              if (d < 2) d = 2; else if (d > 8) d = 8;
              ts_last += d;
              if ((int32_t)(ts_last - f) > 0) ts_last = f;
              g_ticks = (int)d;                          /* FIELDS, not ticks */
              g_tturn = (int)d / TURNDIV; if (g_tturn < 2) g_tturn = 2;
              g_tanim = (int)d / ANIMDIV; if (g_tanim < 2) g_tanim = 2; }
#endif
#ifdef HOPDIAL
            { static uint32_t hdprev;
              if (pad & PAD_OPTION) {
#ifdef GOVERNOR
                  /* the dial edits the RESTORE TARGET; it lands immediately
                     unless the governor is clamping right now */
                  if ((pad & PAD_RIGHT) && !(hdprev & PAD_RIGHT) && g_gov_user < 4)
                      { g_gov_user++;
                        if (!g_gov_on) { g_hopcap = g_gov_user; g_hop_cached_a = -1; }
                        dbg_kv("hopcap", g_gov_user); }
                  if ((pad & PAD_LEFT) && !(hdprev & PAD_LEFT) && g_gov_user > 1)
                      { g_gov_user--;
                        if (!g_gov_on) { g_hopcap = g_gov_user; g_hop_cached_a = -1; }
                        dbg_kv("hopcap", g_gov_user); }
#else
                  if ((pad & PAD_RIGHT) && !(hdprev & PAD_RIGHT) && g_hopcap < 4)
                      { g_hopcap++; g_hop_cached_a = -1; dbg_kv("hopcap", g_hopcap); }
                  if ((pad & PAD_LEFT) && !(hdprev & PAD_LEFT) && g_hopcap > 1)
                      { g_hopcap--; g_hop_cached_a = -1; dbg_kv("hopcap", g_hopcap); }
#endif
#ifdef FARDIAL
                  if ((pad & PAD_UP) && !(hdprev & PAD_UP) && g_fardist < 12000)
                      { g_fardist += 1000; dbg_kv("fard", g_fardist); }
                  if ((pad & PAD_DOWN) && !(hdprev & PAD_DOWN) && g_fardist > 2000)
                      { g_fardist -= 1000; dbg_kv("fard", g_fardist); }
                  pad &= ~(PAD_UP|PAD_DOWN);
#endif
                  hdprev = pad;
                  pad &= ~(PAD_LEFT|PAD_RIGHT|PAD_OPTION);
              } else hdprev = pad; }
#endif
#ifdef ABLADDER
            /* content ladder: OPTION+UP/DOWN steps the admitted-room count
               (0 = Lara alone). Arrows are eaten while OPTION is held so
               Lara stands still during the sweep; each step prints
               abrooms= to the console next to the rolling fps100=. */
            { static uint32_t abprev;   /* dbg_kv comes from skunkdbg.h
                                           (macro no-op when console off) */
              if (pad & PAD_OPTION) {
                  if ((pad & PAD_UP) && !(abprev & PAD_UP) && g_abrooms < 99)
                      { g_abrooms++; dbg_kv("abrooms", g_abrooms); }
                  if ((pad & PAD_DOWN) && !(abprev & PAD_DOWN) && g_abrooms > -1)
                      { g_abrooms--; dbg_kv("abrooms", g_abrooms); }
                  /* rung -1: Lara's dispatch is skipped too (pose still
                     runs) — rung0-minus-rung-1 = her pure GPU render cost */
                  abprev = pad;
                  pad &= ~(PAD_UP|PAD_DOWN|PAD_OPTION);
              } else abprev = pad; }
#endif
#ifdef HEADSPIN
            g_layaw = (uint8_t)(g_layaw + 3);   /* diagnostic: spin Lara */
#endif
            fix cY, sY, cP=COS(CAM_PITCH), sP=SIN(CAM_PITCH);
            int camx, camy, camz, k;
            fbpix *fb;
            /* Lara tank controls + multi-room floor-follow / wall collision */
            { int mv=0, fy;
#ifdef ROOMTOUR
            /* AUTO ROOM AUDIT: teleport through every room's centre, ~2 s each,
               so one recording captures all 38 rooms (label from DBGROOM). */
            { extern volatile uint32_t frame_count;
              static uint32_t nextadv = 120; static int touri = 0;
              if (frame_count >= nextadv && touri < roomCount) {
                  nextadv = frame_count + 120;          /* ~2 s at 60 Hz */
                  g_lax = roomtour[touri][0];
                  g_lay = roomtour[touri][1];
                  g_laz = roomtour[touri][2];
                  g_lavy = 0; g_curroom = touri;
                  if (room_floor_mr(rsect, roomCount, g_lax, g_laz, &fy)) {
                      g_lafloor = fy; g_lay = fy; g_curroom = g_floorroom; }
                  touri++;
              }
              pad = 0;   /* no player control during the tour */
            }
#endif
#if defined(PIPELINE) && defined(COLLECTEARLY)
            /* COLLECT HERE, NOT AT THE END OF THE LOGIC.
             *
             * Tom renders in ~33ms; the 68000's frame is ~130ms; the 68k
             * never waits at the old collect because Tom finished long
             * before.  So the whole of Lara's movement, collision, climbing
             * and the portal walk - the DRAM-hungry part - runs ON TOP of
             * Tom's render, and that bus pressure is what knocks faces out
             * (PIPESTAGE=0, which keeps the 68k off the bus entirely, cut the
             * flicker 549 -> 43).
             *
             * Collecting here costs whatever Tom still owes after the light
             * head of the frame, and buys a quiet bus for everything after.
             * Whether that is a better trade than PIPESTAGE=0's flat 25% is a
             * MEASUREMENT, not an argument - build both and compare. */
            if (g_tominflight) {
#ifdef BUSPROBE
                { uint32_t _s0 = vp_tick();
                  gpu_sync(); g_tominflight = 0;
                  bp_sleep += vp_tick() - _s0; }
#else
                gpu_sync(); g_tominflight = 0;
#endif
            }
#endif
#ifdef BUSPROBE
            { uint32_t _k = vp_tick(); if (bp_s0) bp_sA += _k - bp_s0; bp_s0 = _k; }
#endif
#ifdef MV_SIDE
              /* NOT a local.  As a local, `side` is live across the whole
                 tank-control block and the extra pressure re-allocates
                 registers throughout main() — 522 changed hunks against the
                 control, and the resulting build BLACK-SCREENS silicon from
                 t=0 even though nothing it added ever runs before the title
                 (2026-07-29 bisect: sidestep-only dies, roll-only boots, and
                 a byte- and address-matched padding build boots).  In .bss it
                 costs one word and perturbs nothing. */
              static int side; side = 0;
#else
              enum { side = 0 };
#endif
              g_flr_wy = g_lay;      /* Y context for stacked-room floor picks */
              /* walking: current room + neighbours only. AIRBORNE: search
                 ALL rooms — a long drop (the Caves descent) crosses several
                 rooms in one fall and the room that must catch her can be
                 2+ links down the stack (she fell through the bottom). */
              g_flr_limit = (g_lay >= g_lafloor - 4) ? 1 : 0;
              if (g_pickupt > 0) {
                  /* collecting: play the pick-up crouch anim, controls locked */
                  int st  = rd16(g_sk_anims + LANIM_PICKUP*6);
                  int cnt = rd16(g_sk_anims + LANIM_PICKUP*6 + 2);
                  int af  = ((PICKUP_TICKS - g_pickupt) * (cnt - 1)) / PICKUP_TICKS;
                  if (af > cnt - 1) af = cnt - 1;
                  g_lframe = st + af; g_anim_start = st;
                  g_pickupt--;
                  goto lara_done;
              }
#ifdef MV_ROLL
              /* ROLL: new press, grounded, not already busy. Yaw sweeps a
                 full 180 deg across the move so she ends facing back the
                 way she came, as in the original. */
              { static uint32_t rprev;
                uint32_t redge = pad & ~rprev; rprev = pad;
                if ((redge & (PAD_Y|PAD_PAUSE)) && !g_rollt && !g_vault
                    && !g_swim && g_lay >= g_lafloor - 4)
                    g_rollt = ROLL_TICKS; }
              if (g_rollt > 0) {
                  int half = ROLL_TICKS >> 1;
                  int rid  = (g_rollt > half) ? LANIM_ROLL : LANIM_ROLLEND;
                  int seg  = (g_rollt > half) ? (ROLL_TICKS - g_rollt)
                                              : (half - g_rollt);
                  int rst  = rd16(g_sk_anims + rid*6);
                  int rcnt = rd16(g_sk_anims + rid*6 + 2);
                  int raf  = (half > 1) ? (seg * (rcnt - 1)) / (half - 1) : 0;
                  if (raf > rcnt - 1) raf = rcnt - 1;
                  if (raf < 0) raf = 0;
                  g_lframe = rst + raf; g_anim_start = rst;
                  g_layaw = (g_layaw + (128 / ROLL_TICKS)) & 255;
                  g_rollt--;
                  goto lara_done;
              }
#endif /* MV_ROLL */
              /* ---- HANGING FROM A LEDGE (TR1 anim 29) ---------------------
                 She holds while ACTION is held. UP begins the pull-up (which
                 is the existing vault path, so HANDSTAND still works off WALK);
                 letting go of ACTION drops her into a normal fall. */
              if (g_hang) {
                  g_lay = g_vaulty + LARA_GRABREACH;
                  g_lavy = 0; g_airfr = 0;
                  if (!(pad & PAD_B) && !g_autograb) { /* released: let go, fall */
                      g_hang = 0; g_jumped = 0; g_lajf = 0;
                  } else if (pad & PAD_UP) {      /* pull up */
                      g_hang = 0; g_vault = 1; g_autograb = 0;
                      g_climbf = 0; g_climby0 = g_lay;
                      g_climbx0 = g_lax; g_climbz0 = g_laz;
                      /* TR1: WALK held during the pull-up = HANDSTAND */
                      g_climbanim = (pad & PAD_C) ? LANIM_HANDSTAND
                                                  : LANIM_HANGUP;
                  } else {
                      lanim_set(LANIM_HANG);
                      lanim_step(1, 1);
                      goto lara_done;
                  }
              }
              if (g_vault) {
                  /* PULL-UP: controls locked. Play the chosen climb anim
                     (g_climbanim) across CLIMB_TICKS ticks (fps is low, so run
                     the anim fast) while interpolating feet Y from the grab
                     height up to the ledge. On the last tick she pops forward
                     onto the foothold. The HANDSTAND is a long showpiece anim:
                     give it double the ticks so it reads. */
                  int ticks = (g_climbanim == LANIM_HANDSTAND) ? CLIMB_TICKS*2
                                                               : CLIMB_TICKS;
                  int t = (g_climbf < ticks) ? g_climbf : ticks;
                  int st  = rd16(g_sk_anims + g_climbanim*6);
                  int cnt = rd16(g_sk_anims + g_climbanim*6 + 2);
                  int af  = (g_climbf * (cnt - 1)) / ticks;
                  g_climbf++;
                  g_lay = g_climby0 + (int)((int32_t)(g_vaulty - g_climby0) * t / ticks);
                  /* ease x/z too: the end-of-climb horizontal snap read as
                     "sucked onto the ledge" */
                  g_lax = g_climbx0 + (int)((int32_t)(g_vaultx - g_climbx0) * t / ticks);
                  g_laz = g_climbz0 + (int)((int32_t)(g_vaultz - g_climbz0) * t / ticks);
                  if (af > cnt - 1) af = cnt - 1;
                  g_lframe = st + af; g_anim_start = st;
                  if (g_climbf > ticks) {
                      g_lax = g_vaultx; g_laz = g_vaultz;
                      g_lay = g_vaulty; g_lafloor = g_vaulty; g_lavy = 0;
                      g_vault = 0;
                  }
                  goto lara_done;
              }
              /* ---- WATER (v2, the mansion pool): enter, swim, climb out.
                 A water-surface cell is marked by bit0 of its STORED floorY
                 (extractor marks cells whose roomBelow is a water room). Do NOT
                 test (g_lafloor & 1): the slope interpolation in room_floor_mr
                 dirties bit0, so sloped SNOW read as water and Lara "swam"
                 through the Caves. Query the floor under her fresh and read the
                 clean g_floorwater flag (set from the stored, pre-slope bit). */
              if (!g_swim) {
                  int sfy;
                  if (room_floor_mr(rsect, roomCount, g_lax, g_laz, &sfy) &&
                      g_floorwater && sfy < 0x7000 && g_lay >= sfy - 16) {
                      g_swim = 1; g_watery = sfy; g_lay = sfy;
                      g_lavy = 0; g_airfr = 0;
                  } else if (g_curroom < 64 && rwater[g_curroom]) {
                      /* fallback: floor query resolved inside the water room */
                      g_swim = 1; g_watery = g_lay - 96;
                      g_lavy = 0; g_airfr = 0;
                  }
              }
              if (g_swim) {
                  int fwd = 0;
#ifdef TIMESTEP
                  if (pad & PAD_LEFT)  g_layaw -= 3 * g_tturn;
                  if (pad & PAD_RIGHT) g_layaw += 3 * g_tturn;
#else
                  if (pad & PAD_LEFT)  g_layaw -= 3;
                  if (pad & PAD_RIGHT) g_layaw += 3;
#endif
                  if (pad & PAD_UP)    fwd = 1;
                  if (pad & PAD_A)     g_lay -= 20;      /* rise   */
                  if (pad & PAD_C)     g_lay += 20;      /* dive   */
                  if (fwd) {
                      int nx = g_lax + (int)(((int32_t)SIN(g_layaw)*28)>>16);
                      int nz = g_laz + (int)(((int32_t)COS(g_layaw)*28)>>16);
                      int fy2;
                      if (room_floor_mr(rsect, roomCount, nx, nz, &fy2)) {
                          /* climb out: at the surface, pushing onto a deck
                             just above the water line */
                          if (!g_floorwater && g_lay <= g_watery + 64 &&
                              fy2 < g_lay && fy2 >= g_watery - 900) {
                              g_lax = nx; g_laz = nz;
                              g_swim = 0; g_lay = fy2; g_lafloor = fy2;
                              g_lavy = 0;
                              lanim_set(LANIM_IDLE); lanim_step(1, 1);
                              g_flr_limit = 0;
                              goto lara_done;
                          }
                          g_lax = nx; g_laz = nz;
                      }
                  }
                  if (g_lay < g_watery) g_lay = g_watery;
                  { int fy2;
                    if (room_floor_mr(rsect, roomCount, g_lax, g_laz, &fy2)
                        && g_lay > fy2 - 160) g_lay = fy2 - 160; }
                  /* anims: 114 = to-surface (hold last = tread),
                     116 = surface crawl, 108 = underwater glide */
                  if (g_lay <= g_watery + 32) {
                      lanim_set(fwd ? 116 : 114);
                      lanim_step(1, fwd ? 1 : 0);
                  } else {
                      lanim_set(108);
                      lanim_step(1, 1);
                  }
                  g_lafloor = g_lay;   /* keep the camera/floor logic sane */
                  g_flr_limit = 0;
                  goto lara_done;
              }
#ifdef TIMESTEP
              if (pad & PAD_LEFT)  g_layaw -= 3 * g_tturn;
              if (pad & PAD_RIGHT) g_layaw += 3 * g_tturn;
#else
              if (pad & PAD_LEFT)  g_layaw -= 3;
              if (pad & PAD_RIGHT) g_layaw += 3;
#endif
              if (pad & PAD_UP)   mv = 1;
              if (pad & PAD_DOWN) mv = -1;
#if defined(MULTIROOM) && defined(ENTITIES) && defined(SWANIM)
              /* locked in the switch pull: she cannot walk out of it */
              if (g_swact > 0) { mv = 0; pad &= ~(PAD_LEFT|PAD_RIGHT); }
#endif
#ifdef MV_SIDE
              /* SIDESTEP owns LEFT/RIGHT while WALK is held (undo the turn
                 the two lines above already applied). */
              side = ((pad & PAD_C) && (pad & (PAD_LEFT|PAD_RIGHT)) && !mv)
                     ? ((pad & PAD_LEFT) ? -1 : 1) : 0;
#ifdef TIMESTEP
              if (side) g_layaw = (g_layaw + (side < 0 ? (3*g_tturn)>>1 : -((3*g_tturn)>>1))) & 255;
#else
              if (side) g_layaw = (g_layaw + (side < 0 ? 3 : -3)) & 255;
#endif
#endif
              /* FAST_BACK ramps from rest; the slow WALK-back does not */
              if (mv < 0 && !(pad & PAD_C)) g_backf += g_ticks >> 1;
              else                          g_backf = 0;
              if (mv) {
                  int spd = (pad & PAD_C) ? WALK_SPEED_TR1 : RUN_SPEED_TR1;
                  if (mv < 0) {
                      if (pad & PAD_C) spd = BACK_SPEED_TR1;   /* anim 38 */
                      else {                                   /* anim 88 */
                          spd = (FASTBACK_ACC_N * g_backf) / FASTBACK_ACC_D;
                          if (spd > FASTBACK_MAX) spd = FASTBACK_MAX;
                          if (spd < 1) spd = 1;
                      }
                  }
#ifdef TIMESTEP
                  spd = (spd * g_ticks) >> 1;   /* g_ticks is FIELDS; 2/tick */
#endif
                  int nx = g_lax + (int)(((int32_t)SIN(g_layaw)*(spd*mv))>>16);
                  int nz = g_laz + (int)(((int32_t)COS(g_layaw)*(spd*mv))>>16);
                  int mx0 = g_lax, mz0 = g_laz;   /* for the stall detector */
                  int nf;   /* only walk onto a floor whose step-UP is small (a
                               tall step is a wall/ledge you can't just walk up;
                               this stops Lara popping onto raised sectors) */
                  if (!room_wall_at(rsect[g_curroom], nx, g_laz) &&
                      room_floor_mr(rsect, roomCount, nx, g_laz, &nf) &&
#ifdef ENTITIES
                      !ent_door_blocks(g_curroom, g_lax, g_laz, nx, g_laz) &&
#endif
                      g_lafloor - nf <= LARA_STEPUP) g_lax = nx;
                  if (!room_wall_at(rsect[g_curroom], g_lax, nz) &&
                      room_floor_mr(rsect, roomCount, g_lax, nz, &nf) &&
#ifdef ENTITIES
                      !ent_door_blocks(g_curroom, g_lax, g_laz, g_lax, nz) &&
#endif
                      g_lafloor - nf <= LARA_STEPUP) g_laz = nz;
                  /* pressed against something: forward held but she moved on
                     NEITHER axis. Gates the auto jump-reach probe below so
                     its two extra floor searches (the priciest 68k call in
                     the frame) only run at actual wall contact - probing
                     every UP-held frame measurably dropped the GAME's fps
                     (user 2026-08-07). */
                  g_fwdblk = (mv > 0 && g_lax == mx0 && g_laz == mz0);
              }
#ifdef MV_SIDE
              if (side) lara_sidestep(side, rsect, roomCount);
#endif
#ifdef DEMO_PROPS
              /* the shut door blocks the corridor until it has slid up */
              if (g_dooryoff > -1200 && g_laz > g_doorz - 300 &&
                  mr_iabs(g_lax - g_doorx) < 700) g_laz = g_doorz - 300;
#endif
              if (room_floor_mr(rsect, roomCount, g_lax, g_laz, &fy)) {
                  g_lafloor = fy;
                  g_curroom = g_floorroom;   /* the room Lara stands in */
              }
#ifdef ENTITIES
              /* one tick of the level's own triggers: the sector Lara now
                 stands in may be a PAD, or a SWITCH she can pull with
                 ACTION.  Runs here, on the movement tick, so it sees the
                 room she actually ended up in. */
              if (!g_useset)
                  ent_update(rsect, g_curroom, g_lax, g_laz,
                             (pad & PAD_B) != 0);
#ifdef BUSPROBE
            { uint32_t _k = vp_tick(); if (bp_s0) bp_sB += _k - bp_s0; bp_s0 = _k; }
#endif
              /* PICKUPS: spin them, and collect any Lara walks over */
              if (!g_useset) {
                  int pe; g_pickspin += 4;
#ifdef ENEMIES
                  { static int bf; int be;
                    if ((++bf & 1) == 0) g_batframe++;      /* wing flap */
                    if (!g_batinit) {                        /* seed once */
                        for (be=0; be<MRT_ENTCOUNT; be++)
                            if (ent_is_enemy(mrt_ent[be].type)) {
                                g_batx[be]=mrt_ent[be].x; g_baty[be]=mrt_ent[be].y;
                                g_batz[be]=mrt_ent[be].z;
                            }
                        g_batinit = 1;
                    }
                    /* AI: home toward Lara, bite on contact. Per-type: bats fly
                       at head height and are fast; wolves charge on the ground;
                       bears are slow but hit hard. */
                    for (be=0; be<MRT_ENTCOUNT; be++) {
                        int t, tx, ty, tz, d, bdx, bdz, reach, dmg;
                        t = mrt_ent[be].type;
                        if (!ent_is_enemy(t) || g_batdead[be]) continue;
                        bdx=g_lax-g_batx[be]; bdz=g_laz-g_batz[be];
                        if (bdx<0)bdx=-bdx; if (bdz<0)bdz=-bdz;
                        if (bdx+bdz > 8192) continue;         /* dormant if far */
                        tx = g_lax; tz = g_laz;
                        if (ent_is_bat(t))      { ty = g_lay - 640; d = 24*g_ticks; reach = 400; dmg = 2; }
                        else if (ent_is_wolf(t)){ ty = g_lay;       d = 20*g_ticks; reach = 500; dmg = 3; }
                        else                    { ty = g_lay;       d = 12*g_ticks; reach = 600; dmg = 5; }
                        /* close in only until at bite range - never stack ON Lara
                           (that occludes the enemy behind her and looks wrong).
                           Y still tracks so ground enemies follow ramps. */
                        if (bdx + bdz > reach) {
                            if (g_batx[be] < tx-d) g_batx[be]+=d; else if (g_batx[be] > tx+d) g_batx[be]-=d; else g_batx[be]=tx;
                            if (g_batz[be] < tz-d) g_batz[be]+=d; else if (g_batz[be] > tz+d) g_batz[be]-=d; else g_batz[be]=tz;
                        }
                        if (g_baty[be] < ty-d) g_baty[be]+=d; else if (g_baty[be] > ty+d) g_baty[be]-=d; else g_baty[be]=ty;
                        if (bdx + bdz < reach && g_health > 0)  /* bite */
                            g_health -= dmg * g_ticks;
                    }
                    /* FIRE (PAD_X): auto-target the nearest live bat in range,
                       kill it. Short cooldown so a held button doesn't chew the
                       whole swarm in one tick. No weapon model yet. */
                    if (g_firecd > 0) g_firecd -= g_ticks;
                    if ((pad & PAD_X) && g_firecd <= 0) {
                        int bestk=-1, bestd=1<<30;
                        for (be=0; be<MRT_ENTCOUNT; be++) {
                            int fdx, fdz, fd;
                            if (!ent_is_enemy(mrt_ent[be].type) || g_batdead[be]) continue;
                            fdx=g_lax-g_batx[be]; fdz=g_laz-g_batz[be];
                            if (fdx<0)fdx=-fdx; if (fdz<0)fdz=-fdz;
                            fd=fdx+fdz;
                            if (fd < bestd && fd < 3072) { bestd=fd; bestk=be; }
                        }
                        if (bestk >= 0) {
                            g_batdead[bestk] = 1; g_kills++;
                            g_firecd = 8;
#ifndef NOSOUND
                            sfx_play(1, SFX_MENU_SPIN);
#endif
                        }
                    }
                    if (g_health <= 0) g_dead = 1;   /* killed by damage */
                  }
#endif
#ifdef DARTS
                  darts_update(g_ticks, g_curroom);
#endif
                  for (pe = 0; pe < MRT_ENTCOUNT; pe++) {
                      int pdx, pdz, pdy;
                      if (!ent_is_pickup(mrt_ent[pe].type) || g_pickgot[pe]) continue;
                      /* distance-only (stacked rooms make the room field
                         unreliable), but gate Y too so she can't grab a pickup
                         one floor above/below through the ceiling. */
                      pdx = g_lax - mrt_ent[pe].x; pdz = g_laz - mrt_ent[pe].z;
                      pdy = g_lay - mrt_ent[pe].y;
                      if (pdx < 0) pdx = -pdx; if (pdz < 0) pdz = -pdz;
                      if (pdy < 0) pdy = -pdy;
                      if (pdy < 512 && pdx + pdz < PICKUP_REACH) {
                          g_pickgot[pe] = 1;
#ifdef ENEMIES
                          /* medikits heal (big=full, small=half); crystal=n/a */
                          if (mrt_ent[pe].type == 94) g_health = 1000;
                          else if (mrt_ent[pe].type == 93) {
                              g_health += 500; if (g_health > 1000) g_health = 1000;
                          }
#endif
#ifndef NOSOUND
                          sfx_play(1, SFX_MENU_SPIN);   /* pickup blip */
#endif
                      }
                  }
              }
#endif
#ifdef AUTOSPIN
              /* FPS RIG (2026-07-30): rotate Lara a fixed amount every tick.
                 Frame-differencing needs on-screen MOTION - measuring an idle
                 AUTOSTART scene read 1.50 fps (26 flips in 1200 fields) because
                 a standing Lara barely changes the image. Spinning redraws the
                 whole screen every frame, so every flip is detectable, and it
                 is identical in both arms with no human variance. Rig only. */
              g_layaw = (g_layaw + 2) & 255;
#endif
#ifdef POSDUMP
              /* WHERE-AM-I probe (2026-07-30): the "black entryway you can't
                 walk through" needs an exact room + position to analyse
                 offline. Dump every 64 ticks: walk to the wall, stand still,
                 and the last lines name the spot. Deliberately minimal - this
                 edits main.c, which is subject to the A10 lottery. */
              { static int pdt;
                if (((++pdt) & 63) == 0) {
                    dbg_kv("pd_room", g_curroom);
                    dbg_kv("pd_x",    g_lax >> 8);
                    dbg_kv("pd_y",    g_lay);
                    dbg_kv("pd_z",    g_laz >> 8);
                    dbg_kv("pd_yaw",  g_layaw);
#ifdef CNTDUMP
                    /* CULLCOUNT counters, per dump, at a FIXED camera. If
                       rastered is IDENTICAL every frame while the face visibly
                       toggles, the face IS being submitted and the BLIT is not
                       landing => Blitter-level. If rastered VARIES, the face is
                       being rejected on some frames => something upstream (the
                       vertex data) differs frame to frame. That is the split no
                       amount of flag-toggling could give. */
                    { volatile uint32_t *c = (volatile uint32_t *)0x1C0000u;
                      dbg_kv("staged",   (long)c[0]);
                      dbg_kv("rastered", (long)c[1]);
                      dbg_kv("bexit",    (long)c[4]);
                      dbg_kv("wcull",    (long)c[5]);   /* $1C0014 */   /* $1C0010: near-plane
                            whole-face rejects. If THIS rises and falls in step
                            with the toggle, the marginal threshold is the near
                            plane; if it is flat while rastered swings, it is
                            one of the screen-space guards instead. */
                      c[0] = 0; c[1] = 0; c[4] = 0; c[5] = 0; }
#endif
                } }
#endif
              /* STANDING CLIMB (TR: hold FORWARD + ACTION facing a wall). Lara
                 squares up to the wall (alignToWall) and pulls up onto a ledge
                 that's too tall to step but within climb reach. PAD_B = ACTION. */
              if ((pad & PAD_UP) && (pad & PAD_B) && g_lay >= g_lafloor - 4 && !g_vault) {
                  int px = g_lax + (int)(((int32_t)SIN(g_layaw)*(WALK_SPEED*2))>>16);
                  int pz = g_laz + (int)(((int32_t)COS(g_layaw)*(WALK_SPEED*2))>>16);
                  int lf, rise, lfok;
                  int cy0;
                  g_flr_grab = 1;
                  lfok = room_floor_mr(rsect, roomCount, px, pz, &lf);
                  g_flr_grab = 0;
                  if (lfok &&
                      (rise = g_lafloor - lf) > LARA_STEPUP && rise <= LARA_CLIMB &&
                      (!room_ceil_at(rsect[g_curroom], g_lax, g_laz, &cy0) ||
                       lf >= cy0) &&
                      room_reachable(g_curroom, g_floorroom)) {
                      g_layaw = ALIGN_WALL(g_layaw);   /* turn square to the wall */
                      g_vault = 1; g_vaulty = lf; g_vaultx = px; g_vaultz = pz;
                      g_climbf = 0; g_climby0 = g_lay;
                        sfx_play(1, SFX_GRUNT);
                        g_climbx0 = g_lax; g_climbz0 = g_laz;
                      /* TR1 picks the vault by LEDGE HEIGHT (lara.h ~2548):
                           <= 2.5 clicks  ANIM_CLIMB_2    (the kick-flip)
                           <= 3.5 clicks  ANIM_CLIMB_3    (the taller haul)
                           taller         ANIM_CLIMB_JUMP (the jump-up catch)
                         Only CLIMB_2 was ever used, so tall ledges played the
                         short animation and looked like she teleported up. */
                      g_climbanim = (rise <= 640) ? LANIM_VAULT
                                  : (rise <= 896) ? LANIM_CLIMB3
                                                  : LANIM_CLIMBJUMP;
                      goto lara_done;                  /* skip jump physics       */
                  }
              }
              /* AUTO JUMP-REACH (user 2026-08-07): UP alone against a wall
                 whose ledge sits above vault reach but within an up-jump's
                 hands = she jumps in place reaching for it; the airborne
                 grab catches with ACTION implied and held UP pulls her up. */
              if ((pad & PAD_UP) && g_fwdblk && g_lay >= g_lafloor - 4 &&
                  !g_vault && !g_jumped && !g_hang && !g_autoj) {
                  /* probe a QUARTER CELL ahead, not WALK_SPEED*2 (94): the
                     move gate halts her up to a full frame-step (~140 units)
                     short of the wall, so the short probe sampled HER OWN
                     cell and never armed — she'd long-jump instead (user
                     2026-08-07 second report) */
                  int px = g_lax + (int)(((int32_t)SIN(g_layaw)*256)>>16);
                  int pz = g_laz + (int)(((int32_t)COS(g_layaw)*256)>>16);
                  int lf, rise, lfok;
                  int cy2;
                  g_flr_grab = 1;
                  lfok = room_floor_mr(rsect, roomCount, px, pz, &lf);
                  g_flr_grab = 0;
                  if (lfok &&
                      (rise = g_lafloor - lf) > LARA_CLIMB &&
                      rise <= LARA_JUMPGRAB &&
                      (!room_ceil_at(rsect[g_curroom], g_lax, g_laz, &cy2) ||
                       lf >= cy2) &&
                      room_reachable(g_curroom, g_floorroom)) {
                      g_layaw = ALIGN_WALL(g_layaw);   /* square to the wall */
                      g_autoj = 1;                     /* arm the up-jump */
                  }
              }
              /* jump physics (+Y down: up = negative vy). EDGE-triggered:
                 a NEW press of PAD_A while grounded arms a 1-frame compress,
                 then launch. (Held-A used to relaunch on every landing —
                 pogo — and zero anticipation read as weightless.) */
              { int grounded = (g_lay >= g_lafloor - 4);
                static uint32_t jprev; static int jprep;
                uint32_t jedge = pad & ~jprev;
                jprev = pad;
                if (((jedge & PAD_A) || g_autoj) && grounded && !jprep)
                    jprep = 2;
                if (jprep && grounded) {
                    if (--jprep == 0) {
                        if (g_autoj) {
                            /* AUTO JUMP-REACH launch: straight up, hands out */
                            g_autoj = 0; g_autograb = 1;
                            g_lajf = 0; g_jdir = 0;
                            g_jumped = 1;
                            g_lavy = -JUMP_VEL_UP;
                            g_jfwd = 0;
                        } else {
                        /* TR1 picks the jump by what is held at launch: no
                           direction = UP_JUMP (higher, barely forward); a
                           direction = a directional jump, and its reach
                           depends on whether she was already running. */
                        /* TR1 picks the jump from what is held at launch:
                           nothing = UP_JUMP, forward/back/side = the matching
                           directional jump (anims 16/74/78/80, all velY=-100
                           velZ=50, running forward gets velZ=75). */
                        if (mv > 0)      { g_lajf = 1; g_jdir = 0; }
                        else if (mv < 0) { g_lajf = 1; g_jdir = 1; }
                        else if (side<0) { g_lajf = 1; g_jdir = 2; }
                        else if (side>0) { g_lajf = 1; g_jdir = 3; }
                        else             { g_lajf = 0; g_jdir = 0; }
                        g_jumped = 1;
                        g_lavy = -(g_lajf ? JUMP_VEL_FWD : JUMP_VEL_UP);
                        g_jfwd = !g_lajf ? 0
                               : ((g_jdir == 0 && !(pad & PAD_C)) ? JUMP_FWD_RUN
                                                                  : JUMP_FWD_STAND);
                        }
                    }
                } else if (!grounded) jprep = 0;
                /* Integrate on TR1's 30 Hz TICK, not the render frame.
                   g_ticks counts FIELDS (60 Hz), so one frame is g_ticks/2
                   ticks; stepping them individually reproduces the game's
                   arc exactly instead of approximating it with one big
                   Euler step (which is what made the arc frame-rate
                   dependent, and the reason she could reach ledges she
                   should not). */
                g_layprev = g_lay;             /* for the SWEPT grab test */
                { int jn = g_ticks >> 1; if (jn < 1) jn = 1;
                  while (jn--) {
                      g_lavy += (g_lavy < GRAVITY_TERM) ? GRAVITY : 1;
                      g_lay  += g_lavy;
                  } }
                /* CEILING CLAMP: a running jump at our low fps moves many
                   hundred units per frame and used to sail straight through
                   the ceiling. Head is ~720 above the feet; an open ceiling
                   (vertical portal above) is -32768 and never clamps. */
                if (!grounded && g_curroom < 64) {
                    int cly;
                    if (room_ceil_at(rsect[g_curroom], g_lax, g_laz, &cly) &&
                        cly > -32000 && g_lay - 720 < cly) {
                        g_lay = cly + 720;
                        if (g_lavy < 0) g_lavy = 0;   /* bonk: start falling */
                    }
                }
                g_airfr++;                     /* count airborne frames */
                /* airborne running-jump: carry forward (over the arc), no ground
                   collision while flying; land wherever the floor is. */
                if (g_lay < g_lafloor - 4 && g_lajf) {
                    /* same 30 Hz tick count as the vertical integration above,
                       so the arc keeps its shape at any frame rate */
                    int jt = g_ticks >> 1; if (jt < 1) jt = 1;
                    /* travel along the direction chosen AT LAUNCH (SINTAB is
                       256/turn, so a quarter turn is 64) — she keeps flying
                       that way even if the stick moves, as TR1 does */
                    { int ja = g_layaw + (g_jdir == 1 ? 128 :
                                          g_jdir == 2 ? 192 :
                                          g_jdir == 3 ?  64 : 0);
                    int jd = g_jfwd * jt;
                    int jx = g_lax + (int)(((int32_t)SIN(ja) * jd) >> 16);
                    int jz = g_laz + (int)(((int32_t)COS(ja) * jd) >> 16);
                    /* walls stop her in the air too (no jump-through) — and so
                       does a LEDGE FACE: a column whose floor is more than a
                       step above her feet is a wall while she is under it.
                       Without this the carry drifts her over a high terrace
                       and the landing clamp teleports her UP onto a ledge her
                       arc never reached (user clip 2026-08-07: long-jump
                       gaining a full block; TR1 wants jump-grab-pull-up).
                       Approaching from ABOVE (feet over the ledge top) still
                       lands normally — that is a real jump onto the ledge. */
                    { int af;
                    g_flr_wy = g_lay;   /* integration moved her: fresh Y context */
                    if (!room_wall_at(rsect[g_curroom], jx, g_laz) &&
                        room_floor_mr(rsect, roomCount, jx, g_laz, &af) &&
                        g_lay - af <= LARA_STEPUP) g_lax = jx;
                    if (!room_wall_at(rsect[g_curroom], g_lax, jz) &&
                        room_floor_mr(rsect, roomCount, g_lax, jz, &af) &&
                        g_lay - af <= LARA_STEPUP) g_laz = jz;
                    } }
                }
                if (g_lay >= g_lafloor) {
                    /* thud only on REAL falls: slope descents micro-hop
                       airborne for 1-3 frames and re-land every stride,
                       which double/triple-tapped the step sound (user
                       report 2026-07-13) */
                    if (!grounded && g_lavy > 60 && g_airfr >= 5)
                        sfx_play(1, SFX_LAND);
                    /* TR1 fall damage: harmless under 3 blocks, lethal past
                       about 4.5. We have no health bar yet, so only the fatal
                       case is wired — it is what makes DEATH (anim 25) reachable. */
                    if (!grounded && g_fally && (g_lafloor - g_fally) > 4608)
                        g_dead = 1;
                    g_lay = g_lafloor; g_lavy = 0; grounded = 1; g_lajf = 0;
                    g_jumped = 0; g_jdir = 0; g_fally = 0;
                    g_autograb = 0;               /* auto-reach missed: done */
                    g_airfr = 0; }
                else if (g_lavy == 0 && g_lafloor - g_lay <= LARA_STEPUP) {
                    /* walking DOWN a step/slope or across a room seam: EASE
                       to the new floor over a few frames instead of the
                       one-frame snap (the "sucked in" feel at transitions).
                       Real falls (past STEPUP) keep gravity. */
                    g_lay += (g_lafloor - g_lay + 1) >> 1;
                    grounded = 1; g_lajf = 0; g_airfr = 0;
                }
                else grounded = 0;
                /* JUMP GRAB (TR: hold ACTION in mid-air). If her hands (feet -
                   reach) come level with a ledge ahead, she catches it and pulls
                   up, squaring to the wall. PAD_B = ACTION. */
                if (!grounded && ((pad & PAD_B) || g_autograb)) {
                    int px = g_lax + (int)(((int32_t)SIN(g_layaw)*(WALK_SPEED*2))>>16);
                    int pz = g_laz + (int)(((int32_t)COS(g_layaw)*(WALK_SPEED*2))>>16);
                    int lf, handY = g_lay - LARA_GRABREACH, lfok;
                    int cy1;
                    g_flr_grab = 1;
                    lfok = room_floor_mr(rsect, roomCount, px, pz, &lf);
                    g_flr_grab = 0;
                    /* SWEPT grab window.  TR1's rule is |ledge - hands| < 64
                       (lara.h checkHang), but at 7.5 fps she covers ~500 units
                       of altitude per frame, so a +-64 point test would almost
                       never sample the ledge -- which is why this used to be a
                       544-unit band, and why she caught ledges she should have
                       sailed past.  Instead ask whether her hands CROSSED the
                       ledge height at any point during this frame's motion,
                       then apply TR1's real +-64 tolerance to the swept
                       interval.  Correct at any frame rate. */
                    { int hPrev = g_layprev - LARA_GRABREACH;
                      int hLo = (handY < hPrev ? handY : hPrev) - LARA_GRAB_TOL;
                      int hHi = (handY > hPrev ? handY : hPrev) + LARA_GRAB_TOL;
                    if (lfok &&
                        lf >= hLo && lf <= hHi &&
                        (g_lafloor - lf) > LARA_STEPUP &&
                        (!room_ceil_at(rsect[g_curroom], g_lax, g_laz, &cy1) ||
                         lf >= cy1) &&
                        room_reachable(g_curroom, g_floorroom)) {
                        g_layaw = ALIGN_WALL(g_layaw);   /* turn square to the wall */
                        /* TR1 HANGS here rather than vaulting straight up: she
                           catches the ledge and holds while ACTION is held.
                           UP starts the pull-up, releasing ACTION drops her.
                           (anim 29 HANG was extracted but never reachable.) */
                        g_hang = 1; g_lavy = 0; g_jumped = 0; g_lajf = 0;
                        g_vaulty = lf; g_vaultx = px; g_vaultz = pz;
                        g_lay = lf + LARA_GRABREACH;   /* hands on the lip */
                        sfx_play(1, SFX_GRUNT);
                    } }
                }
                /* remember where a descent began, for fall damage */
                if (!grounded && g_lavy > 0 && !g_fally) g_fally = g_layprev;
                /* ---- SLIDE (TR1 anims 70 / 104) -----------------------------
                   TR1: a sector slides when |slantX| > 2 or |slantZ| > 2. The
                   downhill direction is the dominant axis; if she is facing
                   more than a quarter turn away from it she slides BACKWARDS
                   (SLIDE_BACK) instead. g_flr_slx/slz come from the same cell
                   the floor height was read from. */
                g_sliding = 0;
                if (grounded && !g_dead) {
                    int asx = g_flr_slx < 0 ? -g_flr_slx : g_flr_slx;
                    int asz = g_flr_slz < 0 ? -g_flr_slz : g_flr_slz;
                    if (asx > 2 || asz > 2) {
                        int d8;                       /* SINTAB units, 256/turn */
                        if (asx >= asz) d8 = (g_flr_slx > 0) ? 192 : 64;
                        else            d8 = (g_flr_slz > 0) ? 128 :  0;
                        { int rel = (d8 - g_layaw) & 255;
                          if (rel > 128) rel -= 256;      /* signed shortest arc */
                          if (rel < 0) rel = -rel;
                          g_sliding = (rel > 64) ? 2 : 1; }
                        g_slideang = (g_sliding == 2) ? ((d8 + 128) & 255) : d8;
                        /* slide her downhill; TR1 slide speed is ~ (anim 70) */
                        { int st = (SLIDE_SPEED * g_ticks) >> 1;
                          int sx2 = g_lax + (int)(((int32_t)SIN(g_slideang)*st)>>16);
                          int sz2 = g_laz + (int)(((int32_t)COS(g_slideang)*st)>>16);
                          if (!room_wall_at(rsect[g_curroom], sx2, g_laz)) g_lax = sx2;
                          if (!room_wall_at(rsect[g_curroom], g_lax, sz2)) g_laz = sz2; }
                    }
                }
                /* full moveset (runtime-skinned). airborne = jump cycle (one-
                   shot, clamps at land); fwd = run or WALK (PAD_C); back = the
                   hop-back anim; turning in place = turn L/R; else STAND. */
                if (g_dead) {
                    lanim_set(LANIM_DEATH);
                    lanim_step(0, 1);            /* one-shot, clamps at the end */
                    /* No health bar or reload screen yet, so a fatal fall must
                       not soft-lock the demo: once the anim has played out and
                       the pad is touched, she gets back up. Replace this with a
                       real death/reload once the menu is in. */
                    if (lanim_done() && pad) { g_dead = 0; g_fally = 0;
#ifdef ENEMIES
                        g_health = 1000;   /* revive on respawn */
#endif
                    }
                } else if (!grounded) {
                    /* TR1: a JUMP holds its own pose; walking off a ledge (or
                       an up-jump once it starts descending) plays FALL. */
                    int fallish = !g_jumped || (!g_lajf && g_lavy > 0);
                    /* TR1 REACH (anim 94): ACTION held while descending is the
                       arms-out grab pose, not a plain fall. */
                    lanim_set(((pad & PAD_B) || g_autograb) && g_lavy > 0
                                                 ? LANIM_REACH :
                              /* a BACK jump past its apex becomes FALL_BACK
                                 (anim 93), not the launch pose */
                              (g_jdir == 1 && g_lavy > 0) ? LANIM_FALLBACK :
                              fallish            ? LANIM_FALL     :
                              g_jdir == 1        ? LANIM_BACKJUMP :
                              g_jdir == 2        ? LANIM_LEFTJUMP :
                              g_jdir == 3        ? LANIM_RIGHTJUMP:
                              g_lajf             ? LANIM_FJUMP    : LANIM_UPJUMP);
                    lanim_step(0, 1);
                } else if (g_sliding) {
                    lanim_set(g_sliding == 2 ? LANIM_SLIDEBACK : LANIM_SLIDE);
                    lanim_step(1, 1);
#if defined(MULTIROOM) && defined(ENTITIES) && defined(SWANIM)
                } else if (g_swact > 0) {
                    lanim_set(LANIM_SWITCH);
#ifdef SWDEBUG
                    lanim_step(1, 1);           /* DEBUG: loop the pull cycle */
                    g_swact = 99;               /* DEBUG: never expire        */
#else
                    lanim_step(0, 1);            /* ONE-SHOT, does not loop */
                    /* hold the lock for the anim's OWN length then release -
                       cleaner than a fixed 45-tick guess that held the end
                       frame. lanim_done() = reached the last keyframe. */
                    if (lanim_done()) g_swact = 0;
#endif
#endif
                } else if (jprep) {
                    /* the anticipation crouch before a jump — jprep already
                       existed as a 2-frame launch delay, it just had no anim */
                    lanim_set(LANIM_COMPRESS);
                    lanim_step(1, 1);
                } else if (mv > 0) {
                    /* PHASE-LOCKED FOOTSTEPS: fire when the RUN/WALK cycle
                       actually passes a footfall frame, not on a tick timer.
                       Two footfalls per cycle, placed a half-cycle apart and
                       expressed as a FRACTION of the animation so they hold for
                       either cycle and whatever length the asset has.
                       LARA_FOOT1/2 are eighths of the cycle - tune by ear, or
                       replace both with TR1's own anim SOUND COMMANDS, which
                       carry the exact frames (the extractor skips that array
                       today). */
                    lanim_set((pad & PAD_C) ? LANIM_WALK : LANIM_RUN);
                    lanim_step(1, 2);
                    lara_footstep(rd16(g_sk_anims + g_lanim_id * 6 + 2),
                                  g_lanim_fr, g_lanim_id);
                } else if (mv < 0) {
                    /* TR1 has two retreats: the measured BACK step-back, and
                       FAST_BACK (anim 88) when she is hurrying. Running back
                       (no WALK modifier) gets the fast one. */
                    lanim_set((pad & PAD_C) ? LANIM_BACK : LANIM_FASTBACK);
                    lanim_step(1, 2);
                    lara_footstep(rd16(g_sk_anims + g_lanim_id*6 + 2),
                                  g_lanim_fr, g_lanim_id);
                } else if (side) {
                    lanim_set(side < 0 ? LANIM_STEPL : LANIM_STEPR);
                    lanim_step(1, 1);
                    lara_footstep(rd16(g_sk_anims + g_lanim_id*6 + 2),
                                  g_lanim_fr, g_lanim_id);
                } else if (pad & (PAD_LEFT|PAD_RIGHT)) {
                    /* TR1 escalates a held turn-in-place into FAST_TURN
                       (anim 44) after about half a second. g_turnf counts
                       TICKS, not frames, so the threshold is frame-rate
                       independent like the rest of the movement. */
                    lanim_set((g_turnf > FASTTURN_TICKS) ? LANIM_FASTTURN
                              : ((pad & PAD_LEFT) ? LANIM_TURNL : LANIM_TURNR));
                    lanim_step(1, 1);
                    /* TR1's turn-in-place anims carry footfalls too (anim 12 =
                       frames 3 and 12) — she shuffles her feet round */
                    lara_footstep(rd16(g_sk_anims + g_lanim_id*6 + 2),
                                  g_lanim_fr, g_lanim_id);
                } else {
                    lanim_set(LANIM_IDLE);   /* relaxed breathing stand (103),
                                                not the weapons-ready stance */
                    lanim_step(1, 1);
                }
              }
              lara_done: ;
              g_flr_limit = 0;
            }
#ifdef SKUNK_CONSOLE
            if (frame_count < 200) dbg_kv("bc_move", 1);
#endif
#ifdef HEADSPIN
            cY = COS(0); sY = SIN(0);   /* diagnostic: pinned camera, Lara spins */
#else
#ifdef BUSPROBE
            { uint32_t _k = vp_tick(); if (bp_s0) bp_sC += _k - bp_s0; bp_s0 = _k; }
#endif
            /* CAMERA LAG (2026-08-07, user: PSX turns are smooth/incremental,
               ours jerky): the camera used to be WELDED to Lara's yaw, so a
               turn snapped the whole world by the frame's full step. The PSX
               camera swings behind her with a spring. camyaw eases toward
               g_layaw (shortest path) at ~1/4 of the gap per 30Hz tick, in
               8.8 sub-units so the ease is smooth at any rate; Lara visibly
               rotates WITHIN the frame while the world pans over ~0.3s. */
            { static int camyaw8 = -1;
              int tgt8 = (int)g_layaw << 8, d8, tk2;
              if (camyaw8 < 0) camyaw8 = tgt8;
              d8 = ((tgt8 - camyaw8 + 32768) & 65535) - 32768;
              for (tk2 = g_ticks >> 1; tk2 > 0; tk2--)
                  { d8 = ((((int)g_layaw << 8) - camyaw8 + 32768) & 65535) - 32768;
                    camyaw8 = (camyaw8 + (d8 >> 2)) & 65535;
                    if (d8 && (d8 >> 2) == 0)      /* never stall the tail */
                        camyaw8 = (camyaw8 + (d8 > 0 ? 1 : -1)) & 65535; }
              { int cy1 = (camyaw8 >> 8) & 255, cy2 = (cy1 + 1) & 255, fr8 = camyaw8 & 255;
                cY = COS(cy1) + (((COS(cy2) - COS(cy1)) * fr8) >> 8);
                sY = SIN(cy1) + (((SIN(cy2) - SIN(cy1)) * fr8) >> 8); } }
#endif
            camx = g_lax - (int)(((int32_t)sY*CAMDIST)>>16);
            camz = g_laz - (int)(((int32_t)cY*CAMDIST)>>16);
            camy = g_lafloor - CAMHEIGHT;
            g_camx = camx; g_camy = camy; g_camz = camz;   /* for Lara depth sort */
            camblk[0]=(uint32_t)(cY>>4); camblk[1]=(uint32_t)(sY>>4);
            camblk[2]=(uint32_t)(cP>>4); camblk[3]=(uint32_t)(sP>>4);
            camblk[4]=(uint32_t)camx; camblk[5]=(uint32_t)camy; camblk[6]=(uint32_t)camz; camblk[7]=0;
            pcl_cY4=cY>>4; pcl_sY4=sY>>4; pcl_cP4=cP>>4; pcl_sP4=sP>>4;
            pcl_camx=camx; pcl_camy=camy; pcl_camz=camz;
#define HB(col) do { extern void *video_fb_n(int); int hb_i, hx; \
    for (hb_i=0;hb_i<3;hb_i++) { uint8_t *hb=(uint8_t*)video_fb_n(hb_i); \
    for (hx=0;hx<6;hx++) hb[1*RENDER_W + 8 + (col)*8 + hx] = \
        (frame_count & 1) ? 255 : 254; } } while (0)
            HB(0);   /* stage 0: frame logic done, camera built */
#ifdef SKUNK_CONSOLE
            if (frame_count < 200) dbg_kv("bc_frame", frame_count);
#endif

            /* PORTAL-DEPTH painter order. Visible set = rooms within 2 portal
               hops of Lara's room (1 hop showed BLACK HOLES through a
               neighbour's own doorways/windows). Paint by hop depth DESC
               (2-hop first, 1-hop, current room LAST) so a room always
               repaints its own walls/floor over anything leaking from behind
               them (blue gym mats over the hall floor); ties broken far-first
               by centre distance. Doorways still show through (portal holes
               have no faces). */
            { int a, b, c2, d2;
              for (i=0;i<roomCount;i++) { rdepth[i]=4; prv[i]=0; } /* 4 = off */
              rdepth[g_curroom]=0; prv[g_curroom]=2;            /* full rect */
              for (a=0;a<MRT_ADJ_MAX && S_adj[g_curroom][a]!=255;a++) {
                  int n1=S_adj[g_curroom][a];
                  if (rdepth[n1]>1) rdepth[n1]=1;
                  for (b=0;b<MRT_ADJ_MAX && S_adj[n1][b]!=255;b++) {
                      int n2=S_adj[n1][b];
                      if (rdepth[n2]>2) rdepth[n2]=2;
                      for (c2=0;c2<MRT_ADJ_MAX && S_adj[n2][c2]!=255;c2++)
                          if (rdepth[S_adj[n2][c2]]>3) rdepth[S_adj[n2][c2]]=3;
                  }
              }
              /* RECT CHAIN in depth order: a room's window = union over its
                 shallower neighbours of intersect(neighbour window, doorway
                 rect neighbour->room). Empty window = not drawn AT ALL. */
              for (d2=1; d2<=3; d2++)
                for (i=0;i<roomCount;i++) {
                  int got=0, ux0=0,ux1=0,uy0=0,uy1=0;
                  if (rdepth[i]!=d2) continue;
                  for (a=0;a<MRT_ADJ_MAX && S_adj[i][a]!=255;a++) {
                    int nb=S_adj[i][a];
                    int a0,a1,b0,b1, r1;
                    if (rdepth[nb]!=d2-1 || prv[nb]==0) continue;
                    r1 = room_link_rect(nb, i, &a0,&a1,&b0,&b1);
                    if (!r1) continue;
                    if (r1==2) { a0=0;a1=319;b0=0;b1=RENDER_H-1; }
                    if (prv[nb]==1) {   /* clip through the neighbour's window */
                        if (prx0[nb]>a0) a0=prx0[nb];
                        if (prx1[nb]<a1) a1=prx1[nb];
                        if (pry0[nb]>b0) b0=pry0[nb];
                        if (pry1[nb]<b1) b1=pry1[nb];
                        if (a0>a1 || b0>b1) continue;
                    }
                    if (!got) { ux0=a0;ux1=a1;uy0=b0;uy1=b1;got=1; }
                    else { if(a0<ux0)ux0=a0; if(a1>ux1)ux1=a1;
                           if(b0<uy0)uy0=b0; if(b1>uy1)uy1=b1; }
                    if (ux0==0 && ux1==319 && uy0==0 && uy1==RENDER_H-1) break;
                  }
                  if (got) { prv[i]=1; prx0[i]=ux0; prx1[i]=ux1;
                             pry0[i]=uy0; pry1[i]=uy1; }
                } }
#ifdef M68D_A2
            /* PERFHUNT A2: sort ONLY the rooms the admit loop can accept
               (rdepth<=3 && prv!=0 — its own first filters), with the
               Manhattan distance hoisted out of the O(n^2) inner loop
               (legacy recomputed BOTH distances every inner iteration:
               38 rooms -> 703 iters x 2 iabs pairs, the main+0x1500 hot
               bucket).  Same comparator + same ascending scan order =
               identical relative order of every admitted room; skipped
               rooms never influenced comparisons.  g_m68d_nord bounds the
               admit loop below. */
            { int nc = 0, cdist[64];
              for (i=0;i<roomCount;i++)
                  if (rdepth[i]<=3 && prv[i]) {
                      order[nc]=i;
                      cdist[nc]=mr_iabs(rcx[i]-camx)+mr_iabs(rcz[i]-camz);
                      nc++; }
              for (i=0;i<nc-1;i++) {
                  int j, best=i;
                  for (j=i+1;j<nc;j++) {
                      if (rdepth[order[j]]>rdepth[order[best]] ||
                          (rdepth[order[j]]==rdepth[order[best]] &&
                           cdist[j]>cdist[best])) best=j;
                  }
                  { int t=order[i]; order[i]=order[best]; order[best]=t;
                    t=cdist[i]; cdist[i]=cdist[best]; cdist[best]=t; }
              }
              g_m68d_nord = nc; }
#else
            for (i=0;i<roomCount;i++) order[i]=i;
            for (i=0;i<roomCount-1;i++) {
                int j, best=i;
                for (j=i+1;j<roomCount;j++) {
                    int rb=order[best], rj=order[j];
                    int db=mr_iabs(rcx[rb]-camx)+mr_iabs(rcz[rb]-camz);
                    int dj=mr_iabs(rcx[rj]-camx)+mr_iabs(rcz[rj]-camz);
                    if (rdepth[rj]>rdepth[rb] ||
                        (rdepth[rj]==rdepth[rb] && dj>db)) best=j;
                }
                { int t=order[i]; order[i]=order[best]; order[best]=t; }
            }
#endif
#ifdef M68PAD
            /* AUTOPSY DISCRIMINATOR (2026-07-22): burn ~M68PAD iterations of
               DRAM-touching 68k busy-work at the exact point the M68DIET
               savings came out of the frame (end of the hl_logic segment,
               inside the PIPELINE overlap window).  If M68DIET+M68PAD
               recovers ref fps on silicon, the toxin is the CHANGED 68k
               TIMELINE (duration/phase), not the diet code itself; if it
               stays slow, the toxin is in a specific piece (bisect via
               M68A1/M68A2/M68A3).  ~5 instructions + 1 DRAM data read per
               iteration; M68PAD=7000 modeled ~= the removed ~34K
               instructions/render at spawn. */
            { static volatile int m68pad_sink;
              int _pi, _ps = 0;
              for (_pi = 0; _pi < (M68PAD); _pi++) _ps += rcx[_pi & 31];
              m68pad_sink = _ps; }
#endif
#ifdef PROFILE
            pfA = frame_count;
#endif
#ifdef BUSPROBE
            { uint32_t _k = vp_tick(); if (bp_s0) bp_sD += _k - bp_s0; bp_s0 = _k; }
#endif
            HLP(1);
#ifdef BUSPROBE
            { uint32_t _m = vp_tick(); if (bp_m) bp_p1 += _m - bp_m; bp_m = _m; }
            bp_active += vp_tick() - bp_t0;      /* loop top -> here: ACTIVE */
#endif
#if defined(PIPELINE) && PIPESTAGE >= 1 && !defined(COLLECTEARLY)
            /* PIPELINE collect point: the logic above ran while Tom finished
               the previous frame. Present it before any blitter (clear) or
               pose (lara_blob) work — both would collide with a live render. */
#ifdef PACEPROBE
            { uint32_t _n = PPNOW();
              if (pp_prev0) { uint32_t _d = _n - pp_prev0;
                  if (_d < 60000u) { pp_per += _d; if (_d > pp_pmax) pp_pmax = _d; } }
              pp_prev0 = _n; }
#endif
            if (g_tominflight) {
#ifdef PACEPROBE
                { uint32_t _t0 = PPNOW();
                  gpu_sync(); g_tominflight = 0;
                  { uint32_t _t1 = PPNOW();
                    uint32_t _w = _t1 - _t0, _s = _t1 - pp_kick;
                    pp_coll++;
                    PPADD(pp_wait, _w);
                    PPADD(pp_span, _s);
                    if (_w < 60000u && _w > pp_wmax) pp_wmax = _w;
                    if (_s < 60000u && _s > pp_smax) pp_smax = _s; } }
#else
#ifdef BUSPROBE
                { uint32_t _s0 = vp_tick();
                  gpu_sync(); g_tominflight = 0;
                  bp_sleep += vp_tick() - _s0; }
#else
                gpu_sync(); g_tominflight = 0;
#endif
#endif
            }
#ifdef BUSPROBE
            bp_n++; bp_frames++;
            /* ☠️ THROW THE FIRST WINDOWS AWAY.  bp_t0 starts at zero, so the
               first sample is "time since boot" - seconds - and it dominated
               the running mean forever (the first reading came back as 1.8s a
               frame, which is nonsense on its face). */
            if (bp_n == 8) { bp_active = bp_sleep = bp_safe = bp_blit = 0;
                             bp_p1 = bp_p2 = bp_p3 = bp_p4 = 0;
                             bp_sA = bp_sB = bp_sC = bp_sD = 0; bp_n = 1; }
            bp_t0 = vp_tick();                   /* next window starts here */
#endif
#ifdef JLOOPS
            /* JERRY HEARTBEAT, ON-SCREEN (2026-07-25).  Console readout is not
               available: a NOGD build black-screened the board — SKUNK_CONSOLE's
               startup handshake, the failure the ledger already records twice.
               So draw it instead.
               PLACEMENT IS THE WHOLE TRICK: right after gpu_sync (Tom is IDLE)
               and BEFORE the flip, so `video_backbuffer()` is still the frame
               Tom just finished.  This is why the normal PROFILE bar strip is
               #if'd out under PIPELINE — that one does a Blitter `blit_band`
               grab while Tom is mid-render and wedged the kernel.  These are
               plain 68k stores into an idle buffer: no Blitter, no grab.
                 row 4 = jloops (Jerry main_loop passes per 60 renders) x4 px
                 row 8 = jrend  (60 renders) x4 px = a FIXED 240 px reference
               BARS EQUAL  -> Jerry keeps up; he idle-polls between poses, and
                              jagemu's "100% busy" was a model artifact.
               TOP BAR SHORT -> Jerry OVERRUNS the frame, and jerry_pose_sync
                              (2.23% of wall) understates his real cost. */
            { static uint32_t jl_prev; static int jl_a, jl_b, jl_n;
              if (++jl_n >= 60) {
                  uint32_t j = *(volatile uint32_t *)0xF1C0B0u;
                  jl_a = (int)(j - jl_prev) * 4;  jl_prev = j;
                  jl_b = jl_n * 4;  jl_n = 0;
                  if (jl_a > RENDER_W-1) jl_a = RENDER_W-1;
                  if (jl_a < 0)         jl_a = 0;
                  if (jl_b > RENDER_W-1) jl_b = RENDER_W-1;
              }
              { uint8_t *jfb = (uint8_t *)video_backbuffer(); int xx;
                for (xx = 0; xx < RENDER_W; xx++) {
                    jfb[4*RENDER_W+xx] = 0; jfb[8*RENDER_W+xx] = 0; }
                for (xx = 0; xx < jl_a; xx++) jfb[4*RENDER_W+xx] = 255;
                for (xx = 0; xx < jl_b; xx++) jfb[8*RENDER_W+xx] = 255; } }
#endif
#ifdef LARACOUNT
            /* LARA CULL READOUT (2026-07-26).  Same placement rule as the JLOOPS
               bars: right after gpu_sync (Tom IDLE) and BEFORE the flip, plain
               68k stores into the finished frame — no Blitter, no grab.
                 row 12 = HER faces STAGED    /2 px
                 row 16 = HER faces RASTERED  /2 px
                 row 20 = HER faces CULLED    /2 px   (staged - rastered)
               She has 375 faces => a full bar is ~187 px at /2, so nothing
               clamps (x2 saturated the 320 px screen and read as 100%).
               Compare the CULLED bar against jagemu's 63.6%: a LONGER bar on
               silicon is the see-through hole, measured. */
            { volatile uint32_t *lc = (volatile uint32_t *)0x001C0008u;
              uint32_t st = lc[0], ra = lc[1];
              uint8_t *cfb = (uint8_t *)video_backbuffer(); int xx;
              int bs = (int)(st>>1), br = (int)(ra>>1);
              int bc = (int)((st > ra ? st-ra : 0) >> 1);   /* guard underflow */
              if (bs > RENDER_W-1) bs = RENDER_W-1;
              if (br > RENDER_W-1) br = RENDER_W-1;
              if (bc > RENDER_W-1) bc = RENDER_W-1;
              for (xx = 0; xx < RENDER_W; xx++) {
                  cfb[12*RENDER_W+xx]=0; cfb[16*RENDER_W+xx]=0; cfb[20*RENDER_W+xx]=0; }
              for (xx = 0; xx < bs; xx++) cfb[12*RENDER_W+xx] = 255;
              for (xx = 0; xx < br; xx++) cfb[16*RENDER_W+xx] = 255;
              for (xx = 0; xx < bc; xx++) cfb[20*RENDER_W+xx] = 255;
              lc[0] = 0; lc[1] = 0; }     /* per-FRAME counts, not cumulative */
#endif
#ifdef TRAVDIAG
            /* WHAT DOES THE LEDGE IN FRONT OF LARA ALLOW?
             * The opening canyon asks for a vault ~100 times and a
             * jump-grab-pull-up ~48 times (step census off the level data), so
             * "traversal is broken" has several possible links.  This probes
             * the same spot the vault check probes and prints the verdict, so
             * one screencast says WHICH rule is refusing rather than leaving
             * it to be guessed:
             *   R    = rise in front, in world units (256 = one click)
             *   V    = verdict: 0 nothing/flat, 1 walk-up, 2 VAULT,
             *          3 needs jump+grab, 4 too tall (wall), 9 no floor found
             *   G    = grab state: hands would reach a ledge this frame
             *   S    = Lara's state: 1 vaulting, 2 airborne, 3 hanging
             * Thresholds shown so the numbers can be read against them:
             * step 256, vault 768, hang reach 720. */
            { uint8_t *dfb3 = (uint8_t *)video_backbuffer();
              char tds[40]; int tdp = 0, k6;
              int px6 = g_lax + (int)(((int32_t)SIN(g_layaw)*(WALK_SPEED*2))>>16);
              int pz6 = g_laz + (int)(((int32_t)COS(g_layaw)*(WALK_SPEED*2))>>16);
              int lf6 = 0, rise6 = 0, ok6, verdict;
              uint32_t vals[4];
              g_flr_grab = 1;
              ok6 = room_floor_mr(rsect, roomCount, px6, pz6, &lf6);
              g_flr_grab = 0;
              if (!ok6) verdict = 9;
              else {
                  rise6 = g_lafloor - lf6;
                  if (rise6 <= 0)                    verdict = 0;
                  else if (rise6 <= LARA_STEPUP)     verdict = 1;
                  else if (rise6 <= LARA_CLIMB)      verdict = 2;
                  else if (rise6 <= 2048)            verdict = 3;
                  else                               verdict = 4;
              }
              vals[0] = (uint32_t)(rise6 < 0 ? -rise6 : rise6);
              vals[1] = (uint32_t)verdict;
              vals[2] = (uint32_t)(rise6 > 0 && rise6 <= LARA_GRABREACH + 64);
              vals[3] = (uint32_t)(g_vault ? 1 : (g_lavy != 0 ? 2 :
                                   (g_autograb ? 3 : 0)));
              for (k6 = 0; k6 < 4 && tdp < 34; k6++) {
                  uint32_t v = vals[k6]; char dd[8]; int nd = 0;
                  tds[tdp++] = (char)("RVGS"[k6]);
                  if (!v) dd[nd++] = '0';
                  while (v && nd < 7) { dd[nd++] = (char)('0' + v % 10u); v /= 10u; }
                  while (nd) tds[tdp++] = dd[--nd];
                  tds[tdp++] = ' ';
              }
              tds[tdp] = 0;
              menu_text(dfb3, RENDER_W, RENDER_H, tds, 4, 14, 1, 2, 255); }
#endif
#ifdef FPSBEACON
            /* content-independent frame clock: a 32x8 block toggling on every
               PUBLISHED frame.  Written after gpu_sync (Tom idle) and before
               the flip.  Transitions ARE flips, whatever is on screen. */
            { static uint8_t bk = 0; uint8_t *bfb = (uint8_t *)video_backbuffer();
              int by, bx; uint8_t v = (bk ^= 1) ? 255 : 0;
              for (by = 24; by < 32; by++)
                  for (bx = 32; bx < 64; bx++) bfb[by*RENDER_W + bx] = v; }
#endif
#if defined(BUSPROBE) || defined(STAGECHK)
            /* READ THE NUMBERS, DO NOT DECODE THEM.
             * Three bar-decoders failed today - black-for-zero bars that could
             * not be told from "never painted", a sampler that read the
             * capture's pillarbox, and a cell decoder that reported 711 fps.
             * menu_text already renders "R0" legibly in every capture, so the
             * counters go through the SAME renderer and I read them off the
             * frame.  Nothing to misdecode.
             * ☠️ keep it left of x~240: menu_text past that does not display. */
            { uint8_t *tfb2 = (uint8_t *)video_backbuffer();
              char ts[44]; int tp, ti, ln;
              uint32_t tv[5];
              /* ☠️ CLAMP BEFORE SCALING.  (sum/n)*317 overflowed 32 bits and
                 printed a six-digit millisecond count; 60000 halflines is
                 1.9s, far past any real phase, and 60000*317 still fits. */
              for (ln = 0; ln < 2; ln++) {
                if (ln == 0) {
                    tv[0] = bp_frames;
                    tv[1] = bp_n ? bp_p1 / bp_n : 0;
                    tv[2] = bp_n ? bp_p2 / bp_n : 0;
                    tv[3] = bp_n ? bp_p3 / bp_n : 0;
                    tv[4] = bp_n ? bp_p4 / bp_n : 0;
                } else {
                    tv[0] = bp_n ? bp_sA / bp_n : 0;
                    tv[1] = bp_n ? bp_sB / bp_n : 0;
                    tv[2] = bp_n ? bp_sC / bp_n : 0;
                    tv[3] = bp_n ? bp_sD / bp_n : 0;
                    tv[4] = 0;
                }
                tp = 0;
                for (ti = 0; ti < 5 && tp < 38; ti++) {
                    uint32_t v = tv[ti]; char d[8]; int nd = 0;
                    if (ln || ti) { if (v > 60000u) v = 60000u;
                                    v = v * 317u / 10000u; }
                    ts[tp++] = (char)((ln ? "abcd " : "FPQRS")[ti]);
                    if (!v) d[nd++] = '0';
                    while (v && nd < 7) { d[nd++] = (char)('0' + v % 10u); v /= 10u; }
                    while (nd) ts[tp++] = d[--nd];
                    ts[tp++] = ' ';
                }
                ts[tp] = 0;
                if ((bp_frames & 3u) == 0)
                    menu_text(tfb2, RENDER_W, RENDER_H, ts, 4,
                              (short)(14 + ln*10), 1, 2, 255);
              } }
#endif
#ifdef DBGROOM
            /* ROOM AUDIT (2026-08-03): draw Lara's LOCAL room index top-left so
               every capture is labelled while walking the whole level looking
               for broken rooms.  Overlay into the finished frame before flip. */
            { uint8_t *dfb = (uint8_t *)video_backbuffer();
              char rs[20]; int rn = g_curroom, p = 0;
              rs[p++]='R';
              if (rn >= 10) rs[p++] = (char)('0' + (rn/10)%10);
              rs[p++] = (char)('0' + rn%10);
#ifdef VIDDIAG
              /* Tom context probe verdict: J1 = micro-kernel RAN at game
                 entry (video context is the fault), J2 = failed there too
                 (the loader is), nothing = probe never executed */
              if (g_jvmin_game) {
                  int vb9 = (int)(g_jv_vfy > 9 ? 9 : g_jv_vfy);
                  rs[p++] = 'J';
                  rs[p++] = (char)('0' + g_jvmin_game);
                  rs[p++] = (char)('0' + g_jvmin2);
                  rs[p++] = 'V';
                  rs[p++] = (char)('0' + vb9);
                  rs[p++] = 'D';
                  rs[p++] = (char)('0' + g_jv_d240);
              }
              { int k5;
                rs[p++] = 'S';
                for (k5 = 0; k5 < 4; k5++)
                    rs[p++] = (g_sr[k5] == 99) ? 'X'
                             : (char)('0' + (g_sr[k5] > 9 ? 9 : g_sr[k5]));
              }
#endif
              rs[p] = 0;
              menu_text(dfb, RENDER_W, RENDER_H, rs, 4, 2, 1, 2, 255); }
#endif
#ifdef ENEMIES
            /* HEALTH BAR: raw-pixel bar at the TOP-LEFT (the JLOOPS/LARACOUNT
               bars prove raw writes at x=0+ display; the right half of the
               320px fb and menu_text past x~240 do NOT). White fill scales with
               health; a low-health slice flips to gold (240). */
            { uint8_t *hfb = (uint8_t *)video_backbuffer();
              char hs[12]; int p=0, h=g_health>0?g_health:0;
              hs[p++]='0'+(h/1000)%10; hs[p++]='0'+(h/100)%10;
              hs[p++]='0'+(h/10)%10; hs[p++]='0'+h%10;
              hs[p]=0;
              menu_text(hfb, RENDER_W, RENDER_H, hs, 4, 14, 1, 2, 255); }
#endif
            if (g_pipeframe)   { video_flip(); g_pipeframe = 0; }
#endif
#ifdef FARDIAL
            /* kernel far-cull distance: GPU SRAM poke — MUST be post-collect
               (Tom idle here) and pre-kick. */
            *(volatile uint32_t *)0xF03F3Cu = (uint32_t)g_fardist;
#endif
            fb = video_backbuffer();
            /* the ~76KB Blitter phrase-fill saturates the bus for 1-2ms; if
               it covers the vblank ISR's OP-list rebuild window the top
               scanlines drop for a field (the "bounce"). Dodge the window
               (see video_wait_safe_vc in video.c). */
#ifdef PACEPROBE
            { uint32_t _ta = PPNOW();
              { extern void video_wait_safe_vc(void); video_wait_safe_vc(); }
              { uint32_t _tb = PPNOW();
                blit_band(fb, 0, RENDER_H, CLEAR_IDX);
                { uint32_t _tc = PPNOW();
                  PPADD(pp_safe, _tb - _ta);
                  PPADD(pp_blit, _tc - _tb); } } }
#else
#ifdef BUSPROBE
            { uint32_t _q0 = vp_tick();
              { extern void video_wait_safe_vc(void); video_wait_safe_vc(); }
              bp_safe += vp_tick() - _q0; }
#else
            { extern void video_wait_safe_vc(void); video_wait_safe_vc(); }
#endif
#ifndef NOCLEAR
#ifdef BUSPROBE
            { uint32_t _q1 = vp_tick();
              blit_band(fb, 0, RENDER_H, CLEAR_IDX);
              bp_blit += vp_tick() - _q1; }
#else
            blit_band(fb, 0, RENDER_H, CLEAR_IDX);
#endif
#else /* NOCLEAR */
            /* NOCLEAR (2026-07-31): do not clear the framebuffer. The missing
               faces show BLACK only because we clear to CLEAR_IDX and the
               dropped face never overwrites it. Leave the previous contents
               and a face that drops out for one frame keeps its old pixels
               instead of punching a black hole - the intermittent drops we
               have failed to root-cause become far less visible. It is also
               one less full-screen blit per frame.
               COST: anywhere genuinely empty (past the room, sky) smears the
               previous frame instead of reading black, and with two buffers
               the residue is two frames old, so fast camera motion will
               streak. Judge it by eye - this masks the symptom, it does not
               fix the bug. */
#endif /* NOCLEAR */
#endif /* PACEPROBE */
            HLP(2);
#ifdef BUSPROBE
            { uint32_t _m = vp_tick(); if (bp_m) bp_p2 += _m - bp_m; bp_m = _m; }
#endif
#ifdef PROFILE
            pfB = frame_count;
#endif
            if (gpu_ok) {
                RP_ALL();
                int ndrawn = 0;
#ifdef ABLADDER
                int abinc = 0;   /* rooms admitted this frame (cap g_abrooms) */
#endif
                int posed = 0;
                int lara_disp = 0;
                njx = 0;
#ifdef JERRYPOSE
                if (g_jerry_ok) {
                    /* THREE-PROCESSOR FRAME: Jerry poses Lara while Tom draws
                       rooms and the 68k orchestrates. Header now, verts by
                       Jerry, faces by lara_finish after sync. */
                    extern void jerry_pose_kick(const void*,const void*,const void*,
                        const void*,const void*,void*,int32_t,int32_t,int32_t,
                        int32_t,int32_t,int32_t,int32_t,int32_t,uint32_t);
                    const uint8_t *rfr = g_sk_frames + g_anim_start * g_sk_framestride;
                    const uint8_t *ang = g_sk_frames + g_lframe * g_sk_framestride + 6;
                    int offX = g_lax >> 8, offZ = g_laz >> 8;
                    uint16_t *h = (uint16_t *)lara_blob;
                    h[0]=(uint16_t)g_lnv; h[1]=(uint16_t)g_lnq; h[2]=(uint16_t)g_lnt;
                    h[3]=(uint16_t)atlasW; h[4]=(uint16_t)g_ltx_atH;
                    h[5]=(uint16_t)offX; h[6]=0; h[7]=(uint16_t)offZ;
                    jerry_pose_kick(skv, skvl, sknode, ang, (void*)0, lara_blob+16,
                        rd16(rfr), rd16(rfr+2), rd16(rfr+4),
                        COS(g_layaw)>>2, SIN(g_layaw)>>2,
                        g_lax & 255, g_laz & 255, g_lay + LARA_FEET,
                        (uint32_t)g_sk_mcount);
                    posed = 2;   /* 68k pose halves not needed */
                }
#endif
                /* frustum cull: skip rooms behind the camera or past the far clip.
                   The 5-room draw was ~60% of the frame and most aren't visible. */
                RP(1);
                for (k=0;k<M68D_ORD_N;k++) {
                    int ri = order[k];
                    /* 32-bit depth (no long long under jcc68k): pre-shift
                       so the products fit — dx/16 (<=~6250) * trig/256
                       (<=256) then >>4 restores the >>16 scale. Precision
                       cost <= a few hundred world units, well inside the
                       +-rrad conservative margins below. */
                    int dxl=(rcx[ri]-camx)>>4, dzl=(rcz[ri]-camz)>>4;
                    int depth = (dxl*(sY>>8) + dzl*(cY>>8)) >> 4;
#ifdef ROOMCAP
                    /* perf experiment: draw only the ROOMCAP NEAREST rooms
                       (order[] is far-first, so the nearest are the last). */
                    if (k < roomCount - ROOMCAP) continue;
#endif
#ifdef CURONLY
                    /* A10-free diagnostic: draw ONLY the room Lara is in. If the
                       staged/rastered discard ratio collapses, the wasted
                       transforms are in NEIGHBOUR rooms - faces of a room seen
                       through a small doorway, transformed then thrown away by
                       the portal-window clip. That distinguishes "needs per-face
                       frustum test" from "needs tighter portal rects". */
                    if (ri != g_curroom) continue;
#endif
#ifndef NOVISCULL
                    /* PORTAL VISIBILITY: draw rooms within 2 portal hops of
                       Lara's room (rdepth computed with the paint order above;
                       1 hop left black holes through neighbours' doorways).
                       Build with NOVISCULL=1 to disable. */
                    if (rdepth[ri] > 3) continue;
                    if (prv[ri] == 0) continue;    /* no visible window */
#endif
                    /* conservative: cull only if the whole room sphere is behind
                       the camera or entirely past the far clip (never the room
                       you're in / partly in view). */
                    if (depth + rrad[ri] < 0)      continue;   /* fully behind   */
                    /* FARCLIP=N (2026-07-25): room-level far cull distance.
                       Was a bare 9000. Made settable to measure what draw
                       distance is still worth cutting now that Tom is 90.3%
                       saturated and is the thing to cut work FROM — note the
                       portal-hop dial (HOPBOOT) is already at its tight end
                       (1 = current room + neighbours), so this and the sliver
                       thresholds are what remain. */
#ifndef FARCLIP
#define FARCLIP 9000
#endif
                    if (depth - rrad[ri] > FARCLIP) continue;  /* fully past far */
                    /* SLIVER CULL: a room seen only through a tiny distant
                       window costs full per-face setup for ~nothing on
                       screen (long corridors stacked 10+ rooms = the dip).
                       Skipping it leaves a few dark pixels in a doorway. */
                    /* SLIVERW/SLIVERH (2026-07-25): the sliver thresholds made
                       settable.  Now that Tom is 90.3% saturated and the
                       portal-hop dial is already at its tight end (so the room
                       far clip is provably inert — see the FARCLIP commit),
                       this is the LAST knob that trades picture for Tom cycles.
                       Note the defaults were chosen when the framebuffer was
                       240 lines; at LOWRES's 120 a 6px height is 5% of the
                       screen, so the vertical threshold is effectively already
                       twice as aggressive as it reads. */
#ifndef SLIVERW
#define SLIVERW 16
#endif
#ifndef SLIVERH
#define SLIVERH 6
#endif
                    if (ri != g_curroom && prv[ri] == 1 &&
                        (prx1[ri] - prx0[ri] < SLIVERW || pry1[ri] - pry0[ri] < SLIVERH))
                        continue;
#ifdef ABLADDER
                    /* content ladder: admit only the first g_abrooms rooms
                       (loop order); 0 = Lara alone. OPTION+UP/DOWN live. */
                    if (abinc >= g_abrooms) continue;
                    abinc++;
#endif
#ifdef HOPDIAL
                    /* draw-distance dial: portal-hop cap (4 = uncapped) */
                    if (g_hopcap <= 3 &&
                        !room_withinK(g_curroom, ri, g_hopcap,
                                      &g_hop_cached_a, g_hop_inset)) continue;
#endif
                    /* PORTAL-WINDOW CLIP: neighbour rooms render only inside
                       the doorway rect they're seen through; invisible
                       doorway = the room isn't drawn AT ALL. */
                    { int cx0=0, cx1=319, cy0=0, cy1=RENDER_H-1;
#ifndef NOPCLIP
                      /* NOPCLIP=1 (2026-07-30): draw neighbour rooms over the
                         FULL screen instead of clipping them to the doorway
                         rect. The rect is recomputed every frame from the
                         projected portal, so a wrong or unstable one carves a
                         hard AXIS-ALIGNED hole and makes the region blink -
                         which is the only single mechanism that explains both
                         the rectangular black notch and the twitching. If both
                         vanish with this on, the fault is portal_rect. */
                      if (ri != g_curroom && prv[ri] == 1) {
                          cx0=prx0[ri]; cx1=prx1[ri];
                          cy0=pry0[ri]; cy1=pry1[ri];
                      }
#endif
#ifdef NODISPATCH
                      /* bisect build: per-room kicks with the portal rects */
                      gpu_geotex_setclip(cx0,cx1,cy0,cy1);
                      if (posed < 2) {
                          gpu_geotex_kick(rgeom[ri], fb, camblk, S_atlas, (uint32_t)atlasW);
                          if (posed == 0) build_lara_part(lara_blob, atlasW, 0, 99);
                          posed = 2;
                          gpu_sync();
                      } else {
                          gpu_geotex(rgeom[ri], fb, camblk, S_atlas, (uint32_t)atlasW);
                      }
#else
                      /* SINGLE-DISPATCH: queue room + clip rect. Rooms
                         after the FIRST get a JERRY cache slot (he
                         transforms their verts while Tom rasters room 0;
                         Tom polls the flag, self-transforms on timeout). */
                      { int rvc = rd16(rgeom[ri]);
                        uint32_t cp = 0;
                        if (ndrawn >= 39) continue;   /* batch-loop cap */
#ifdef NOJX
                        { if (0) {
#else
                        { int rqc = rd16(rgeom[ri]+2), rtc = rd16(rgeom[ri]+4);
                        /* njx cap 5: hand Jerry only what he finishes AHEAD
                           of Tom's polls; the rest self-transform (no wait) */
#ifndef JERRYX
                        /* RETIRED 2026-07-19: handing rooms to Jerry to co-transform
                           ahead of Tom measured 4.59 fps vs 4.59 fps and a 0-pixel
                           render diff — ZERO benefit — while costing a cross-chip
                           protocol: the jcache flag arming dance, and the ordering
                           hazard where a stale flag=2 orphaned in Jerry's posted
                           queue could land AFTER the arm, making Tom trust last
                           frame's cache (stale-camera "geometry towers", HW-seen).
                           Tom self-transforms every room instead.
                           `make JERRYX=1` restores it. */
                        if (0) {
#else
                        if (ndrawn > 0 && rvc <= 512 && rqc <= 448 && rtc <= 256 && njx < 5) {
#endif
#endif
                            cp = (uint32_t)&jcache[njx][4];
                            /* flag armed LATER (post pose-sync) — see kick */
                            jxroom[njx] = rgeom[ri];
                            jxlist[1+njx*5+0] = (uint32_t)(rgeom[ri]+16);
                            jxlist[1+njx*5+1] = (uint32_t)rvc;
                            jxlist[1+njx*5+2] = (uint32_t)(((int32_t)(int16_t)rd16(rgeom[ri]+10))<<8);
                            jxlist[1+njx*5+3] = (uint32_t)(((int32_t)(int16_t)rd16(rgeom[ri]+14))<<8);
                            jxlist[1+njx*5+4] = cp;
                            njx++;
                        }
                        displist[1+ndrawn*4+0] = (uint32_t)rgeom[ri];
                        displist[1+ndrawn*4+1] = ((uint32_t)cx0<<16)|(uint32_t)(uint16_t)cx1;
                        displist[1+ndrawn*4+2] = ((uint32_t)cy0<<16)|(uint32_t)(uint16_t)cy1;
                        displist[1+ndrawn*4+3] = cp; } }
#endif
                    }
                    ndrawn++;
                }
#ifdef NODISPATCH
                if (0) { /* rooms already drawn per-room above */
#else
                /* props join the SAME dispatch (they don't depend on Jerry):
                   door then item, full-screen clips, after all rooms */
                RP(2);
#ifdef ENTITIES
                /* real doors: only those in the room Lara is standing in,
                   and only while still low enough to be visible.  The
                   displist has 8 slots total and the rooms already took
                   most of them, so this is deliberately capped. */
                { int e, nd = 0;
                  for (e = 0; e < MRT_ENTCOUNT && ndrawn < 8; e++) {
                      if (g_useset) break;
                      if (!ent_is_door(mrt_ent[e].type)) continue;
                      /* a door is visible from BOTH sides: keep drawing it
                         after Lara steps through into the next room, else it
                         vanishes.  ☠️ 2026-08-03: was room_within3 (3 hops) -
                         drew doors from rooms visible across a big cave, as a
                         misplaced slab. Now ADJACENT-ONLY + a hard DISTANCE
                         gate so a far door in a big adjacent room can't show. */
                      if (mrt_ent[e].room != (unsigned char)g_curroom &&
                          !room_reachable(g_curroom, mrt_ent[e].room)) continue;
                      { int ddx = g_lax - mrt_ent[e].x, ddz = g_laz - mrt_ent[e].z;
                        if (ddx < 0) ddx = -ddx; if (ddz < 0) ddz = -ddz;
                        if (ddx + ddz > 5120) continue; }   /* ~5 sectors */
                      if (nd >= ENT_DOOR_MAXDRAW) break;
                      build_ent_door(ent_door_blob[nd], atlasW, e);
                      displist[1+ndrawn*4+0] = (uint32_t)ent_door_blob[nd];
                      nd++;
                      displist[1+ndrawn*4+1] = 319u;
                      displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                      displist[1+ndrawn*4+3] = 0;
                      ndrawn++;
                  } }
                /* the wall levers in this room, drawn like the doors */
                { int e, ns = 0;
                  for (e = 0; e < MRT_ENTCOUNT && ndrawn < 8; e++) {
                      if (g_useset) break;
                      if (!ent_is_switch(mrt_ent[e].type)) continue;
                      if (mrt_ent[e].room != (unsigned char)g_curroom &&
                          !room_reachable(g_curroom, mrt_ent[e].room)) continue;
                      { int ddx = g_lax - mrt_ent[e].x, ddz = g_laz - mrt_ent[e].z;
                        if (ddx < 0) ddx = -ddx; if (ddz < 0) ddz = -ddz;
                        if (ddx + ddz > 5120) continue; }
                      if (ns >= ENT_SW_MAXDRAW) break;
                      build_ent_switch(ent_sw_blob[ns], atlasW, e);
                      displist[1+ndrawn*4+0] = (uint32_t)ent_sw_blob[ns];
                      displist[1+ndrawn*4+1] = 319u;
                      displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                      displist[1+ndrawn*4+3] = 0;
                      ns++; ndrawn++;
                  } }
                /* the bridge platforms in this room (were invisible) */
                { int e, nb = 0;
                  for (e = 0; e < MRT_ENTCOUNT && ndrawn < 39; e++) {
                      if (g_useset) break;
                      if (!ent_is_bridge(mrt_ent[e].type)) continue;
                      /* bridges span stacked rooms (18/22) so the entity's room
                         may not equal g_curroom; there is only ONE bridge
                         cluster in the level, so a pure DISTANCE gate is safe
                         and dodges the stacked-room adjacency question. */
                      { int ddx = g_lax - mrt_ent[e].x, ddz = g_laz - mrt_ent[e].z;
                        if (ddx < 0) ddx = -ddx; if (ddz < 0) ddz = -ddz;
                        if (ddx + ddz > 6144) continue; }
                      if (nb >= ENT_BR_MAXDRAW) break;
                      build_ent_bridge(ent_br_blob[nb], atlasW, e);
                      displist[1+ndrawn*4+0] = (uint32_t)ent_br_blob[nb];
                      displist[1+ndrawn*4+1] = 319u;
                      displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                      displist[1+ndrawn*4+3] = 0;
                      nb++; ndrawn++;
                  } }
                /* uncollected pickups near Lara, spinning */
                { int e, np = 0;
                  for (e = 0; e < MRT_ENTCOUNT && ndrawn < 39; e++) {
                      if (g_useset) break;
                      if (!ent_is_pickup(mrt_ent[e].type) || g_pickgot[e]) continue;
                      if (mrt_ent[e].room != (unsigned char)g_curroom &&
                          !room_reachable(g_curroom, mrt_ent[e].room)) continue;
                      { int ddx = g_lax - mrt_ent[e].x, ddz = g_laz - mrt_ent[e].z;
                        if (ddx < 0) ddx = -ddx; if (ddz < 0) ddz = -ddz;
                        if (ddx + ddz > 6144) continue; }
                      if (np >= ENT_PK_MAXDRAW) break;
                      build_ent_pickup(ent_pk_blob[np], atlasW, e);
                      displist[1+ndrawn*4+0] = (uint32_t)ent_pk_blob[np];
                      displist[1+ndrawn*4+1] = 319u;
                      displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                      displist[1+ndrawn*4+3] = 0;
                      np++; ndrawn++;
                  } }
#ifdef DARTS
                /* darts in flight: no room gate - a dart is only ever alive
                   for a few sectors of travel, and it is fired from an
                   emitter that already checked Lara's distance */
                { int d, nq2 = 0;
                  for (d = 0; d < DART_MAX; d++) {
                      if (!g_dart[d].life) continue;
                      if (ndrawn >= 37) break;
                      if (nq2 >= ENT_DART_MAXDRAW) break;
                      build_ent_dart(ent_dart_blob[nq2], atlasW, d);
                      displist[1+ndrawn*4+0] = (uint32_t)ent_dart_blob[nq2];
                      displist[1+ndrawn*4+1] = 319u;
                      displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                      displist[1+ndrawn*4+3] = 0;
                      nq2++; ndrawn++;
                  } }
#endif
#ifdef ENEMIES
                /* bats: flying enemies, near Lara, wing-flapping */
                { int e, na = 0;
                  for (e = 0; e < MRT_ENTCOUNT && ndrawn < 39; e++) {
                      if (g_useset) break;
                      if (!ent_is_bat(mrt_ent[e].type) || g_batdead[e]) continue;
                      /* bats MOVE toward Lara, so gate on the LIVE position, not
                         the spawn room/pos - they've usually left the spawn. */
                      { int ddx = g_lax - g_batx[e], ddz = g_laz - g_batz[e];
                        if (ddx < 0) ddx = -ddx; if (ddz < 0) ddz = -ddz;
                        if (ddx + ddz > 6144) continue; }
                      if (na >= ENT_BAT_MAXDRAW) break;
                      build_ent_bat(ent_bat_blob[na], atlasW, e,
                                    g_batframe % MRT_BAT_FRAMES);
                      displist[1+ndrawn*4+0] = (uint32_t)ent_bat_blob[na];
                      displist[1+ndrawn*4+1] = 319u;
                      displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                      displist[1+ndrawn*4+3] = 0;
                      na++; ndrawn++;
                  } }
                /* wolves: ground enemies, ~6x the bat's faces - capped low and
                   gated tight (face count is the frame cost). */
                { int e, na = 0;
                  for (e = 0; e < MRT_ENTCOUNT && ndrawn < 39; e++) {
                      if (g_useset) break;
                      if (!ent_is_wolf(mrt_ent[e].type) || g_batdead[e]) continue;
                      { int ddx = g_lax - g_batx[e], ddz = g_laz - g_batz[e];
                        if (ddx < 0) ddx = -ddx; if (ddz < 0) ddz = -ddz;
                        if (ddx + ddz > 6144) continue; }
                      if (na >= ENT_WOLF_MAXDRAW) break;
                      build_ent_wolf(ent_wolf_blob[na], atlasW, e,
                                     g_batframe % MRT_WOLF_FRAMES);
                      displist[1+ndrawn*4+0] = (uint32_t)ent_wolf_blob[na];
                      displist[1+ndrawn*4+1] = 319u;
                      displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                      displist[1+ndrawn*4+3] = 0;
                      na++; ndrawn++;
                  } }
                /* bear: single ground enemy, static reared pose (1 frame). */
                { int e, na = 0;
                  for (e = 0; e < MRT_ENTCOUNT && ndrawn < 39; e++) {
                      if (g_useset) break;
                      if (!ent_is_bear(mrt_ent[e].type) || g_batdead[e]) continue;
                      { int ddx = g_lax - g_batx[e], ddz = g_laz - g_batz[e];
                        if (ddx < 0) ddx = -ddx; if (ddz < 0) ddz = -ddz;
                        if (ddx + ddz > 6144) continue; }
                      if (na >= ENT_BEAR_MAXDRAW) break;
                      build_ent_bear(ent_bear_blob[na], atlasW, e, 0);
                      displist[1+ndrawn*4+0] = (uint32_t)ent_bear_blob[na];
                      displist[1+ndrawn*4+1] = 319u;
                      displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                      displist[1+ndrawn*4+3] = 0;
                      na++; ndrawn++;
                  } }
#ifdef ENEMYPREVIEW
                /* DIAGNOSTIC: draw one wolf + one bear at a fixed spot ~1600
                   units in front of Lara, ignoring entities/AI/gates, to prove
                   the model renders. Wolf ahead, bear ahead-and-right. */
                if (ndrawn < 37) {
                    int fx = (int)(((int32_t)SIN(g_layaw) * 900) >> 16);
                    int fz = (int)(((int32_t)COS(g_layaw) * 900) >> 16);
                    g_batx[0] = g_lax + fx;  g_baty[0] = g_lay;  g_batz[0] = g_laz + fz;
                    build_ent_wolf(ent_wolf_blob[0], atlasW, 0,
                                   g_batframe % MRT_WOLF_FRAMES);
                    displist[1+ndrawn*4+0] = (uint32_t)ent_wolf_blob[0];
                    displist[1+ndrawn*4+1] = 319u;
                    displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                    displist[1+ndrawn*4+3] = 0; ndrawn++;
                    g_batx[1] = g_lax + fx + 900; g_baty[1] = g_lay; g_batz[1] = g_laz + fz;
                    build_ent_bear(ent_bear_blob[0], atlasW, 1, 0);
                    displist[1+ndrawn*4+0] = (uint32_t)ent_bear_blob[0];
                    displist[1+ndrawn*4+1] = 319u;
                    displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                    displist[1+ndrawn*4+3] = 0; ndrawn++;
                }
#endif
#endif
#endif
#ifdef DEMO_PROPS
                if (g_dooryoff > -1500 && ndrawn < 8) {
                    build_door_blob(door_blob, atlasW);
                    displist[1+ndrawn*4+0] = (uint32_t)door_blob;
                    displist[1+ndrawn*4+1] = 319u;
                    displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                    displist[1+ndrawn*4+3] = 0;
                    ndrawn++;
                }
                if (!g_itemcollected && ndrawn < 8) {
                    build_item_blob(item_blob, atlasW);
                    displist[1+ndrawn*4+0] = (uint32_t)item_blob;
                    displist[1+ndrawn*4+1] = 319u;
                    displist[1+ndrawn*4+2] = (uint32_t)(RENDER_H-1);
                    displist[1+ndrawn*4+3] = 0;
                    ndrawn++;
                }
#endif /* DEMO_PROPS */
                RP(3);
#ifdef ABLADDER
                if (1) {   /* g_abrooms=0 must still dispatch Lara alone */
#else
                if (ndrawn) {
#endif
                    /* PIPELINE: Tom starts room 0 (self-transform) while the
                       68k reads Jerry's pose; Jerry then transforms rooms
                       1..N AHEAD of Tom's raster (Tom polls per-room flags,
                       self-transforms on timeout). ONE Tom sync. */
                    displist[0] = (uint32_t)ndrawn;
#endif
                    /* DETERMINISTIC ORDER (the flag-dance under full bus
                       load livelocked): finish ALL Jerry transforms + 68k
                       sorts BEFORE Tom starts. Tom then finds every cache
                       flag already 3 — zero cross-chip polling under load.
                       Costs ~6% parallelism; buys total correctness. */
                    if (posed < 2) { build_lara_part(lara_blob, atlasW, 0, 99); posed = 2; }
#ifdef JERRYPOSE
                    if (g_jerry_ok) {
                        extern int jerry_pose_sync(void);
                        extern void jerry_pose_read(void*,int);
                        extern void jerry_roomx_kick(const void*, const void*);
                        jerry_pose_sync();
                        HLP(3); RP(4);
                        /* Jerry wrote lara_blob+16 directly (params[6]) */
                        lara_finish(lara_blob, atlasW);
                        HLP(4); RP(5);
                        HB(2);   /* stage 2: pose consumed */
                        if (njx) {
                            /* OT PARKED: Jerry transforms only; his flag=2
                               is what the kernel polls. ot_sort stays for
                               the next campaign (pixel-diff verified). */
                            extern void video_wait_safe_vc(void);
                            int ai;
                            /* ARM FLAGS HERE, not at displist build: Jerry is
                               provably IDLE+DRAINED after pose_sync, so a
                               stale flag=2 orphaned in his posted queue by a
                               mid-roomx pose kill has already landed and gets
                               overwritten. Arming earlier let the orphan land
                               AFTER the arm -> Tom trusted last frame's cache
                               (stale-camera geometry towers, HW-seen). */
                            for (ai = 0; ai < njx; ai++)
                                jcache[ai][3] = 1;    /* pending */
                            jxlist[0] = (uint32_t)njx;
                            /* Jerry's DRAM write storm must not cover the
                               vblank ISR's OP-list rebuild window or the
                               top scanlines drop for a field (the "bounce"
                               — see video_wait_safe_vc in video.c). */
                            video_wait_safe_vc();
                            jerry_roomx_kick(jxlist, camblk);
                        }
                        HLP(5); RP(6);
                        g_jerry_frame_done = 1;
                    }
#endif
                    /* LARA RIDES THE DISPATCH: her blob is a geotex room
                       (self-transform, full-screen clip) appended LAST so
                       she paints over everything. Saves the second kick +
                       gpu_sync round-trip per frame; lara_finish completed
                       before this point so the blob is whole. */
#ifdef ABLADDER
                    if (g_abrooms >= 0)
#endif
                    { uint32_t dn = displist[0];
                      /* bit0 of the blob ptr = PER-PACKET NO-CULL (kernel
                         NCULF). Not set anymore: the extractor's LARA
                         WINDING FIX reorients her 11 inconsistently-wound
                         faces (broken elbows / see-through, 2026-07-20), so
                         she culls correctly like everything else. The flag
                         plumbing stays for future double-sided models. */
                      displist[1+dn*4+0] = (uint32_t)lara_blob;
                      displist[1+dn*4+1] = (0u<<16) | 319u;
                      displist[1+dn*4+2] = (0u<<16) | (uint32_t)(RENDER_H-1);
                      displist[1+dn*4+3] = 0;        /* Tom self-transform */
                      displist[0] = dn + 1;
                      lara_disp = 1; }
                    /* TWO-BATCH DISPATCH: the SRAM list holds 8 entries; big
                       junctions (>7 rooms) render in two sequential batches
                       (painter order preserved: far batch first). Dropping
                       rooms instead made them BLINK at the cap boundary
                       (flashing "ceiling" floors) + black holes. */
#ifdef STAGECHK
                    /* IS THE 68000 STAGING THE SAME FRAME TWICE?
                     * With the camera still and the world static, the display
                     * list the 68k hands Tom - room pointers plus clip rects -
                     * must be identical frame to frame.  If it is, the faces
                     * are being lost INSIDE the render (bus contention - the
                     * documented "Tom mis-reads DRAM shortly after branches")
                     * and no amount of double-buffering on this side helps.
                     * If it drifts, the race is 68k-side and findable here.
                     * One XOR pass over ~40 longs: it does not meaningfully
                     * perturb the timing it is measuring. */
                    { uint32_t ck = displist[0], ci;
                      uint32_t cam = camblk[0]^camblk[4]^camblk[5]^camblk[6];
                      for (ci = 0; ci < displist[0]*4u; ci++)
                          ck = (ck * 33u) ^ displist[1+ci];
                      if (cam == g_stagecam && ck != g_stageck) g_stagechg++;
                      g_stageck = ck; g_stagecam = cam; }
#endif
                    { uint32_t total = displist[0], base = 0;
                      static uint32_t batch[1+8*4];
                      while (base < total) {
                        uint32_t bn = total - base, bi;
                        if (bn > 3) bn = 3;   /* matches the 3-entry SRAM list */   /* matches the shrunken SRAM
                                                 dispatch list (gpu.c) */
                        batch[0] = bn;
                        for (bi = 0; bi < bn*4; bi++)
                            batch[1+bi] = displist[1+base*4+bi];
                        gpu_geotex_dispatch(batch, fb, camblk, S_atlas, (uint32_t)atlasW);
                        HB(1);   /* stage 1: dispatch kicked */
                        base += bn;
#ifdef PIPELINE
                        /* inner batches must drain before the SRAM list is
                           reused; the LAST stays IN FLIGHT — next frame's
                           68k logic runs under it (sync at loop top). Flag
                           set HERE so anything downstream that must own the
                           Blitter/GPU can test-and-collect first. */
                        if (base < total) gpu_sync();
                        else {
                            g_tominflight = 1;
#ifdef PACEPROBE
                            pp_kick = PPNOW();
#endif
                        }
#else
                        gpu_sync();
#endif
                      } }
                    HLP(6);
#ifdef BUSPROBE
                    { uint32_t _m = vp_tick(); if (bp_m) bp_p3 += _m - bp_m; bp_m = _m; }
#endif
                    HB(12);  /* stage 12: Tom done (all rooms) */
                    RP(7);
                } else if (posed < 2) {
                    build_lara_tex(lara_blob, atlasW); posed = 2;
                }
#if defined(PROFILE) && !defined(PIPELINE)
                /* rooms-drawn dots (row 33, left): one 4px dot per room drawn */
                { int d,xx; for (d=0;d<ndrawn;d++)
                    for (xx=0;xx<4;xx++) fb[33*RENDER_W + 8 + d*8 + xx] = 255; }
#endif
#ifdef PROFILE
                pfC = frame_count;
                { volatile uint32_t *tx=(volatile uint32_t*)0xF03EF4u;
                  volatile uint32_t *tr=(volatile uint32_t*)0xF03EF8u;
                  volatile uint32_t *st=(volatile uint32_t*)0xF03EECu;
                  g_gpuC = *tx + *tr + *st; }   /* GPU halflines at rooms end */
#endif
#ifdef JERRYPOSE
                if (g_jerry_ok && !g_jerry_frame_done) {
                    /* no-rooms path: pose still needs consuming */
                    extern int jerry_pose_sync(void);
                    extern void jerry_pose_read(void*,int);
                    jerry_pose_sync();
                    lara_finish(lara_blob, atlasW);
                }
                g_jerry_frame_done = 0;
#endif
                if (!lara_disp) {
                    /* fallback (dispatch list full or no rooms drawn):
                       Lara gets her own kick, painted last as before */
#ifdef PIPELINE
                    if (g_tominflight) {
                        gpu_sync(); g_tominflight = 0;
#ifdef PACEPROBE
                        pp_mid++;
#endif
                    }
#endif
                    gpu_geotex_setclip(0, 319, 0, RENDER_H-1);
                    gpu_geotex(lara_blob, fb, camblk, S_atlas, (uint32_t)atlasW);
                }
                HB(13);  /* stage 13: props+Lara done */
            }
#ifdef PROFILE
            pfD = frame_count;
#endif
#ifdef DEMO_PROPS
            /* pickup: item spins; collect when Lara walks into it */
            g_itemspin += 4;
            if (!g_itemcollected) {
                int dx = g_lax - g_itemx, dz = g_laz - g_itemz;
                if (dx<0)dx=-dx; if (dz<0)dz=-dz;
                if (dx + dz < 500) { g_itemcollected = 1; g_pickups++; g_pickupt = PICKUP_TICKS; }
            }
            /* the key opens the door: once collected it slides up out of sight */
            if (g_itemcollected && g_dooryoff > -1500) g_dooryoff -= 60;
#endif /* DEMO_PROPS */
            { fbpix bc=(fc++&1)?BLINK_ON:BLINK_OFF; int yy,xx;
              for (yy=0;yy<10;yy++) for (xx=0;xx<10;xx++) fb[yy*RENDER_W+xx]=bc; }
            /* pickup counter: g_pickups white dots at top-right */
            { int d,xx; for (d=0;d<g_pickups && d<8;d++)
                for (xx=0;xx<6;xx++) fb[3*RENDER_W + (RENDER_W-8) - d*8 + xx] = 255; }
#if defined(PROFILE) && !defined(PIPELINE)
                 /* white bars on a black strip; rows: 68k / 5rooms / lara / wait.
                    PIPELINE builds draw NO on-screen instrument: the strip
                    clear is a Blitter grab and Tom is still rendering — it
                    wedged the kernel (the all-black pipeline bug). The
                    console fps100 print is the instrument there. */
            { int xx;
              /* Blitter fill, NOT a 68k byte loop: the pc-histogram (floor
                 campaign, 2026-07-20) showed this strip clear as the #1 68k
                 line item — 14.2% of awake cycles drawing our own instrument. */
              blit_band(fb, 10, 30, 254);
              for (xx=0;xx<bar68;xx++) fb[12*RENDER_W+xx]=255;
              for (xx=0;xx<barrm;xx++) fb[16*RENDER_W+xx]=255;
              for (xx=0;xx<barla;xx++) fb[20*RENDER_W+xx]=255;
              for (xx=0;xx<barwt;xx++) fb[24*RENDER_W+xx]=255;
              /* row 5 = ABSOLUTE fps (px = fps*100/3): longer = FASTER. Compare
                 builds directly by this bar's length. */
              for (xx=0;xx<barfps;xx++) fb[28*RENDER_W+xx]=255;
#ifdef ASSETSUM
              { int b;
                for (b=0;b<32;b++) for (xx=0;xx<6;xx++) {
                  fb[32*RENDER_W + b*8+xx] = (uint8_t)(((g_sum1>>(31-b))&1) ? 255:254);
                  fb[36*RENDER_W + b*8+xx] = (uint8_t)(((g_sum2>>(31-b))&1) ? 255:254);
                  fb[40*RENDER_W + b*8+xx] = (uint8_t)(((g_blt >>(31-b))&1) ? 255:254); } }
#endif
            }
#endif
#ifdef BLITPROBE
            /* after ~15s of proven in-game chained rendering, hot-swap the
             * probe kernel into GPU SRAM and run the matrix IN the working
             * environment (never returns) */
            if (frame_count > 900)
                blitprobe_run();
#endif
#ifdef CLUTGUARD
            /* CLUT diagnostic (MULTIROOM loop): rewrite the CLUT every
               frame and draw the palette index ramp on rows 190-198. */
            video_set_clut(S_pal);
            { volatile uint16_t *clutg = (volatile uint16_t *)0xF00400u;
              clutg[254]=0x0000; clutg[255]=0xFFFF; }
            { int yy,xx;
              for (yy = 190; yy < 199; yy++)
                for (xx = 0; xx < RENDER_W; xx++)
                  fb[yy*RENDER_W+xx] = (fbpix)((xx*240)/RENDER_W); }
#endif
#ifdef STAGECHK
            /* 16-bit bar, top-right, white=1: how many frames restaged an
               IDENTICAL camera.  Zero bar = the 68k is deterministic and the
               loss is Tom-side. */
            { uint8_t *sb = (uint8_t *)fb; int bq, by4, bx4;
              for (bq = 0; bq < 16; bq++) {
                  uint8_t v = (uint8_t)((g_stagechg >> (15-bq)) & 1 ? 255 : 254);
                  for (by4 = 2; by4 < 8; by4++)
                      for (bx4 = 0; bx4 < 3; bx4++)
                          sb[by4*RENDER_W + 250 + bq*4 + bx4] = v;
              } }
#endif
#ifdef BANDPROBE
            /* WHO OWNS THE BOTTOM LINES? (2026-08-08, user: "at the bottom
               there are lines that extend across")  The band is FROZEN while
               the picture animates - measured off the screencast - so those
               rows are never redrawn, yet the per-frame clear covers rows
               0..RENDER_H.  Rather than reason about the OP height field,
               paint the two candidate regions in flat, unmistakable values
               as the LAST writes before the flip and let one capture say it:
                 white band  -> the tail of the RENDER area (rows 116..119)
                 grey band   -> the OP is reading PAST the render area into
                                rows 120+, which still hold the loading
                                screen / video frame
                 unchanged   -> neither: something else owns those scanlines */
            /* v2: stay INSIDE the render area.  The first probe also painted
               rows 120..239 every frame and the console came up with a
               corrupt display on TWO different pads - so that write, not the
               A10 lottery, was the problem, and a diagnostic that breaks the
               thing it measures is worthless.  Index 232 is the brightest
               entry in the level palette (near-white); 4 rows of it at the
               very bottom of the RENDER area cannot be missed.
                 white band at the bottom -> those scanlines ARE render rows
                                             116..119, and the fault is that
                                             the game never draws them
                 band unchanged           -> the display is showing something
                                             past the render area entirely */
            blit_band((uint8_t *)fb, RENDER_H - 4, RENDER_H, 232);
#endif
#ifdef PIPELINE
            /* flip deferred: Tom may still be drawing this frame. The next
               iteration's LOGIC runs under him; sync+flip happen at loop top
               before any blitter/fb/pose work. PROFILE caveats: phase stamps
               HLP(6)/HB(12) now mean "kick done" not "Tom done", and bars
               drawn above may race Tom's spans (cosmetic, PROFILE only).
               PIPESTAGE bisect: 0 = collect immediately (null overlap window,
               mechanism test), 2 = full overlap (collect after next logic). */
            g_pipeframe = 1;
#if PIPESTAGE == 0
            if (g_tominflight) { gpu_sync(); g_tominflight = 0; }
            if (g_pipeframe)   { video_flip(); g_pipeframe = 0; }
#endif
#else
            video_flip();
#endif
            HLP(7);
            /* FREE-RUN: no vblank wait. Triple buffering means the flip is
               latched by the vblank ISR whenever it lands; the render loop
               starts the next frame immediately in the third buffer instead
               of idling to a vblank boundary (~up to 16ms/frame reclaimed). */
#ifdef PROFILE
            /* accumulate per-phase vblank deltas; recompute bars every 60 frames.
               bar length (px) = phase_vblanks / total_vblanks * (RENDER_W-16). */
            pf68 += pfB-pfA; pfrm += pfC-pfB; pfla += pfD-pfC;
            /* rooms-block split: 1=jerry-kick 2=room-loop 3=props 4=dispatch.
               RP_ALL() stamped all slots at block entry, so a skipped phase
               yields 0 and no probe can go stale across frames. */
            { int _i; for (_i=1;_i<8;_i++) {
                /* forward-fill: a probe inside a branch that did not run still
                   holds the block-entry stamp, which is EARLIER than its
                   predecessor and would underflow to ~2^32. Clamp instead. */
                if (g_rp[_i] < g_rp[_i-1]) g_rp[_i] = g_rp[_i-1];
                g_rpa[_i] += g_rp[_i]-g_rp[_i-1]; } }
            { int hi; for (hi=1; hi<8; hi++) {
                uint32_t d = hlm[hi]-hlm[hi-1];
                if (d < 60000u) hla[hi] += d; }
              { uint32_t d8 = hlm[8]-hlm[3];
                if (d8 < 60000u) hla[8] += d8; } }  /* skip wrap glitches */
            { volatile uint32_t *tx=(volatile uint32_t*)0xF03EF4u;
              volatile uint32_t *tr=(volatile uint32_t*)0xF03EF8u;
              volatile uint32_t *st=(volatile uint32_t*)0xF03EECu;
              uint32_t tot = *tx + *tr + *st;   /* cumulative within 60f block */
              acc_gpu_rooms += g_gpuC - prevtot; /* rooms-phase GPU delta      */
              acc_gpu_lara  += tot - g_gpuC;     /* lara-phase GPU delta       */
              prevtot = tot; }
            { uint32_t fvbl = frame_count - pfA;   /* this render's VBL span */
              static uint32_t maxvbl2;
              if (fvbl > maxvbl2) maxvbl2 = fvbl;
              if (pfn >= 59) {   /* about to close the block: report + reset */
                  dbg_kv("maxvbl", (long)maxvbl2);
                  { extern uint32_t g_syncspins; static uint32_t lspin;
                    dbg_kv("spind", (long)(g_syncspins - lspin));
                    lspin = g_syncspins; }
#ifdef GOVERNOR
                  { static uint32_t ltrip;
                    dbg_kv("govt", (long)(g_gov_trips - ltrip));
                    ltrip = g_gov_trips; }
#endif
                  maxvbl2 = 0;
              } }
            pftt += frame_count-pfA; pfn++;
            if (pfn >= 60) {
                int W = RENDER_W-16, den = pftt ? (int)pftt : 1;
                uint32_t wt = pftt - (pf68+pfrm+pfla);
                bar68=(int)(pf68*(uint32_t)W/den); barrm=(int)(pfrm*(uint32_t)W/den);
                barla=(int)(pfla*(uint32_t)W/den); barwt=(int)(wt*(uint32_t)W/den);
                /* ABSOLUTE fps bar (row 5): px = fps*100/3, so 3fps=100px,
                   6fps=200px. Longer = FASTER. Compare builds by this directly. */
                { int fps100 = (int)((6000L*(long)pfn)/(long)den);
                  barfps = fps100/3; if (barfps > RENDER_W-1) barfps = RENDER_W-1;
                  /* capture-independent readout (B-feed video capture is dead
                     2026-07-20): NOGD builds stream the number over the skunk
                     console every 60-render block. NOTE under STAGEDIET the
                     phase bars (bar68/rm/la) read garbage — CAMLOC reuses
                     $F03EF0-FF — but this fps100 is pure VBL math, honest. */
                  dbg_kv("fps100", fps100);
                  /* fpsT: loop-top..loop-top window — includes logic. THE
                     honest number (fps100 kept only for ledger continuity). */
                  { int fpsT = pftt2 ? (int)((6000L*(long)pfn)/(long)pftt2) : 0;
                    dbg_kv("fpsT", fpsT); }
#ifdef JLOOPS
                  /* JERRY HEARTBEAT (2026-07-25). jagemu cannot answer whether
                     Jerry keeps up: its DSP accounting contradicts itself (IPC
                     0.537 under `run`, exactly 1.000 under `serve`, and the
                     heartbeat below disagrees with both). So read it on
                     silicon. LOOP_COUNT is bumped once per pass through
                     dsp_pose.das main_loop, and do_pose returns THROUGH
                     main_loop (dsp_pose.das:957), so every completed pose
                     bumps it exactly once.
                       jloops / jrend  ~= 1  -> Jerry keeps up, he is idle-polling
                                       <<  1 -> Jerry OVERRUNS the frame, and
                                                jerry_pose_sync (2.23% of wall)
                                                understates his true cost.
                     Read-only probe of DSP SRAM; costs one long load per
                     60-render block. */
                  { static uint32_t jlc_prev;
                    uint32_t jlc = *(volatile uint32_t *)0xF1C0B0u;
                    dbg_kv("jloops", (int)(jlc - jlc_prev));
                    dbg_kv("jrend",  (int)pfn);
                    jlc_prev = jlc; }
#endif
                  pftt2 = 0;
#if defined(QUIETFPS) && defined(SKUNK_CONSOLE)
                  /* FLOOR DECOMPOSITION (compact, every 4th block = ~7 prints
                     per ~30-60s, no fps skew worth caring about): the 68k
                     sub-phase halfline counters over the last 4 blocks.
                     hlw_pose = jerry_pose_sync wait (the floor's prime
                     suspect), hl_tom = dispatch..sync (Tom render + wait). */
                  { static int abdet;
                    if (++abdet >= 4) { abdet = 0;
                      dbg_kv("hl_logic", hla[1]);
                      dbg_kv("hl_clear", hla[2]);
                      dbg_kv("hlw_pose", hla[3]);
                      dbg_kv("hlr_pose", hla[4]);
                      dbg_kv("hl_mdep",  hla[8]);
                      dbg_kv("hl_tom",   hla[6]);
                      dbg_kv("hl_flip",  hla[7]);
                      { int hi; for (hi=0;hi<10;hi++) hla[hi]=0; }
#ifdef PACEPROBE
                      dbg_kv("pp_wait", pp_wait);
                      dbg_kv("pp_span", pp_span);
                      dbg_kv("pp_safe", pp_safe);
                      dbg_kv("pp_blit", pp_blit);
                      dbg_kv("pp_mid",  pp_mid);
                      dbg_kv("pp_per",  pp_per);
                      dbg_kv("pp_pmax", pp_pmax);
                      dbg_kv("pp_coll", pp_coll);
                      dbg_kv("pp_wmax", pp_wmax);
                      dbg_kv("pp_smax", pp_smax);
                      { extern uint32_t g_synccalls; static uint32_t lsc;
                        dbg_kv("synk", (long)(g_synccalls - lsc));
                        lsc = g_synccalls; }
                      pp_wait = pp_span = pp_safe = pp_blit = pp_mid = 0;
                      pp_per = pp_pmax = pp_coll = pp_wmax = pp_smax = 0;
#endif
                  } }
#endif
                  }
#ifdef ASSETSUM
                /* console-independent readout: recompute the atlas+geom sums
                   every 60 renders and draw them as 32 bit-cells on fb rows
                   32 (atlas) / 36 (geom), 8px/cell, white=1 — readable from
                   any video capture even with the skunk console dead. */
                { extern const uint8_t mrt_atlas[], mrt_geom[], mrt_sect[];
                  extern const uint16_t mrt_pal[];
                  const uint8_t *p;
                  g_sum1=0; g_sum2=0;
                  for(p=mrt_atlas;p<(const uint8_t*)mrt_pal;p++) g_sum1+=*p;
                  for(p=mrt_geom;p<mrt_sect;p++) g_sum2+=*p;
                  /* BLITTER SELF-TEST: copy one 320B atlas row through the
                     production blit_copy path, 68k-sum the RESULT (row-40
                     cells). Healthy = stable and equal to the 68k source
                     sum; a damaged Blitter datapath differs/flickers. */
                  { const uint8_t *src = mrt_atlas + 4096; uint32_t i;
                    for(i=0;i<320;i++) g_bltbuf[i]=0xA5;
                    blit_copy(src, g_bltbuf, 1);
                    g_blt=0; for(i=0;i<320;i++) g_blt+=g_bltbuf[i]; } }
#endif
#ifdef SKUNK_CONSOLE
#ifdef QUIETFPS
                if (0) {   /* ~20 prints/block stall the 68k (1.19 vs 4.0 fps);
                              QUIETFPS keeps only the fps100 line above */
#else
                if (skunk_up()) {
#endif
                    dbg_kv("=== MULTIROOM vbl over 60 frames ===", pfn);
#ifdef ASSETSUM
                    { extern const uint8_t mrt_atlas[];
                      extern const uint16_t mrt_pal[];
                      uint32_t s1=0; const uint8_t *p;
                      for(p=mrt_atlas;p<(const uint8_t*)mrt_pal;p++) s1+=*p;
                      dbg_kv("atlas_sum_now", (int)s1); }
#endif
                    dbg_kv("total_vbl", pftt);
                    dbg_kv("68k+clear", pf68);
                    dbg_kv("rooms", pfrm);
                    dbg_kv("lara", pfla);
                    dbg_kv("wait", wt);
                    dbg_kv("fps_x100", pftt ? (long)(6000L*pfn)/(long)pftt : 0);
                    /* 68k sub-phases, half-lines per 60 frames (525 = 1 field):
                       logic=loop-top..pfA  clear=pfA..blit  posewait=..sync
                       poseread=read+finish  jkick=dodge+roomx  tom=dispatch..sync
                       fliptail=sync..flip-done */
                    dbg_kv("hl_logic",    hla[1]);
                    dbg_kv("hl_clear",    hla[2]);
                    dbg_kv("hl_posewait", hla[3]);
                    dbg_kv("hl_poseread", hla[4]);
                    dbg_kv("hl_mdepth",   hla[8]);
                    dbg_kv("lemits",      g_lemits); g_lemits=0;
                    dbg_kv("hl_jkick",    hla[5]);
                    dbg_kv("hl_tom",      hla[6]);
                    dbg_kv("hl_fliptail", hla[7]);
                    { int hi; for (hi=0; hi<10; hi++) hla[hi]=0; }
                    /* GPU section split (halflines ~31.8us, accumulated by the
                       kernel over these 60 frames; read then clear) */
                    { volatile uint32_t *tx=(volatile uint32_t*)0xF03EF4u;
                      volatile uint32_t *tr=(volatile uint32_t*)0xF03EF8u;
                      dbg_kv("gpu_xform_hl", *tx);
                      dbg_kv("gpu_raster_hl", *tr);
                      { volatile uint32_t *ns=(volatile uint32_t*)0xF03EFCu;
                        dbg_kv("scanlines", *ns); *ns = 0; }
                      { volatile uint32_t *st=(volatile uint32_t*)0xF03EECu;
                        dbg_kv("gpu_stage_hl", *st); *st = 0; }
                      /* phase GPU-vs-68k split (halflines; phases in vbl=8.4hl) */
                      dbg_kv("gpurooms_hl", acc_gpu_rooms); acc_gpu_rooms=0;
                      dbg_kv("gpulara_hl",  acc_gpu_lara);  acc_gpu_lara=0;
                      prevtot = 0;   /* TACCs cleared below -> restart deltas */
                      dbg_kv("bench_reg_lines",  *(volatile uint32_t*)0xF03F40u);
                      dbg_kv("bench_sram_lines", *(volatile uint32_t*)0xF03F44u);
                      /* VC unit discriminator: max VC over one field
                         (~262 = full lines, ~524 = halflines) */
                      { volatile uint16_t *vc=(volatile uint16_t*)0xF00006u;
                        uint16_t mx=0; uint32_t f0=frame_count;
                        while (frame_count==f0) { uint16_t v=*vc; if (v>mx) mx=v; }
                        f0=frame_count;
                        while (frame_count==f0) { uint16_t v=*vc; if (v>mx) mx=v; }
                        dbg_kv("vc_max", mx); }
                      *tx = 0; *tr = 0; }
                }
#endif
                pf68=pfrm=pfla=pftt=pfn=0;
            }
#endif
        }
    }
#endif

    /* bad/mismatched data -> solid ORANGE and stop (format problem). */
    if (mh->magic != 0x4D52) {
        for (;;) {
            uint16_t *fb = video_backbuffer();
            blit_band(fb, 0, RENDER_H, 0xFFC0);
            video_flip();
            video_wait_vblank();
        }
    }

    /* Lara mesh (laramesh.bin, baked default pose) */
    {
        const LHdr *lh = (const LHdr *)lara_data;
        if (lh->magic == 0x4C41) {
            g_lnv = lh->vcount; g_lnq = lh->qcount; g_lnt = lh->tcount;
            if (g_lnv > 512) g_lnv = 512;
            g_lnframes = (lara_data[8] << 8) | lara_data[9];
            if (g_lnframes < 1) g_lnframes = 1;
            g_lframe0 = lara_data + 12;
            g_lverts = (const LVert *)g_lframe0;
            g_lquads = (const RQuad *)(g_lframe0 + g_lnframes*lh->vcount*6);
            g_ltris  = (const RTri  *)((const uint8_t *)g_lquads + lh->qcount*12);
            g_lshades = (const uint8_t *)(g_ltris + g_lnt);
        }
    }

    /* parse the room table (portal-connected rooms in common space) */
    {
        const uint8_t *p = rooms_data + 12;
        g_nrooms = mh->roomCount;
        if (g_nrooms > 16) g_nrooms = 16;
        for (r = 0; r < g_nrooms; r++) {
            const MRBlock *bl = (const MRBlock *)p;
            const uint8_t *body = p + 18;
            RoomDesc *rm = &g_rooms[r];
            rm->nv = bl->vcount; rm->nq = bl->qcount; rm->nt = bl->tcount;
            rm->xSec = bl->xSec; rm->zSec = bl->zSec;
            rm->offX = bl->offX; rm->offZ = bl->offZ; rm->yTopD = bl->yTopD;
            rm->verts = (const RVert *)body;
            rm->quads = (const RQuad *)(body + bl->vcount * 8);
            rm->tris  = (const RTri  *)(body + bl->vcount * 8 + bl->qcount * 12);
            rm->sect  = (const RSector *)(body + bl->vcount * 8
                          + bl->qcount * 12 + bl->tcount * 10);
            rm->norms = (const int16_t *)(body + bl->vcount * 8
                          + bl->qcount * 12 + bl->tcount * 10
                          + bl->xSec * bl->zSec * 4);
            p = (const uint8_t *)rm->norms + (bl->qcount + bl->tcount) * 6;
        }
    }

    /* Lara start (common coords) */
    g_lax = mh->laraX;
    g_laz = mh->laraZ;
    g_layaw = (uint8_t)(mh->laraAngle >> 8);
    {
        int fy;
        g_lafloor = global_walkable(g_lax, g_laz, &fy) ? fy : mh->laraY;
    }

    for (;;) {
        uint16_t *fb = video_backbuffer();
        uint32_t pad;
#ifndef NO_GAMEDRIVE
        if ((fctr++ & 3) == 0)       /* poll SD input file every 4th frame */
            gdpad = gd_input_poll();
#else
        fctr++;
#endif
        pad = joypad_read() | gdpad;
        fix cY, sY, cP, sP, cY4, sY4, cP4, sP4;
        int a, b, r;
#ifdef SKUNK_CONSOLE
        uint32_t pt0, pt1, pt2, pt3, pt4;   /* per-phase vblank timestamps */
#endif

        /* Lara tank controls + global floor-follow / wall collision */
        {
            int mv = 0, fy;
            if (pad & PAD_LEFT)  g_layaw -= 3;
            if (pad & PAD_RIGHT) g_layaw += 3;
            if (pad & PAD_UP)   mv = 1;
            if (pad & PAD_DOWN) mv = -1;
            if (mv) {
                int nx = g_lax + (int)(((int32_t)SIN(g_layaw) * (WALK_SPEED * mv)) >> 16);
                int nz = g_laz + (int)(((int32_t)COS(g_layaw) * (WALK_SPEED * mv)) >> 16);
                if (global_walkable(nx, nz, 0)) { g_lax = nx; g_laz = nz; }
            }
            if (global_walkable(g_lax, g_laz, &fy)) g_lafloor = fy;
        }
        /* 3rd-person camera behind Lara, looking her way */
        yaw = g_layaw;
        pitch = CAM_PITCH;
        cx = g_lax - (int)(((int32_t)SIN(g_layaw) * CAMDIST) >> 16);
        cz = g_laz - (int)(((int32_t)COS(g_layaw) * CAMDIST) >> 16);
        cy = g_lafloor - CAMHEIGHT;

        cY = COS(yaw); sY = SIN(yaw);
        cP = COS(pitch); sP = SIN(pitch);
        cY4 = cY >> 4; sY4 = sY >> 4; cP4 = cP >> 4; sP4 = sP >> 4;

#ifndef OVERLAP
        blit_band(fb, 0, RENDER_H, 0x0004);   /* dark backdrop */
#endif
        nspans = 0;
        ndp = 0;
#ifdef SKUNK_CONSOLE
        pt0 = frame_count;
#endif
#if defined(GEOMDIRECT)
        /* Tom reads the room geometry tables straight from DRAM: the 68k
         * only builds a far-first list of per-room base pointers (a few
         * rooms, no per-vertex work). Lara stays on a small world packet. */
        {
            int vis[16], nvis = 0, nrl = 0, k, i2, j2;
            for (r = 0; r < g_nrooms; r++) {
                RoomDesc *rm = &g_rooms[r];
                int ddx = (rm->offX + rm->xSec * 512) - cx;
                int ddz = (rm->offZ + rm->zSec * 512) - cz;
                if (ddx < 0) ddx = -ddx;
                if (ddz < 0) ddz = -ddz;
                if (ddx + ddz > 14000) continue;
                if (nvis < 16) vis[nvis++] = r;
            }
            /* order far-first by manhattan distance to room centre */
            for (i2 = 1; i2 < nvis; i2++) {
                int ri = vis[i2], di;
                RoomDesc *rm = &g_rooms[ri];
                int dx = (rm->offX + rm->xSec * 512) - cx;
                int dz = (rm->offZ + rm->zSec * 512) - cz;
                if (dx < 0) dx = -dx; if (dz < 0) dz = -dz; di = dx + dz;
                for (j2 = i2 - 1; j2 >= 0; j2--) {
                    RoomDesc *rj = &g_rooms[vis[j2]];
                    int ex = (rj->offX + rj->xSec * 512) - cx;
                    int ez = (rj->offZ + rj->zSec * 512) - cz;
                    if (ex < 0) ex = -ex; if (ez < 0) ez = -ez;
                    if (ex + ez >= di) break;      /* descending: far first */
                    vis[j2 + 1] = vis[j2];
                }
                vis[j2 + 1] = ri;
            }
            for (k = 0; k < nvis; k++) {
                RoomDesc *rm = &g_rooms[vis[k]];
                uint32_t *rec = &roomlist[nrl * 9];
                rec[0] = (uint32_t)rm->verts;
                rec[1] = (uint32_t)rm->quads;
                rec[2] = (uint32_t)rm->nq;
                rec[3] = (uint32_t)rm->tris;
                rec[4] = (uint32_t)rm->nt;
                rec[5] = (uint32_t)rm->offX;
                rec[6] = (uint32_t)rm->offZ;
                rec[7] = (uint32_t)rm->yTopD;
                rec[8] = (uint32_t)rm->norms;
                nrl++;
            }
            /* Lara: 68k does model->world, sorts her polys by depth */
            nwp = 0;
            draw_lara_x(cx, cz, cY4, sY4);
            {
                int bi;
                uint32_t np;
                for (bi = 0; bi < 1024; bi++) bkt_cnt[bi] = 0;
                for (a = 0; a < nwp; a++) {
                    int bk = wp[a].z >> 5;
                    if (bk < 0) bk = 0; else if (bk > 1023) bk = 1023;
                    bkt_cnt[bk]++;
                }
                b = 0;
                for (bi = 1023; bi >= 0; bi--) { bkt_pos[bi] = b; b += bkt_cnt[bi]; }
                for (a = 0; a < nwp; a++) {
                    int bk = wp[a].z >> 5;
                    if (bk < 0) bk = 0; else if (bk > 1023) bk = 1023;
                    word_[bkt_pos[bk]++] = a;
                }
                np = 0;
                for (a = 0; a < nwp; a++) {
                    WPoly *p = &wp[word_[a]];
                    int j;
                    polyx[gcur][np++] = ((uint32_t)p->n << 16) | p->color;
                    for (j = 0; j < p->n; j++) {
                        polyx[gcur][np++] = (uint32_t)p->wx[j];
                        polyx[gcur][np++] = (uint32_t)p->wy[j];
                        polyx[gcur][np++] = (uint32_t)p->wz[j];
                    }
                }
            }
            camblk[gcur][0] = (uint32_t)cY4; camblk[gcur][1] = (uint32_t)sY4;
            camblk[gcur][2] = (uint32_t)cP4; camblk[gcur][3] = (uint32_t)sP4;
            camblk[gcur][4] = (uint32_t)cx;  camblk[gcur][5] = (uint32_t)cy;
            camblk[gcur][6] = (uint32_t)cz;  camblk[gcur][7] = 0;
            if (gpu_ok)
                gpu_geomdirect(roomlist, (uint32_t)nrl, fb,
                               camblk[gcur], polyx[gcur], (uint32_t)nwp);
        }
#elif defined(GEOMXFORM)
        nwp = 0;
        for (r = 0; r < g_nrooms; r++) {
            RoomDesc *rm = &g_rooms[r];
            int ddx = (rm->offX + rm->xSec * 512) - cx;
            int ddz = (rm->offZ + rm->zSec * 512) - cz;
            if (ddx < 0) ddx = -ddx;
            if (ddz < 0) ddz = -ddz;
            if (ddx + ddz > 14000) continue;
            render_room_x(rm, cx, cz, cY4, sY4);
        }
        draw_lara_x(cx, cz, cY4, sY4);
        {
            int bi;
            uint32_t np;
            for (bi = 0; bi < 1024; bi++) bkt_cnt[bi] = 0;
            for (a = 0; a < nwp; a++) {
                int bk = wp[a].z >> 5;
                if (bk < 0) bk = 0; else if (bk > 1023) bk = 1023;
                bkt_cnt[bk]++;
            }
            b = 0;
            for (bi = 1023; bi >= 0; bi--) { bkt_pos[bi] = b; b += bkt_cnt[bi]; }
            for (a = 0; a < nwp; a++) {
                int bk = wp[a].z >> 5;
                if (bk < 0) bk = 0; else if (bk > 1023) bk = 1023;
                word_[bkt_pos[bk]++] = a;
            }
            np = 0;
            for (a = 0; a < nwp; a++) {
                WPoly *p = &wp[word_[a]];
                int j;
                polyx[gcur][np++] = ((uint32_t)p->n << 16) | p->color;
                for (j = 0; j < p->n; j++) {
                    polyx[gcur][np++] = (uint32_t)p->wx[j];
                    polyx[gcur][np++] = (uint32_t)p->wy[j];
                    polyx[gcur][np++] = (uint32_t)p->wz[j];
                }
            }
        }
        camblk[gcur][0] = (uint32_t)cY4; camblk[gcur][1] = (uint32_t)sY4;
        camblk[gcur][2] = (uint32_t)cP4; camblk[gcur][3] = (uint32_t)sP4;
        camblk[gcur][4] = (uint32_t)cx;  camblk[gcur][5] = (uint32_t)cy;
        camblk[gcur][6] = (uint32_t)cz;  camblk[gcur][7] = 0;
#ifdef OVERLAP
        /* async: sync + present the PREVIOUS frame, then fire this one
         * and return so the next iteration builds while Tom draws. */
        if (gpu_ok) {
            static int primed = 0;
            if (primed) {
                uint16_t *done = video_backbuffer();   /* GPU-finished buf */
                uint16_t bc = (fctr & 1) ? 0xFFFF : 0x0000;
                int yy, xx;
                gpu_sync();
                for (yy = 0; yy < 12; yy++)
                    for (xx = 0; xx < 12; xx++)
                        done[yy * RENDER_W + xx] = bc;
                video_flip();
            }
            fb = video_backbuffer();
            blit_band(fb, 0, RENDER_H, 0x0004);
            gpu_geomxform_kick(polyx[gcur], (uint32_t)nwp, fb, camblk[gcur]);
            primed = 1;
            gcur ^= 1;
        }
#else
        if (gpu_ok)
            gpu_geomxform(polyx[gcur], (uint32_t)nwp, fb, camblk[gcur]);
#endif
#else
        /* transform only nearby rooms (68k transform doesn't scale);
         * far rooms render as the camera approaches. Collision still
         * uses every room, so walking room-to-room is unaffected. */
        for (r = 0; r < g_nrooms; r++) {
            RoomDesc *rm = &g_rooms[r];
            int ddx = (rm->offX + rm->xSec * 512) - cx;
            int ddz = (rm->offZ + rm->zSec * 512) - cz;
            if (ddx < 0) ddx = -ddx;
            if (ddz < 0) ddz = -ddz;
            if (ddx + ddz > 14000) continue;     /* too far -> skip transform */
            render_room(rm, cx, cy, cz, cY4, sY4, cP4, sP4);
        }

        draw_lara(cx, cy, cz, cY, sY, cP, sP);   /* pushes Lara's polys */
#ifdef SKUNK_CONSOLE
        pt1 = frame_count;
#endif

        /* painter sort (far first) via counting sort on depth buckets -
         * O(ndp), not the O(ndp^2) insertion sort that dominated the
         * frame once Lara + rooms pushed ~1000+ faces (~4 s/frame). */
        {
            int bi;
            for (bi = 0; bi < 1024; bi++) bkt_cnt[bi] = 0;
            for (a = 0; a < ndp; a++) {
                int bk = dp[a].z >> 8;
                if (bk < 0) bk = 0; else if (bk > 1023) bk = 1023;
                bkt_cnt[bk]++;
            }
            b = 0;
            for (bi = 1023; bi >= 0; bi--) { bkt_pos[bi] = b; b += bkt_cnt[bi]; }
            for (a = 0; a < ndp; a++) {
                int bk = dp[a].z >> 8;
                if (bk < 0) bk = 0; else if (bk > 1023) bk = 1023;
                dord[bkt_pos[bk]++] = a;
            }
        }
#ifdef SKUNK_CONSOLE
        pt2 = frame_count;
#endif

#ifdef GEOMWALK
        /* Tom edge-walks the projected polys itself (the 68k builds only
         * the poly packets - no per-scanline fill_convex on the CPU). */
        if (gpu_ok) {
            uint32_t np = 0;
            for (a = 0; a < ndp; a++) {
                DPoly *p = &dp[dord[a]];
                int j;
                polylist[np++] = ((uint32_t)p->n << 16) | p->color;
                for (j = 0; j < p->n; j++)
                    polylist[np++] = ((uint32_t)(uint16_t)p->sx[j] << 16)
                                   | (uint32_t)(uint16_t)p->sy[j];
            }
            gpu_spanfill(polylist, (uint32_t)ndp, fb);   /* count = polys */
        } else
#endif
        {
            for (a = 0; a < ndp; a++) {
                DPoly *p = &dp[dord[a]];
                fill_convex(fb, p->sx, p->sy, p->n, p->color);
            }
            if (gpu_ok) {
                gpu_spanfill(spanlist, nspans, fb);
            } else {
                uint32_t k;
                for (k = 0; k < nspans; k++) {
                    uint32_t *r = &spanlist[k * 3];
                    int yy = (int)(r[0] >> 16);
                    int xx = (int)(r[0] & 0xFFFF);
                    int nn = (int)(r[2] & 0xFFFF);
                    blit_span(fb, yy, xx, xx + nn - 1, (uint16_t)(r[1] & 0xFFFF));
                }
            }
        }
#endif /* GEOMXFORM else */
#ifdef SKUNK_CONSOLE
        pt4 = frame_count;
        if ((fctr & 3) == 0) {
            dbg_kv("ndp", ndp);
            dbg_kv("nspans", (long)nspans);
            dbg_kv("xform", pt1 - pt0);   /* transform + cull (vblanks) */
            dbg_kv("sort", pt2 - pt1);
            dbg_kv("walk", pt3 - pt2);    /* edge-walk / span build     */
            dbg_kv("gpufill", pt4 - pt3);
        }
#endif

#ifndef OVERLAP
        /* frame-rate blink (top-left corner toggles every frame) - lets
         * true fps be counted from a capture even on a static scene. */
        {
            uint16_t bc = (fctr & 1) ? 0xFFFF : 0x0000;
            int yy, xx;
            for (yy = 0; yy < 12; yy++)
                for (xx = 0; xx < 12; xx++)
                    fb[yy * RENDER_W + xx] = bc;
        }

#ifdef SKUNK_CONSOLE
        if ((fctr & 63) == 0) video_dump_oplist();   /* periodic: jcp -c catches it */
#endif
        video_flip();
        video_wait_vblank();
#endif
    }

    return 0;
}
