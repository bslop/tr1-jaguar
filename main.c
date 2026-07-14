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
#include "joypad.h"
#include "gd_input.h"
#include "gdbios.h"
#include "gpu.h"
#include "skunkdbg.h"
#include "sintab.h"
#ifdef GEOTEX
#include "room0_tex.h"      /* ROOM0_ATLAS_H + Lara swatch cell defines */
#endif
#ifdef MULTIROOM
#include "mrt.h"            /* MRT_ATLAS_H + Lara swatch cell defines */
#include "mrt_lara.h"       /* Lara anim frame indices (run/stand/jump) */
#include "mrt_spawn.h"      /* Lara entity from LEVEL1.PSX (room/pos/yaw) */
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
#define CLEAR_IDX 254
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
static int g_floorwater;  /* 1 if that floor is a WATER-SURFACE cell. Read from
   bit0 of the STORED floorY (the extractor's mark) BEFORE the slope interpolation
   runs -- slope math dirties bit0, which made sloped SNOW read as water. The
   returned *floorY is always kept even, so callers must use THIS flag, never
   (floorY & 1), to detect water. */
static int room_reachable(int a, int b);   /* fwd (defined after S_adj) */
static int g_flr_limit;   /* 1 = only search current room + portal neighbours
   (gameplay walking; stops stair-casing up stacked rooms' floors at region
   boundaries — the "launched to the top crossing onto the rug" bug) */
static int g_curroom_fwd(void);
/* ceiling of the CURRENT room's sector at (wx,wz): grab targets ABOVE this
   are through solid geometry (the courtyard->roof "fly to the top" chain). */
static int room_ceil_at(const uint8_t *sp, int wx, int wz, int *ceilY)
{
    int xS = (sp[0]<<8)|sp[1], zS = (sp[2]<<8)|sp[3];
    int ix = (int)(((uint32_t)sp[4]<<24)|((uint32_t)sp[5]<<16)|((uint32_t)sp[6]<<8)|sp[7]);
    int iz = (int)(((uint32_t)sp[8]<<24)|((uint32_t)sp[9]<<16)|((uint32_t)sp[10]<<8)|sp[11]);
    int lx = wx - ix, lz = wz - iz;
    const uint8_t *e;
    if (lx < 0 || lx >= xS*1024 || lz < 0 || lz >= zS*1024) return 0;
    e = sp + 12 + ((lx>>10)*zS + (lz>>10))*6;
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
        rx  = (dx*pcl_cY4 - dz*pcl_sY4) >> 12;
        rz  = (dx*pcl_sY4 + dz*pcl_cY4) >> 12;
        ry  = (dy*pcl_cP4 - rz*pcl_sP4) >> 12;
        rz2 = (dy*pcl_sP4 + rz*pcl_cP4) >> 12;
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
    e = sp + 12 + ((lx>>10)*zS + (lz>>10))*6;
    return (int16_t)(((uint16_t)e[0]<<8)|e[1]) == 0x7FFF;
}
static int g_flr_grab;    /* 1 = ledge-search mode: plain closest-floor (the
                             vault/grab checks look for floors ABOVE Lara) */
static int g_flr_wy;      /* caller's current Y for Y-AWARE floor selection:
   with vertically STACKED rooms (10-room sets), "lowest floor wins" grabbed
   floors in rooms BELOW; instead prefer the nearest floor at/below wy that's
   reachable (within step-up), falling back to the old rule if none. */
static int room_floor_mr(const uint8_t **rsect, int n, int wx, int wz, int *floorY)
{
    int r, found = 0, best = 0, best_w = 0;
    int nfound = 0, nbest = 0, nbestd = 0, nbest_w = 0;  /* Y-aware: CLOSEST reachable floor */
    int nbtier = 0;
    g_floorwater = 0;
    for (r = 0; r < n; r++) {
        const uint8_t *sp = rsect[r];
        int xS, zS;
        if (g_flr_limit && !room_reachable(g_curroom_fwd(), r)) continue;
        xS = (sp[0]<<8)|sp[1]; zS = (sp[2]<<8)|sp[3];
        int ix = (int)(((uint32_t)sp[4]<<24)|((uint32_t)sp[5]<<16)|((uint32_t)sp[6]<<8)|sp[7]);
        int iz = (int)(((uint32_t)sp[8]<<24)|((uint32_t)sp[9]<<16)|((uint32_t)sp[10]<<8)|sp[11]);
        int lx = wx - ix, lz = wz - iz;
        const uint8_t *e; int fy, dx, dz, sxs, szs, w;
        if (lx < 0 || lx >= xS*1024 || lz < 0 || lz >= zS*1024) continue;
        e = sp + 12 + ((lx>>10)*zS + (lz>>10))*6;   /* cell stride 6 (slant added) */
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
        fy -= (sxs * (sxs > 0 ? (dx - 1023) : dx)) >> 2;
        fy -= (szs * (szs > 0 ? (dz - 1023) : dz)) >> 2;
        /* rooms overlap at portals: pick the LOWEST floor (largest Y, +Y down)
         * so Lara stands on the actual ground, not a phantom higher surface
         * from an adjoining room (that caused her to float near walls). */
        if (!found || fy > best) { best = fy; best_w = w; found = 1; g_floorroom = r; }
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
#define WALK_SPEED 140          /* forward units per frame (>>16 of trig)  */
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
static fix g_lax, g_laz, g_lafloor;   /* Lara world position       */
static fix g_lay, g_lavy;             /* Lara feet Y + vertical vel (jump) */
static int g_lajf;                    /* running-jump: carry forward while airborne */
static int g_airfr;                   /* consecutive airborne frames (land-thud gate) */
static int g_swim;                    /* 1 = swimming (water room)          */
static int g_watery;                  /* water surface Y for the active pool */
#define JUMP_VEL  245                 /* PS1-feel clamp: apex ~1.7 clicks   */
#define GRAVITY    70                 /* per-frame downward accel          */
#define JUMP_FWD  135                 /* PS1-feel clamp: shorter leaps      */
#define LARA_STEPUP 384               /* max walk-up step (taller = wall/ledge) */
/* ledge vault / pull-up: a step too tall to walk up but low enough to climb
   triggers a pull-up; while airborne, hands within reach of a ledge grab it. */
static int g_vault;                   /* pull-up in progress (0/1)        */
static fix g_vaultx, g_vaultz;        /* foothold on top of the ledge     */
static int g_vaulty;                  /* target feet Y once up            */
static int g_climbf;                  /* climb tick counter               */
static int g_climby0;                 /* feet Y at the moment she grabs    */
static int g_climbx0, g_climbz0;      /* x/z at grab: eased onto the ledge  */
static int g_climbanim = LANIM_STOP;  /* which climb/vault anim to play    */
static int g_pickupt;                 /* pickup animation countdown (ticks) */
#define PICKUP_TICKS 8                 /* ticks the pickup pose plays        */
static int g_curroom;                 /* room Lara is standing in (visibility) */
static int g_curroom_fwd(void) { return g_curroom; }
#define LARA_CLIMB    768             /* max standing VAULT = 3 clicks (TR1-authentic;
                                         taller ledges need a jump+grab at the right
                                         height — 7-click vaults let her scale the
                                         mansion from the courtyard) */
#define LARA_GRABREACH 720            /* PS1-feel: hands ~2.8 clicks above feet */
#define CLIMB_TICKS    6              /* game ticks the pull-up spans (fps  */
                                      /* is low, so play the 26f anim fast) */
/* alignToWall: TR squares Lara up to the wall before a climb (nearest 90 deg).
   The sector grid is axis-aligned, so snap her heading to the nearest cardinal. */
#define ALIGN_WALL(yaw) ((uint8_t)(((yaw) + 0x20) & 0xC0))
static uint8_t g_layaw;               /* Lara heading (0..255)     */
static int lsx[512], lsy[512], lz[512];
static uint8_t lbehind[512];

#if defined(GEOTEX) || defined(MULTIROOM)
/* Build Lara's per-frame WORLD geometry in room0_tex format so the SAME
 * gpu_geotex kernel draws her over the textured room (no separate kernel).
 * Verts use the offX/offZ split (world = local + off<<8) so they fit s16;
 * every face's UVs point at one solid Lara swatch cell (flat colour). */
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
static inline int32_t mul16(int32_t a, int32_t b){
    __asm__("muls.w %1,%0" : "+d"(a) : "d"(b));
    return a;
}

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
      for (mo=0;mo<mc;mo++){ int mm=morder[mo];
        for (x=mq_start[mm];x<mq_start[mm+1];x++){
            const uint16_t *s=qb+mq_list[x]*12; for(k=0;k<12;k++) w[k]=s[k]; w+=12; } }
      for (mo=0;mo<mc;mo++){ int mm=morder[mo];
        for (x=mt_start[mm];x<mt_start[mm+1];x++){
            const uint16_t *s=tb+mt_list[x]*9; for(k=0;k<9;k++) w[k]=s[k]; w+=9; } }
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
static void sfx_play(int voice, int id)
{
    const uint8_t *b = sfx_bank;
    int n = (b[0]<<8)|b[1];
    uint32_t off, len;
    if (!g_sfx_ok || id < 0 || id >= n) return;
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
#define HLP(i) (hlm[i] = frame_count*525u + (uint32_t)VC)
#else
#define HLP(i) ((void)0)
#endif

static int32_t g_mtpos[16*3] __attribute__((aligned(8)));  /* Jerry: mesh T per pose */

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
    for (i=1;i<mc;i++){ int j=i, mo=ps_morder[i]; int32_t md=ps_mdepth[mo];
        while (j>0 && ps_mdepth[ps_morder[j-1]] < md){ ps_morder[j]=ps_morder[j-1]; j--; }
        ps_morder[j]=mo; }
    HLP(8);   /* depth+sort done; 8-3 = mdepth cost, 4-3 = whole finish */
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
            uint32_t *d=(uint32_t*)w;
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; d[4]=s[4]; d[5]=s[5];
            w+=12; } }
      for (mo=0;mo<mc;mo++){ int mm=ps_morder[mo];
        for (x=mt_start[mm];x<mt_start[mm+1];x++){
            const uint16_t *s=tb+mt_list[x]*9;
            uint32_t *d=(uint32_t*)w;
            d[0]=((const uint32_t*)s)[0]; d[1]=((const uint32_t*)s)[1];
            d[2]=((const uint32_t*)s)[2]; d[3]=((const uint32_t*)s)[3];
            w[8]=s[8];
            w+=9; } }
    }
}
/* DEPTH-SORT OT: order one Jerry-cached room's faces far->near. Runs on
   the 68k while Tom rasters earlier rooms (time it used to sleep through).
   Writes qlist/tlist of RECORD ADDRESSES; Tom's face iterator walks them. */
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
static void lanim_set(int id){ if(id!=g_lanim_id){ g_lanim_id=id; g_lanim_fr=0; } }
static int  lanim_done(void){                 /* one-shot reached last frame? */
    return g_lanim_fr >= rd16(g_sk_anims+g_lanim_id*6+2) - 1;
}
static void lanim_step(int loop, int step){   /* advance + compute g_lframe   */
    int st=rd16(g_sk_anims+g_lanim_id*6), cnt=rd16(g_sk_anims+g_lanim_id*6+2);
    if(cnt<1) cnt=1;
    g_lanim_fr += step;
    if(g_lanim_fr >= cnt){ if(loop) g_lanim_fr %= cnt; else g_lanim_fr = cnt-1; }
    g_lframe = st + g_lanim_fr; g_anim_start = st;
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
        w[0]=item_q[i][0]; w[1]=item_q[i][1]; w[2]=item_q[i][2]; w[3]=item_q[i][3];
        w[4]=DOOR_UL;w[5]=DOOR_VL; w[6]=DOOR_UH;w[7]=DOOR_VL;
        w[8]=DOOR_UH;w[9]=DOOR_VH; w[10]=DOOR_UL;w[11]=DOOR_VH;
        w += 12;
    }
}
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
    video_init();
    CRUMB(0xFFE0);                   /* YELLOW: video up */
    skunk_init();                    /* Skunkboard USB console (NOGD only) */
    video_dump_oplist();             /* dump OP list once for OP debugging  */
#ifndef NO_GAMEDRIVE
    gd_input_init();
#endif
    gpu_ok = gpu_init();             /* Tom drains the span list if up */
    CRUMB(0x07FF);                   /* CYAN: gpu_init done */
    { extern int jerry_init(void);
      gpu_geotex_setclip(0, 319, 0, RENDER_H-1);   /* clip = full screen */
      g_jerry_ok = jerry_init();
      CRUMB(0xF81F);                 /* MAGENTA: jerry_init done */
      g_sfx_ok = g_jerry_ok;
      /* constants -> Jerry local SRAM happens later, once skv is converted */
#ifdef SKUNK_CONSOLE
      dbg_kv("jerry_alive", g_jerry_ok);
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
                fbpix bc = (fc++ & 1) ? BLINK_ON : BLINK_OFF;
                gpu_sync();
                done = video_backbuffer();
                for (yy = 0; yy < 10; yy++) for (xx = 0; xx < 10; xx++)
                    done[yy * RENDER_W + xx] = bc;
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
            {   fbpix bc = (fc++ & 1) ? BLINK_ON : BLINK_OFF; int yy, xx;
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
        static uint8_t lara_blob[12288] __attribute__((aligned(8)));
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
              if (pad & PAD_LEFT)  g_layaw -= 3;
              if (pad & PAD_RIGHT) g_layaw += 3;
              if (pad & PAD_UP)   mv = 1;
              if (pad & PAD_DOWN) mv = -1;
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
            {   fbpix bc = (fc++ & 1) ? BLINK_ON : BLINK_OFF; int yy, xx;
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
        static uint8_t lara_blob[12288] __attribute__((aligned(8)));
        static uint8_t item_blob[512] __attribute__((aligned(8)));
        static uint8_t door_blob[512] __attribute__((aligned(8)));
        const uint8_t *rgeom[64]; const uint8_t *rsect[64];
        int rcx[64], rcz[64], order[64], rrad[64], rdepth[64];
        int prv[64], prx0[64], prx1[64], pry0[64], pry1[64]; /* room windows */
        static uint32_t displist[1+40*4];   /* dispatch: {room,cx,cy,cache};
                                               39 rooms + Lara; >8 entries
                                               render in sequential batches
                                               (NOVISCULL can request all) */
        static uint32_t jxlist[1+8*5];      /* Jerry room-transform list */
        static uint32_t jcache[8][2244] __attribute__((aligned(8)));
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
          extern const uint8_t  photo_geom[], photo_atlas[];
          static uint32_t tcam[8] __attribute__((aligned(16)));
          /* AUTHENTIC RING (OpenLara inventory.h:1706): items on a circle,
             selected at front, other across the ring (farther + higher);
             every item faces with the PI flip the PSX models are authored
             for. 68k rotates the tiny vert sets into scratch blobs. */
          static uint8_t rblob[2][1280] __attribute__((aligned(8)));
          const uint8_t *rsrc[2]; const uint8_t *ratl[2];
          int rvcnt[2], rblen[2];
          int ringR = 0, ringT = 0;    /* current/target ring angle (1024) */
          int spin = 0;
          int page = 0, shown = -1, armed = 0;
          uint32_t praw = 0, stable = 0, sprev = 0;
#ifndef NO_GAMEDRIVE
          /* TITLE MUSIC: stream MUSIC.PCM (raw s8 @11025) from the SD
             card through a double buffer on voice 0 (footsteps own it
             in-game — no conflict). No seek in the GD BIOS: hold the
             handle, sequential reads, close+reopen to loop the theme. */
          static int8_t mbuf[2][4096] __attribute__((aligned(4)));
          /* 4KB = 0.37s per swap; halved from 8KB to protect the 68k STACK:
             sp starts at 0x200000 and grows DOWN into the top of BSS — the
             8KB buffers left only 208 BYTES of headroom (cold-boot crash,
             2026-07-12). Makefile now guards bss_end < 0x1FC000. */
          static const char *mnames[2] = { "MUSIC.PCM", "/MUSIC.PCM" };
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
          rsrc[0]=pass_geom; ratl[0]=pass_atlas;
          rsrc[1]=photo_geom; ratl[1]=photo_atlas;
          { int it2;
            for (it2=0; it2<2; it2++) {
              const uint8_t *sb=rsrc[it2];
              int nv=(sb[0]<<8)|sb[1], nq=(sb[2]<<8)|sb[3], nt=(sb[4]<<8)|sb[5];
              int len=16+nv*8+nq*24+nt*18, i3;
              if (len > (int)sizeof(rblob[0])) len = sizeof(rblob[0]);
              for (i3=0;i3<len;i3++) rblob[it2][i3]=sb[i3];
              rvcnt[it2]=nv; rblen[it2]=len;
            } }
#ifndef NO_GAMEDRIVE
          { int mi;
            for (mi = 0; mi < 2 && mh < 0; mi++)
                mh = gd_fopen(mnames[mi], GD_FOPEN_READ | GD_FOPEN_OPEN_EXISTING);
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
              if (mh >= 0 && g_sfx_ok) {
                  /* gapless service: the pump promoted the queued buffer
                     (NCNT==0) -> refill the dead one and re-queue it. */
                  volatile uint32_t *ncnt = (volatile uint32_t *)0xF1C378u;
                  if (*ncnt == 0) {
                      extern void jerry_sfx_queue(const void*, uint32_t);
                      int dead = mplay ^ 1, n;
                      if (mleft <= 0) {              /* EOF: reopen to loop */
                          int mi;
                          gd_fclose((unsigned)mh); mh = -1;
                          for (mi = 0; mi < 2 && mh < 0; mi++)
                              mh = gd_fopen(mnames[mi], GD_FOPEN_READ | GD_FOPEN_OPEN_EXISTING);
                          mleft = mh >= 0 ? msz : 0;
                      }
                      n = mleft < (int)sizeof(mbuf[0]) ? mleft : (int)sizeof(mbuf[0]);
                      mlq[dead] = 0;
                      if (mh >= 0 && n &&
                          gd_fread((unsigned)mh, mbuf[dead], (unsigned)n, GD_FREAD_CPU) == 0)
                          { mlq[dead] = n; mleft -= n; }
                      if (mlq[dead]) { jerry_sfx_queue(mbuf[dead], (uint32_t)mlq[dead]); mplay = dead; }
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
#ifdef HALFRES
                  /* art is 320x240: take every other row for the 120-line fb
                     (the OP line-double displays it full height again).
                     (Native-240 composite tried 2026-07-12: too slow on the
                     68k — revisit with a Blitter composite, task #29.) */
                  { int yy2, xx2;
                    for (yy2 = 0; yy2 < RENDER_H; yy2++)
                      for (xx2 = 0; xx2 < RENDER_W; xx2++)
                          tfb[yy2*RENDER_W+xx2] = simg[(yy2*2)*RENDER_W+xx2]; }
#else
                  for (i2 = 0; i2 < RENDER_W*RENDER_H; i2++) tfb[i2] = simg[i2];
#endif
                  shown = page;
                  /* the 3D passport (TITLE.PSX model 71), orbiting */
                  if (gpu_ok) {
                      /* SPLIT PLACEMENTS (per-model, per-state): the two
                         models are authored facing OPPOSITE ways and need
                         individual parking spots + yaws. phase 0 = passport
                         selected, 256 = photo selected; positions/yaws lerp.
                         {selx,sely,selz,selyaw, unx,uny,unz,unyaw} */
                      static const int16_t mp2[2][8] = {
                        { 0, -10, 260, 120,  -230, -80, 470, 24  },
                        { 0,   0, 240, 0,     230, -80, 470, 96  },
                      };
                      int it2, ord2, zi[2], px[2], py[2], pz[2], yw[2];
                      { int dd = ringT - ringR;
                        ringR += (dd > 0) ? ((dd+3)>>2) : -(((-dd)+3)>>2); }
                      spin = (spin + 4) & 1023;
                      for (it2 = 0; it2 < 2; it2++) {
                          int f = it2 ? (256 - ringR) : ringR;   /* 0=sel */
                          const int16_t *m = mp2[it2];
                          px[it2] = m[0] + ((m[4]-m[0])*f>>8);
                          py[it2] = m[1] + ((m[5]-m[1])*f>>8);
                          pz[it2] = m[2] + ((m[6]-m[2])*f>>8);
                          yw[it2] = (m[3] + ((m[7]-m[3])*f>>8)
                                     + ((f<64) ? ((SIN(spin)*24)>>16) : 0)) & 1023;
                          zi[it2] = pz[it2];
                      }
                      tcam[0]=(uint32_t)(COS(0)>>4); tcam[1]=(uint32_t)(SIN(0)>>4);
                      tcam[2]=(uint32_t)(COS(6)>>4); tcam[3]=(uint32_t)(SIN(6)>>4);
                      tcam[4]=0; tcam[5]=(uint32_t)(-20); tcam[6]=0; tcam[7]=0;
                      gpu_geotex_setclip(0, 319, 0, RENDER_H-1);
                      for (ord2 = 0; ord2 < 2; ord2++) {
                        it2 = (zi[0] >= zi[1]) ? ord2 : 1-ord2;  /* deeper first */
                        { fix cy2 = COS(yw[it2]), sy2 = SIN(yw[it2]);
                          const uint8_t *sb = rsrc[it2];
                          uint8_t *db = rblob[it2];
                          int nv = rvcnt[it2], vi;
                          for (vi = 0; vi < nv; vi++) {
                              const uint8_t *vp = sb + 16 + vi*8;
                              int x=(int16_t)((vp[0]<<8)|vp[1]);
                              int y=(int16_t)((vp[2]<<8)|vp[3]);
                              int z=(int16_t)((vp[4]<<8)|vp[5]);
                              int rx2 = ((x*cy2 + z*sy2)>>16) + px[it2];
                              int rz2 = ((z*cy2 - x*sy2)>>16) + pz[it2];
                              int ry2 = y + py[it2];
                              uint8_t *dp = db + 16 + vi*8;
                              dp[0]=(uint8_t)(rx2>>8); dp[1]=(uint8_t)rx2;
                              dp[2]=(uint8_t)(ry2>>8); dp[3]=(uint8_t)ry2;
                              dp[4]=(uint8_t)(rz2>>8); dp[5]=(uint8_t)rz2;
                          }
                          gpu_geotex(db, tfb, tcam, ratl[it2], 256u);
                        }
                      }
                  }
                  video_flip();
                  video_wait_vblank();
              }
              if (edge & (PAD_LEFT|PAD_RIGHT)) { page ^= 1; ringT = page ? 256 : 0;
                                                 sfx_play(1, SFX_MENU_SPIN); }
              else if (edge & PAD_A) { g_useset = page;
                                       sfx_play(1, SFX_MENU_SHOW); break; }
          }
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
          for (py2=0; py2<LH; py2++)
            for (px2=0; px2<RENDER_W; px2++) lfb[py2*RENDER_W+px2]=254;
          /* glyph cells are 16 wide but the INK varies (8..15 cols, left-
             aligned) — advance by ink width + a constant gap so the word
             is evenly spaced. Two passes: measure, then draw. */
          { int inkw[16];
            totw=0;
            for (gi=0; gi<n && gi<16; gi++) {
                const uint8_t *rec=font_load+4+gi*8;
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
            ox=RENDER_W-totw-10; oy=LH-16/DIV-8;     /* lower-right corner */
            for (gi=0; gi<n && gi<16; gi++) {
                const uint8_t *rec=font_load+4+gi*8;
                int w=rec[1], h=rec[2];
                int yoff=(16-h)/DIV;         /* baseline-align short glyphs */
                uint32_t po=((uint32_t)rec[4]<<24)|((uint32_t)rec[5]<<16)
                           |((uint32_t)rec[6]<<8)|rec[7];
                for (gy=0; gy<h/DIV; gy++)
                  for (gx=0; gx<inkw[gi]; gx++) {
                      int v=font_load[po+gy*DIV*w+gx*DIV];
                      int X=ox+gx, Y=oy+yoff+gy;
                      if (v && X>=0&&X<RENDER_W&&Y>=0&&Y<LH)
                          lfb[Y*RENDER_W+X]=255;   /* flat white */
                  }
                ox += inkw[gi] + 2;
            } }
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
          (void)i2;
          g_pickidx = 240;
          g_dooridx = 241; }
        int i;
        uint32_t fc = 0;
#ifdef PROFILE
        extern volatile uint32_t frame_count;   /* VI-ISR vblank clock (60Hz) */
        uint32_t pf68=0,pfrm=0,pfla=0,pftt=0,pfn=0,pfA=0,pfB=0,pfC=0,pfD=0;

        uint32_t g_gpuC=0, acc_gpu_rooms=0, acc_gpu_lara=0, prevtot=0;
        int bar68=0,barrm=0,barla=0,barwt=0,barfps=0;   /* on-screen profile bars (px) */
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
              long long hx=(long long)xS*512, hz=(long long)zS*512;
              long long r2=hx*hx+hz*hz; int r=1;
              while ((long long)(r+1)*(r+1) <= r2) r++;   /* isqrt */
              rcx[i] = ix + xS*512; rcz[i] = iz + zS*512; rrad[i] = r; }
        }
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
#if defined(JERRYPOSE) && defined(SKUNK_CONSOLE)
          /* JERRY MATH SELF-TEST: pose frame 0 on BOTH processors, compare
             every vert. Definitive fixed-point verification, no eyes needed. */
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
          g_curroom = sproom;
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
        /* poke pickup GOLD into swatch cell 0 and door WOOD into cell 1 + CLUT */
        { uint8_t *atl = S_atlas; int yy, xx;
          for (yy=0; yy<MRT_LARA_CELL; yy++)
            for (xx=0; xx<MRT_LARA_CELL; xx++) {
              atl[(g_swy+yy)*MRT_ATLAS_W + xx] = (uint8_t)g_pickidx;
              atl[(g_swy+yy)*MRT_ATLAS_W + MRT_LARA_CELL + xx] = (uint8_t)g_dooridx;
            } }

#ifdef SKUNK_CONSOLE
        dbg_kv("bc_pokes", 1);
#endif
        video_set_clut(S_pal);
        { volatile uint16_t *clut=(volatile uint16_t*)0xF00400u;
          clut[254]=0x0000; clut[255]=0xFFFF;
          clut[g_pickidx]=0xFA37;                   /* gold pickup */
          clut[g_dooridx]=0x918D; }                  /* brown wood door */


        for (;;) {
            uint32_t pad;
            HLP(0);
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
#ifdef SKUNK_CONSOLE
            /* stream the raw probe counters every ~5s of vblanks */
            { extern volatile uint32_t frame_count;
              static uint32_t nextvb, lws, lvc;
              if (frame_count >= nextvb) {
                  uint32_t ws = *(volatile uint32_t *)0xF1C328u;
                  uint32_t vc = *(volatile uint32_t *)0xF1C330u;
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
              uint32_t c = *(volatile uint32_t *)0xF1C328u;  /* poll counter */
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
                uint32_t vcc = *(volatile uint32_t *)0xF1C330u;   /* WAKE_D reused */
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
#ifdef HEADSPIN
            g_layaw = (uint8_t)(g_layaw + 3);   /* diagnostic: spin Lara */
#endif
            fix cY, sY, cP=COS(CAM_PITCH), sP=SIN(CAM_PITCH);
            int camx, camy, camz, k;
            fbpix *fb;
            /* Lara tank controls + multi-room floor-follow / wall collision */
            { int mv=0, fy;
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
                  if (pad & PAD_LEFT)  g_layaw -= 3;
                  if (pad & PAD_RIGHT) g_layaw += 3;
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
              if (pad & PAD_LEFT)  g_layaw -= 3;
              if (pad & PAD_RIGHT) g_layaw += 3;
              if (pad & PAD_UP)   mv = 1;
              if (pad & PAD_DOWN) mv = -1;
              if (mv) {
                  int spd = (pad & PAD_C) ? (WALK_SPEED/2) : WALK_SPEED;  /* PAD_C = walk */
                  if (mv < 0) spd = (spd*3)>>2;            /* backing up is slower */
                  int nx = g_lax + (int)(((int32_t)SIN(g_layaw)*(spd*mv))>>16);
                  int nz = g_laz + (int)(((int32_t)COS(g_layaw)*(spd*mv))>>16);
                  int nf;   /* only walk onto a floor whose step-UP is small (a
                               tall step is a wall/ledge you can't just walk up;
                               this stops Lara popping onto raised sectors) */
                  if (!room_wall_at(rsect[g_curroom], nx, g_laz) &&
                      room_floor_mr(rsect, roomCount, nx, g_laz, &nf) &&
                      g_lafloor - nf <= LARA_STEPUP) g_lax = nx;
                  if (!room_wall_at(rsect[g_curroom], g_lax, nz) &&
                      room_floor_mr(rsect, roomCount, g_lax, nz, &nf) &&
                      g_lafloor - nf <= LARA_STEPUP) g_laz = nz;
              }
#ifdef DEMO_PROPS
              /* the shut door blocks the corridor until it has slid up */
              if (g_dooryoff > -1200 && g_laz > g_doorz - 300 &&
                  mr_iabs(g_lax - g_doorx) < 700) g_laz = g_doorz - 300;
#endif
              if (room_floor_mr(rsect, roomCount, g_lax, g_laz, &fy)) {
                  g_lafloor = fy;
                  g_curroom = g_floorroom;   /* the room Lara stands in */
              }
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
                      g_climbanim = LANIM_VAULT;        /* standing kick-flip vault */
                      goto lara_done;                  /* skip jump physics       */
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
                if ((jedge & PAD_A) && grounded && !jprep) jprep = 2;
                if (jprep && grounded) {
                    if (--jprep == 0) {
                        g_lavy = -JUMP_VEL;
                        g_lajf = (mv > 0);   /* running jump if moving at launch */
                    }
                } else if (!grounded) jprep = 0;
                g_lavy += GRAVITY;
                g_lay  += g_lavy;
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
                    int jx = g_lax + (int)(((int32_t)SIN(g_layaw) * JUMP_FWD) >> 16);
                    int jz = g_laz + (int)(((int32_t)COS(g_layaw) * JUMP_FWD) >> 16);
                    /* walls stop her in the air too (no jump-through) */
                    if (!room_wall_at(rsect[g_curroom], jx, g_laz)) g_lax = jx;
                    if (!room_wall_at(rsect[g_curroom], g_lax, jz)) g_laz = jz;
                }
                if (g_lay >= g_lafloor) {
                    /* thud only on REAL falls: slope descents micro-hop
                       airborne for 1-3 frames and re-land every stride,
                       which double/triple-tapped the step sound (user
                       report 2026-07-13) */
                    if (!grounded && g_lavy > 60 && g_airfr >= 5)
                        sfx_play(1, SFX_LAND);
                    g_lay = g_lafloor; g_lavy = 0; grounded = 1; g_lajf = 0;
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
                if (!grounded && (pad & PAD_B)) {
                    int px = g_lax + (int)(((int32_t)SIN(g_layaw)*(WALK_SPEED*2))>>16);
                    int pz = g_laz + (int)(((int32_t)COS(g_layaw)*(WALK_SPEED*2))>>16);
                    int lf, handY = g_lay - LARA_GRABREACH, lfok;
                    int cy1;
                    g_flr_grab = 1;
                    lfok = room_floor_mr(rsect, roomCount, px, pz, &lf);
                    g_flr_grab = 0;
                    if (lfok &&
                        lf >= handY - 256 && lf <= handY + 288 &&   /* generous grab window */
                        (g_lafloor - lf) > LARA_STEPUP &&
                        (!room_ceil_at(rsect[g_curroom], g_lax, g_laz, &cy1) ||
                         lf >= cy1) &&
                        room_reachable(g_curroom, g_floorroom)) {
                        g_layaw = ALIGN_WALL(g_layaw);   /* turn square to the wall */
                        g_vault = 1; g_vaulty = lf; g_vaultx = px; g_vaultz = pz;
                        g_climbf = 0; g_climby0 = g_lay;
                        sfx_play(1, SFX_GRUNT);
                        g_climbx0 = g_lax; g_climbz0 = g_laz;
                        /* TR1: WALK held during the pull-up = HANDSTAND */
                        g_climbanim = (pad & PAD_C) ? LANIM_HANDSTAND
                                                    : LANIM_HANGUP;
                    }
                }
                /* full moveset (runtime-skinned). airborne = jump cycle (one-
                   shot, clamps at land); fwd = run or WALK (PAD_C); back = the
                   hop-back anim; turning in place = turn L/R; else STAND. */
                if (!grounded) {
                    lanim_set(g_lajf ? LANIM_FJUMP : LANIM_UPJUMP);
                    lanim_step(0, 1);
                } else if (mv > 0) {
                    static int stepctr;
                    lanim_set((pad & PAD_C) ? LANIM_WALK : LANIM_RUN);
                    lanim_step(1, 2);
                    if (++stepctr >= ((pad & PAD_C) ? 5 : 3)) {
                        stepctr = 0;
                        sfx_play(0, SFX_STEP);
                    }
                } else if (mv < 0) {
                    lanim_set(LANIM_BACK);
                    lanim_step(1, 2);
                } else if (pad & (PAD_LEFT|PAD_RIGHT)) {
                    lanim_set((pad & PAD_LEFT) ? LANIM_TURNL : LANIM_TURNR);
                    lanim_step(1, 1);
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
            cY = COS(g_layaw); sY = SIN(g_layaw);
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
#ifdef PROFILE
            pfA = frame_count;
#endif
            HLP(1);
            fb = video_backbuffer();
            /* the ~76KB Blitter phrase-fill saturates the bus for 1-2ms; if
               it covers the vblank ISR's OP-list rebuild window the top
               scanlines drop for a field (the "bounce"). Dodge the window
               (see video_wait_safe_vc in video.c). */
            { extern void video_wait_safe_vc(void); video_wait_safe_vc(); }
            blit_band(fb, 0, RENDER_H, CLEAR_IDX);
            HLP(2);
#ifdef PROFILE
            pfB = frame_count;
#endif
            if (gpu_ok) {
                int ndrawn = 0;
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
                for (k=0;k<roomCount;k++) {
                    int ri = order[k];
                    long long dxl=(long long)(rcx[ri]-camx), dzl=(long long)(rcz[ri]-camz);
                    int depth = (int)((dxl*(long long)sY + dzl*(long long)cY) >> 16);
#ifdef ROOMCAP
                    /* perf experiment: draw only the ROOMCAP NEAREST rooms
                       (order[] is far-first, so the nearest are the last). */
                    if (k < roomCount - ROOMCAP) continue;
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
                    if (depth - rrad[ri] > 9000)   continue;   /* fully past far */
                    /* SLIVER CULL: a room seen only through a tiny distant
                       window costs full per-face setup for ~nothing on
                       screen (long corridors stacked 10+ rooms = the dip).
                       Skipping it leaves a few dark pixels in a doorway. */
                    if (ri != g_curroom && prv[ri] == 1 &&
                        (prx1[ri] - prx0[ri] < 16 || pry1[ri] - pry0[ri] < 6))
                        continue;
                    /* PORTAL-WINDOW CLIP: neighbour rooms render only inside
                       the doorway rect they're seen through; invisible
                       doorway = the room isn't drawn AT ALL. */
                    { int cx0=0, cx1=319, cy0=0, cy1=RENDER_H-1;
                      if (ri != g_curroom && prv[ri] == 1) {
                          cx0=prx0[ri]; cx1=prx1[ri];
                          cy0=pry0[ri]; cy1=pry1[ri];
                      }
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
                        if (ndrawn > 0 && rvc <= 512 && rqc <= 448 && rtc <= 256 && njx < 5) {
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
                if (ndrawn) {
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
                        HLP(3);
                        /* Jerry wrote lara_blob+16 directly (params[6]) */
                        lara_finish(lara_blob, atlasW);
                        HLP(4);
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
                        HLP(5);
                        g_jerry_frame_done = 1;
                    }
#endif
                    /* LARA RIDES THE DISPATCH: her blob is a geotex room
                       (self-transform, full-screen clip) appended LAST so
                       she paints over everything. Saves the second kick +
                       gpu_sync round-trip per frame; lara_finish completed
                       before this point so the blob is whole. */
                    { uint32_t dn = displist[0];
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
                    { uint32_t total = displist[0], base = 0;
                      static uint32_t batch[1+8*4];
                      while (base < total) {
                        uint32_t bn = total - base, bi;
                        if (bn > 8) bn = 8;
                        batch[0] = bn;
                        for (bi = 0; bi < bn*4; bi++)
                            batch[1+bi] = displist[1+base*4+bi];
                        gpu_geotex_dispatch(batch, fb, camblk, S_atlas, (uint32_t)atlasW);
                        HB(1);   /* stage 1: dispatch kicked */
                        gpu_sync();
                        base += bn;
                      } }
                    HLP(6);
                    HB(12);  /* stage 12: Tom done (all rooms) */
                } else if (posed < 2) {
                    build_lara_tex(lara_blob, atlasW); posed = 2;
                }
#ifdef PROFILE
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
#ifdef PROFILE   /* white bars on a black strip; rows: 68k / 5rooms / lara / wait.
                    255=white,254=black are reserved (not used by scene). Read the
                    bar LENGTHS by row (each = phase_time / total_frame_time). */
            { int yy,xx;
              for (yy=10;yy<30;yy++) for (xx=0;xx<RENDER_W;xx++) fb[yy*RENDER_W+xx]=254;
              for (xx=0;xx<bar68;xx++) fb[12*RENDER_W+xx]=255;
              for (xx=0;xx<barrm;xx++) fb[16*RENDER_W+xx]=255;
              for (xx=0;xx<barla;xx++) fb[20*RENDER_W+xx]=255;
              for (xx=0;xx<barwt;xx++) fb[24*RENDER_W+xx]=255;
              /* row 5 = ABSOLUTE fps (px = fps*100/3): longer = FASTER. Compare
                 builds directly by this bar's length. */
              for (xx=0;xx<barfps;xx++) fb[28*RENDER_W+xx]=255; }
#endif
            video_flip();
            HLP(7);
            /* FREE-RUN: no vblank wait. Triple buffering means the flip is
               latched by the vblank ISR whenever it lands; the render loop
               starts the next frame immediately in the third buffer instead
               of idling to a vblank boundary (~up to 16ms/frame reclaimed). */
#ifdef PROFILE
            /* accumulate per-phase vblank deltas; recompute bars every 60 frames.
               bar length (px) = phase_vblanks / total_vblanks * (RENDER_W-16). */
            pf68 += pfB-pfA; pfrm += pfC-pfB; pfla += pfD-pfC;
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
            pftt += frame_count-pfA; pfn++;
            if (pfn >= 60) {
                int W = RENDER_W-16, den = pftt ? (int)pftt : 1;
                uint32_t wt = pftt - (pf68+pfrm+pfla);
                bar68=(int)(pf68*(uint32_t)W/den); barrm=(int)(pfrm*(uint32_t)W/den);
                barla=(int)(pfla*(uint32_t)W/den); barwt=(int)(wt*(uint32_t)W/den);
                /* ABSOLUTE fps bar (row 5): px = fps*100/3, so 3fps=100px,
                   6fps=200px. Longer = FASTER. Compare builds by this directly. */
                { int fps100 = (int)((6000L*(long)pfn)/(long)den);
                  barfps = fps100/3; if (barfps > RENDER_W-1) barfps = RENDER_W-1; }
#ifdef SKUNK_CONSOLE
                if (skunk_up()) {
                    dbg_kv("=== MULTIROOM vbl over 60 frames ===", pfn);
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
