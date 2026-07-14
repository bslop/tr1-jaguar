// tr2jag.cpp
//
// HOST (Linux x86-64, g++) converter: extracts ONE Tomb Raider room's geometry
// from OpenLara's packed 32X level data (.PKD) and writes a simple BIG-ENDIAN
// binary ("room.bin") for an Atari Jaguar renderer.
//
// WHY WE PARSE THE .PKD DIRECTLY (instead of compiling OpenLara's read_PKD):
//   The 32X target is an SH2 => the .PKD is stored BIG-ENDIAN and its "pointer"
//   fields are 32-bit file-relative offsets. OpenLara's read_PKD() does
//   `memcpy(&level, data, sizeof(level))` then `*(uint32*)ptr += (uint32)data`
//   with NO byte swapping (fine on a big-endian 32-bit 32X, broken on a
//   little-endian 64-bit host: wrong endianness AND 64-bit pointers). So on x86-64
//   the ONLY correct route is to replicate the exact struct layout with explicit
//   big-endian reads. Every layout below is proven against the engine + packer:
//
//   Vertex scale (=256):  src/platform/32x/render.cpp:164-167  vx = vertices->x << 8 (etc)
//                         src/platform/gba/packer/out_32X.h:1408-1410  px = v.pos.x >> 8 (etc)
//   Room origin:          src/fixed/room.h:490  matrixTranslateAbs(info->x << 8, 0, info->z << 8)
//                         src/platform/gba/packer/out_32X.h:1566-1567  info.x = room->info.x / 256
//   Vertex Y is room-local, measured DOWN from yTop:
//                         src/platform/gba/packer/out_32X.h:1409  py = (v.pos.y - yOffset) >> 8, yOffset=info.yTop
//   Quad indices are DELTA-encoded int8 (accumulate, prev starts 0, prev=i3):
//                         src/platform/gba/packer/out_32X.h:1616-1642
//   Tri indices are absolute uint16 = vertexIndex<<3 (byte offset / 8):
//                         src/platform/gba/packer/out_32X.h:1662-1678
//   RoomVertex {u8 x,y,z,g}:  src/fixed/common.h:812-813
//   RoomInfo layout (56 bytes): src/platform/gba/packer/out_32X.h:143-201
//   Level header layout:        src/platform/gba/packer/out_32X.h:85-138
//   Texture {u32 tile,uv01,uv23}: src/fixed/common.h:1061-1063
//     tile = (page<<16)|NON_CACHE_ADDR(0x20000000): out_32X.h:487,504,22 ; page=256x256=65536B
//     texel lookup tile[(v<<8)|u]: src/platform/32x/rasterizer.h ; uv extraction render.cpp:650-653
//   Palette: 256 * u16 RGB555, R=bits0-4 G=5-9 B=10-14: src/fixed/common.cpp:1533-1536
//   Item record (12B) type,roomIndex,posx,posy,posz,intensity,flags: out_32X.h:574-598,1919-1925
//
// Output room.bin (all BIG-ENDIAN):
//   u16 magic=0x524D('RM'); u16 vertexCount; u16 quadCount; u16 triCount;
//   s16 originX, originY, originZ
//   verts: { s16 x,y,z; u16 shade }   (room-local world units = packed*256; shade higher=brighter)
//   quads: { u16 v0,v1,v2,v3; u16 color; u16 flags }
//   tris:  { u16 v0,v1,v2;    u16 color; u16 flags }

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

// -------- big-endian readers over the loaded file buffer --------
static std::vector<u8> gFile;

static inline u16 be16(u32 off) { return (u16)((gFile[off] << 8) | gFile[off + 1]); }
static inline s16 sbe16(u32 off) { return (s16)be16(off); }
static inline u32 be32(u32 off) {
    return ((u32)gFile[off] << 24) | ((u32)gFile[off+1] << 16) |
           ((u32)gFile[off+2] << 8) | (u32)gFile[off+3];
}

// -------- Level header field offsets (bytes from file start) --------
// counts (u16): version u32 @0, then 14 u16 counts @4..31
enum {
    OFF_VERSION       = 0,
    OFF_TILES_COUNT   = 4,
    OFF_ROOMS_COUNT   = 6,
    OFF_TEX_COUNT     = 20,
    OFF_ITEMS_COUNT   = 24,
    // pointer table (u32 file-relative offsets), starts @32
    OFF_P_PALETTE     = 32,
    OFF_P_TILES       = 40,
    OFF_P_ROOMSINFO   = 44,
    OFF_P_TEXTURES    = 92,   // objectTextures
    OFF_P_ITEMS       = 148,  // itemsInfo (after overlaps@116, zones[2][3]@120..143, animTexData@144)
};

#define ROOMINFO_SIZE 56
#define ITEM_SIZE     12
#define TEX_SIZE      12
#define TILE_PAGE     65536
#define NON_CACHE     0x20000000u

// ============================================================================
//  LARA MESH extraction support (see buildLaraMesh at end of main)
//
//  Additional header field offsets (out_32X.h Header::write order, proof lines):
//    modelsCount  u16 @8   : out_32X.h:88  (3rd u16 after magic u32)
//    meshesCount  u16 @10  : out_32X.h:89  (== meshOffsetsCount)
//    pointer table @32: palette,lightmap,tiles,rooms,floors,meshData,meshOffsets,
//                       anims,states,ranges,commands,nodes,frameData,models,...
//      meshData    u32 @52 : out_32X.h:106
//      meshOffsets u32 @56 : out_32X.h:107
//      anims       u32 @60 : out_32X.h:108
//      nodes       u32 @76 : out_32X.h:112
//      frameData   u32 @80 : out_32X.h:113
//      models      u32 @84 : out_32X.h:114
//
//  Model (8B): u8 type@0, s8 count@1, u16 start@2, u16 nodeIndex@4, u16 animIndex@6
//              out_32X.h:784-791 ; nodeIndex already /4 (out_32X.h:2064)
//  Anim (32B): u32 frameOffset@0, u8 frameRate@4, u8 frameSize@5, ...  out_32X.h:181-197
//  Node/ModelNode (8B): s16 x@0,y@2,z@4, u16 flags@6   out_32X.h:289-301,2043-2049
//  meshOffsets[]: s32 byte-offset into meshData, one per mesh index  out_32X.h:2005-2006
//  Mesh header (20B) then verts: out_32X.h:664-693 / common.h:971-982
//    center s16 x@0,y@2,z@4 ; radius s16 @6 ; intensity u16 @8 ;
//    vCount u8 @10 ; hasNormals u8 @11 ; rCount s16 @12 (= textured+colored quads);
//    tCount s16 @14 (= textured+colored tris) ; @16,@18 = 0 ; verts @20
//  Mesh vertex on disk = origWorldUnit << 2  (out_32X.h:686-688, MESH_SHIFT/F16_SHIFT=2)
//    => WORLD UNITS (room.bin scale) = onDiskMeshVert >> 2  (i.e. / 4)
//  Mesh face (6B each): u16 flags + u8 indices[4]  (quad uses 4, tri uses 3+pad)
//    out_32X.h:245-273 ; layout order = [rCount quads][tCount tris]
//    flags = texIndex(&0x3FFF) | (FACE_TYPE << 14); FACE_TYPE 0=F(colored)1=FT 2=FTA
//    FACE_TEXTURE=0x3FFF (packer common.h:554), FACE_TYPE_SHIFT=14 (out_32X.h:18)
//  Pose = model->animIndex first anim, FIRST FRAME. Assembled by replicating
//    drawNodes (draw.h:246-287) + matrixFrame (common.cpp:1338-1359) +
//    DECODE_ANGLES big-endian (common.h:2652-2656) + matrixRotate*/TranslateRel
//    (common.cpp:1230-1288). frame @ frameData+frameOffset ; frame layout:
//    box[12] + pos s16 x@12,y@14,z@16 + (skip 2) + angles: count * u32 @20
//    (draw.h:254 `(uint32*)(frameA->angles + 1)` => +20 bytes; item.h:869-891)
// ============================================================================

// 3x4 double matrix (rotation r[row][col] + translation t) for host baking.
struct Mat { double r[3][3]; double t[3]; };
static Mat matIdentity() {
    Mat m; for (int i=0;i<3;i++){ for(int j=0;j<3;j++) m.r[i][j]=(i==j)?1.0:0.0; m.t[i]=0.0; }
    return m;
}
// matrixTranslateRel_c (common.cpp:1230): t += R * (x,y,z)
static void matTranslateRel(Mat &m, double x, double y, double z) {
    for (int i=0;i<3;i++) m.t[i] += m.r[i][0]*x + m.r[i][1]*y + m.r[i][2]*z;
}
// X_ROTXY(a,b,s,c): a'=a*c-b*s ; b'=b*c+a*s   (common.h:500-507)
static void matRotAxis(Mat &m, int ia, int ib, double ang) {
    double c = cos(ang), s = sin(ang);
    for (int i=0;i<3;i++) {
        double a = m.r[i][ia], b = m.r[i][ib];
        m.r[i][ia] = a*c - b*s;
        m.r[i][ib] = b*c + a*s;
    }
}
// matrixRotateX_c: X_ROTXY(e02,e01) => cols(2,1) ; Y: X_ROTXY(e00,e02)=>cols(0,2)
// Z: X_ROTXY(e01,e00)=>cols(1,0)   (common.cpp:1257-1288)
static void matRotX(Mat &m, double a){ matRotAxis(m,2,1,a); }
static void matRotY(Mat &m, double a){ matRotAxis(m,0,2,a); }
static void matRotZ(Mat &m, double a){ matRotAxis(m,1,0,a); }
// matrixRotateYXZ_c (common.cpp:1331): Y then X then Z (skip zero for parity)
static void matRotYXZ(Mat &m, int aX,int aY,int aZ){
    const double K = 2.0*M_PI/65536.0;
    if (aY) matRotY(m, aY*K);
    if (aX) matRotX(m, aX*K);
    if (aZ) matRotZ(m, aZ*K);
}

// Pack Jaguar RGB16 = R5[15:11] . B5[10:6] . G6[5:0]  (blue in the MIDDLE)
static inline u16 jagRGB16(int r5, int g5, int b5) {
    if (r5 < 0) r5 = 0; if (r5 > 31) r5 = 31;
    if (g5 < 0) g5 = 0; if (g5 > 31) g5 = 31;
    if (b5 < 0) b5 = 0; if (b5 > 31) b5 = 31;
    int g6 = (g5 << 1) | (g5 >> 4); // 5-bit -> 6-bit
    return (u16)((r5 << 11) | (b5 << 6) | g6);
}

int main(int argc, char** argv) {
    const char* inPath  = "LEVEL1.PKD";
    const char* outPath = "room.bin";
    int wantRoom = -1; // -1 => default (Lara start if found, else 0)

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) inPath = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outPath = argv[++i];
        else wantRoom = atoi(argv[i]);
    }

    // load file
    FILE* f = fopen(inPath, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", inPath); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    gFile.resize(sz);
    if (fread(gFile.data(), 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 1; }
    fclose(f);

    printf("input        : %s (%ld bytes)\n", inPath, sz);
    printf("version      : 0x%08X\n", be32(OFF_VERSION));

    int roomsCount = be16(OFF_ROOMS_COUNT);
    int texCount   = be16(OFF_TEX_COUNT);
    int tilesCount = be16(OFF_TILES_COUNT);
    int itemsCount = be16(OFF_ITEMS_COUNT);
    printf("roomsCount   : %d\n", roomsCount);
    printf("texturesCount: %d\n", texCount);
    printf("tilesCount   : %d\n", tilesCount);
    printf("itemsCount   : %d\n", itemsCount);

    u32 pPalette   = be32(OFF_P_PALETTE);
    u32 pTiles     = be32(OFF_P_TILES);
    u32 pRoomsInfo = be32(OFF_P_ROOMSINFO);
    u32 pTextures  = be32(OFF_P_TEXTURES);
    u32 pItems     = be32(OFF_P_ITEMS);
    printf("palette@%u tiles@%u roomsInfo@%u textures@%u items@%u\n",
           pPalette, pTiles, pRoomsInfo, pTextures, pItems);

    // find Lara start room (item type 0 == ITEM_LARA)
    int laraRoom = -1;
    u32 laraItemOff = 0;
    for (int i = 0; i < itemsCount; i++) {
        u32 io = pItems + i * ITEM_SIZE;
        u8 type = gFile[io];
        u8 rIdx = gFile[io + 1];
        if (type == 0) { laraRoom = rIdx; laraItemOff = io; break; }
    }
    printf("lara start room: %d%s\n", laraRoom, laraRoom < 0 ? " (not found)" : "");

    int room = wantRoom;
    if (room < 0) room = (laraRoom >= 0) ? laraRoom : 0;
    if (room >= roomsCount) { fprintf(stderr, "room %d >= roomsCount %d\n", room, roomsCount); return 1; }
    printf("chosen room  : %d\n", room);

    // ----- read RoomInfo -----
    u32 ri = pRoomsInfo + room * ROOMINFO_SIZE;
    s16 infoX  = sbe16(ri + 0);
    s16 infoZ  = sbe16(ri + 2);
    s16 yBottom= sbe16(ri + 4);
    s16 yTop   = sbe16(ri + 6);
    int quadsCount = be16(ri + 8);
    int trisCount  = be16(ri + 10);
    int vertsCount = be16(ri + 12);
    u32 pQuads = be32(ri + 24);
    u32 pTris  = be32(ri + 28);
    u32 pVerts = be32(ri + 32);

    // Info struct byte offsets (out_32X.h Info::write, 56 bytes):
    //   xSectors u8 @20, zSectors u8 @21, sectors ptr u32 @44
    int xSectors = gFile[ri + 20];
    int zSectors = gFile[ri + 21];
    u32 pSectors = be32(ri + 44);

    printf("room info    : infoX=%d infoZ=%d yTop=%d yBottom=%d\n", infoX, infoZ, yTop, yBottom);
    printf("counts       : verts=%d quads=%d tris=%d\n", vertsCount, quadsCount, trisCount);
    printf("sectors      : xSectors=%d zSectors=%d sectors@%u\n", xSectors, zSectors, pSectors);

    // ----- Lara start (item type ITEM_LARA==0). Item record (out_32X.h Item::write, 12B):
    //   u8 type@0, u8 roomIndex@1, s16 pos.x@2, s16 pos.y@4, s16 pos.z@6, s16 intensity@8, u16 flags@10
    //   angleY quantized to 90deg steps is packed into flags bits 14-15:
    //     flags |= ((angleY/0x4000 + 2) << 14)   (out_32X.h:1925)
    //   recover: q=(flags>>14)&3; angleY = (q-2)*0x4000  (0x10000 = full turn)
    s16 laraLocalX = -1, laraLocalY = -1, laraLocalZ = -1;
    u16 laraAngle = 0;
    bool laraHere = (laraItemOff != 0 && laraRoom == room);
    if (laraHere) {
        s16 wx = sbe16(laraItemOff + 2);
        s16 wy = sbe16(laraItemOff + 4);
        s16 wz = sbe16(laraItemOff + 6);
        u16 iflags = be16(laraItemOff + 10);
        // The packer ALREADY stores item pos as room-local for X/Z, absolute for Y
        //   (out_32X.h:1921-1923): pos.x = item.x - room.info.x ; pos.z = item.z - room.info.z ; pos.y = item.y
        // So pos.x / pos.z ARE the room-local world units (same space as vertices),
        // and pos.y needs only yTop removed to match the vertex Y space (+Y down):
        //   localY = worldY - yTop
        laraLocalX = wx;
        laraLocalY = (s16)((s32)wy - (s32)yTop);
        laraLocalZ = wz;
        int q = (iflags >> 14) & 3;
        laraAngle = (u16)(((q - 2) * 0x4000) & 0xFFFF);
    }

    // ----- vertices -----
    struct OutVert { s16 x, y, z; u16 shade; u8 g; };
    std::vector<OutVert> verts(vertsCount);
    int minX=1<<30,minY=1<<30,minZ=1<<30, maxX=-(1<<30),maxY=-(1<<30),maxZ=-(1<<30);
    int gMin=255,gMax=0;
    for (int i = 0; i < vertsCount; i++) {
        u32 vo = pVerts + i * 4;
        int px = gFile[vo + 0];       // packed 0..127
        int py = gFile[vo + 1];
        int pz = gFile[vo + 2];
        int g  = gFile[vo + 3];       // engine shade: HIGHER = DARKER (see notes)
        int wx = px << 8;             // room-local world units (scale 256)
        int wy = py << 8;
        int wz = pz << 8;
        verts[i].x = (s16)wx;
        verts[i].y = (s16)wy;
        verts[i].z = (s16)wz;
        verts[i].g = (u8)g;
        // shade output: task wants "higher = brighter". Engine g is darkness (higher=darker),
        // so invert. g is 5-bit (lighting>>8). brightness8 = (31-g)*255/31.
        int gc = g > 31 ? 31 : g;
        int bright8 = (31 - gc) * 255 / 31;
        verts[i].shade = (u16)bright8;
        if (wx<minX)minX=wx; if (wx>maxX)maxX=wx;
        if (wy<minY)minY=wy; if (wy>maxY)maxY=wy;
        if (wz<minZ)minZ=wz; if (wz>maxZ)maxZ=wz;
        if (g<gMin)gMin=g; if (g>gMax)gMax=g;
    }

    // int16 fit check + divisor
    int divisor = 1;
    int absmax = 0;
    int lim[6] = {minX,maxX,minY,maxY,minZ,maxZ};
    for (int i=0;i<6;i++){ int a = lim[i]<0?-lim[i]:lim[i]; if (a>absmax) absmax=a; }
    while (absmax / divisor > 32767) divisor++;
    if (divisor > 1) {
        for (auto &v : verts) { v.x/=divisor; v.y/=divisor; v.z/=divisor; }
        printf("NOTE: coords exceeded int16, divided all vertex coords by %d\n", divisor);
    }

    // ----- palette (256 * u16 RGB555) -----
    // helper: RGB555 (5-bit each) for palette index
    auto palRGB = [&](int idx, int &r5, int &g5, int &b5) {
        u16 p = be16(pPalette + (idx & 0xFF) * 2);
        r5 =  p        & 31;
        g5 = (p >> 5)  & 31;
        b5 = (p >> 10) & 31;
    };

    bool haveTextures = (pTextures != 0 && pTiles != 0 && pPalette != 0 && texCount > 0);

    // representative color for a face given its texture index + avg brightness(0..255)
    auto faceColor = [&](int texIndex, int avgBright) -> u16 {
        if (!haveTextures || texIndex < 0 || texIndex >= texCount) {
            // grey fallback from brightness
            int c5 = avgBright * 31 / 255;
            return jagRGB16(c5, c5, c5);
        }
        u32 to = pTextures + texIndex * TEX_SIZE;
        u32 tile = be32(to + 0);
        u32 uv01 = be32(to + 4);
        u32 uv23 = be32(to + 8);
        u32 pageByteOff = tile & 0x00FFFFFF; // strips NON_CACHE 0x20000000, keeps page<<16
        // corner uv extraction (render.cpp:650-653 semantics: idx = (v<<8)|u)
        int u0=(uv01>>24)&0xFF, v0=(uv01>>8)&0xFF;
        int u1=(uv01>>16)&0xFF, v1=(uv01)&0xFF;
        int u2=(uv23>>24)&0xFF, v2=(uv23>>8)&0xFF;
        int u3=(uv23>>16)&0xFF, v3=(uv23)&0xFF;
        int umin=u0,umax=u0,vmin=v0,vmax=v0;
        int us[4]={u0,u1,u2,u3}, vs[4]={v0,v1,v2,v3};
        for (int k=0;k<4;k++){ if(us[k]<umin)umin=us[k]; if(us[k]>umax)umax=us[k];
                               if(vs[k]<vmin)vmin=vs[k]; if(vs[k]>vmax)vmax=vs[k]; }
        // sample a small grid, average non-transparent (index!=0) texels
        long ar=0,ag=0,ab=0; int n=0, nAny=0; long ar2=0,ag2=0,ab2=0;
        int su = (umax-umin)/4; if (su<1) su=1;
        int sv = (vmax-vmin)/4; if (sv<1) sv=1;
        for (int vv=vmin; vv<=vmax; vv+=sv)
        for (int uu=umin; uu<=umax; uu+=su) {
            u32 addr = pTiles + pageByteOff + ((vv & 0xFF) << 8) + (uu & 0xFF);
            if (addr >= gFile.size()) continue;
            int idx = gFile[addr];
            int r5,g5,b5; palRGB(idx, r5,g5,b5);
            ar2+=r5; ag2+=g5; ab2+=b5; nAny++;
            if (idx != 0) { ar+=r5; ag+=g5; ab+=b5; n++; }
        }
        int r5,g5,b5;
        if (n>0)      { r5=ar/n;  g5=ag/n;  b5=ab/n; }
        else if(nAny>0){ r5=ar2/nAny; g5=ag2/nAny; b5=ab2/nAny; }
        else          { int c5=avgBright*31/255; return jagRGB16(c5,c5,c5); }
        // modulate by average face brightness (0..255)
        r5 = r5 * avgBright / 255;
        g5 = g5 * avgBright / 255;
        b5 = b5 * avgBright / 255;
        return jagRGB16(r5,g5,b5);
    };

    // ----- quads (delta-decoded int8 indices) -----
    struct OutQuad { u16 v[4]; u16 color; u16 flags; };
    std::vector<OutQuad> quads(quadsCount);
    {
        int prev = 0;
        for (int i = 0; i < quadsCount; i++) {
            u32 qo = pQuads + i * 8; // {u32 flags; s8 indices[4]}
            u32 flags = be32(qo + 0);
            s8 p0 = (s8)gFile[qo + 4];
            s8 p1 = (s8)gFile[qo + 5];
            s8 p2 = (s8)gFile[qo + 6];
            s8 p3 = (s8)gFile[qo + 7];
            int i0 = prev + p0;
            int i1 = i0 + p1;
            int i2 = i1 + p2;
            int i3 = i2 + p3;
            prev = i3;
            quads[i].v[0]=(u16)i0; quads[i].v[1]=(u16)i1;
            quads[i].v[2]=(u16)i2; quads[i].v[3]=(u16)i3;
            int texIndex = flags & 0x3FFF;
            int bsum=0;
            int ii[4]={i0,i1,i2,i3};
            for(int k=0;k<4;k++){ int g=(ii[k]>=0&&ii[k]<vertsCount)?verts[ii[k]].g:0;
                                  int gc=g>31?31:g; bsum += (31-gc)*255/31; }
            int avgB = bsum/4;
            quads[i].color = faceColor(texIndex, avgB);
            quads[i].flags = (u16)(flags & 0xFFFF);
        }
    }

    // ----- triangles (absolute uint16 = vertexIndex<<3) -----
    struct OutTri { u16 v[3]; u16 color; u16 flags; };
    std::vector<OutTri> tris(trisCount);
    for (int i = 0; i < trisCount; i++) {
        u32 to = pTris + i * 8; // {u16 flags; u16 indices[3]}
        u16 flags = be16(to + 0);
        int i0 = be16(to + 2) >> 3;
        int i1 = be16(to + 4) >> 3;
        int i2 = be16(to + 6) >> 3;
        tris[i].v[0]=(u16)i0; tris[i].v[1]=(u16)i1; tris[i].v[2]=(u16)i2;
        int texIndex = flags & 0x3FFF;
        int bsum=0; int ii[3]={i0,i1,i2};
        for(int k=0;k<3;k++){ int g=(ii[k]>=0&&ii[k]<vertsCount)?verts[ii[k]].g:0;
                              int gc=g>31?31:g; bsum += (31-gc)*255/31; }
        int avgB = bsum/3;
        tris[i].color = faceColor(texIndex, avgB);
        tris[i].flags = flags;
    }

    // ----- write room.bin (BIG-ENDIAN) -----
    std::vector<u8> out;
    auto w16 = [&](u16 v){ out.push_back(v>>8); out.push_back(v&0xFF); };
    w16(0x524D);                 // magic 'RM'
    w16((u16)vertsCount);
    w16((u16)quadsCount);
    w16((u16)trisCount);
    w16((u16)(s16)infoX);        // originX  (world X = originX*256 + vx)
    w16((u16)(s16)yTop);         // originY  (world Y = originY   + vy)
    w16((u16)(s16)infoZ);        // originZ  (world Z = originZ*256 + vz)
    for (auto &v : verts) { w16((u16)v.x); w16((u16)v.y); w16((u16)v.z); w16(v.shade); }
    for (auto &q : quads) { w16(q.v[0]); w16(q.v[1]); w16(q.v[2]); w16(q.v[3]); w16(q.color); w16(q.flags); }
    for (auto &t : tris)  { w16(t.v[0]); w16(t.v[1]); w16(t.v[2]); w16(t.color); w16(t.flags); }

    // ----- sector/collision trailer (BIG-ENDIAN), appended after Tris -----
    // Semantics (proof lines):
    //   floor scale : Sector.floor(int8) -> worldY = floor << 8   (room.h:33,260,649)
    //   local Y     : localFloorY = (floor << 8) - yTop  (vertex space, +Y down)
    //   wall marker : NO_FLOOR == -127 (common.h:477); getFloor()->WALL==NO_FLOOR*256 (common.h:478)
    //   index order : sectors[sx*zSectors + sz]  (room.h:199)  sx=(x-roomX)>>10, sz=(z-roomZ)>>10
    size_t trailerOff = out.size();
    w16((u16)xSectors);
    w16((u16)zSectors);
    w16((u16)laraLocalX);
    w16((u16)laraLocalY);
    w16((u16)laraLocalZ);
    w16(laraAngle);
    const int NO_FLOOR = -127;
    // Lara's sector via the same formula the engine uses: sx=(localX)>>10, sz=(localZ)>>10
    int laraSx = laraHere ? (((int)laraLocalX) >> 10) : -1;
    int laraSz = laraHere ? (((int)laraLocalZ) >> 10) : -1;
    for (int sx = 0; sx < xSectors; sx++) {
        for (int sz = 0; sz < zSectors; sz++) {
            u32 so = pSectors + (u32)(sx * zSectors + sz) * 8;
            // Sector (8B): u16 floorIndex@0, u16 boxIndex@2, u8 roomBelow@4, s8 floor@5, u8 roomAbove@6, s8 ceiling@7
            s8 floor = (s8)gFile[so + 5];
            int walkable = (floor != NO_FLOOR) ? 1 : 0;
            s16 floorY = 0;
            if (walkable) floorY = (s16)(((s32)floor << 8) - (s32)yTop);
            w16((u16)floorY);
            w16((u16)walkable);
        }
    }

    FILE* of = fopen(outPath, "wb");
    if (!of) { fprintf(stderr, "cannot write %s\n", outPath); return 1; }
    fwrite(out.data(), 1, out.size(), of);
    fclose(of);

    // ----- stats -----
    printf("\n=== RESULTS ===\n");
    printf("coord scale     : packed_u8 * 256 = world units (divisor=%d)\n", divisor);
    printf("vertex world X  : [%d .. %d]\n", minX/divisor, maxX/divisor);
    printf("vertex world Y  : [%d .. %d]  (relative to yTop, +Y=down)\n", minY/divisor, maxY/divisor);
    printf("vertex world Z  : [%d .. %d]\n", minZ/divisor, maxZ/divisor);
    printf("raw vertex g    : [%d .. %d]  (engine: HIGHER=DARKER; shade output inverted so higher=brighter)\n", gMin, gMax);
    printf("colors          : %s\n", haveTextures ? "REAL texture-derived (avg palette RGB, brightness-modulated)"
                                                   : "GREY-SHADE fallback (no textures)");
    printf("origin (s16)    : originX=%d originY(yTop)=%d originZ=%d\n", infoX, yTop, infoZ);
    printf("  world formula : Xw=originX*256+vx  Yw=yTop+vy  Zw=originZ*256+vz\n");
    printf("room.bin bytes  : %zu\n", out.size());
    printf("output          : %s\n", outPath);

    // ----- sector trailer report -----
    printf("\n=== SECTOR TRAILER ===\n");
    size_t predicted = 14u + (size_t)vertsCount*8 + (size_t)quadsCount*12 + (size_t)trisCount*10;
    printf("trailer offset  : %zu bytes  (formula 14 + vc*8 + qc*12 + tc*10 = %zu)  %s\n",
           trailerOff, predicted, trailerOff == predicted ? "MATCH" : "MISMATCH!");
    printf("xSectors=%d zSectors=%d  (index = sx*zSectors + sz)\n", xSectors, zSectors);
    printf("floor scale     : worldY = floor<<8 ; localFloorY = (floor<<8) - yTop  (yTop=%d)\n", yTop);
    printf("wall marker     : floor == -127 (NO_FLOOR) -> walkable=0\n");
    if (laraHere) {
        printf("Lara local x/y/z: %d / %d / %d   angle=0x%04X (%d deg, 0x10000=full turn)\n",
               laraLocalX, laraLocalY, laraLocalZ, laraAngle, laraAngle * 360 / 65536);
        printf("Lara sector     : sx=%d sz=%d\n", laraSx, laraSz);
        if (laraSx >= 0 && laraSx < xSectors && laraSz >= 0 && laraSz < zSectors) {
            u32 so = pSectors + (u32)(laraSx * zSectors + laraSz) * 8;
            s8 floor = (s8)gFile[so + 5];
            int walkable = (floor != -127) ? 1 : 0;
            s16 fy = walkable ? (s16)(((s32)floor << 8) - (s32)yTop) : 0;
            printf("  -> floorY=%d walkable=%d (floor=%d)  [sanity: expect walkable=1]\n",
                   fy, walkable, floor);
        }
    } else {
        printf("Lara not in this room (localX/Y/Z = -1)\n");
    }
    // sample records + find an obvious wall sector
    printf("sample sectors (sx,sz -> floorY,walkable):\n");
    int shownWall = 0;
    for (int sx = 0; sx < xSectors; sx++)
    for (int sz = 0; sz < zSectors; sz++) {
        u32 so = pSectors + (u32)(sx * zSectors + sz) * 8;
        s8 floor = (s8)gFile[so + 5];
        int walkable = (floor != -127) ? 1 : 0;
        s16 fy = walkable ? (s16)(((s32)floor << 8) - (s32)yTop) : 0;
        bool isLara = (sx == laraSx && sz == laraSz);
        // print first few, Lara's, and the first wall
        if ((sx*zSectors+sz) < 4 || isLara || (!walkable && shownWall < 2)) {
            printf("  (%d,%d) -> floorY=%4d walkable=%d%s%s\n", sx, sz, fy, walkable,
                   isLara ? "  <-- Lara" : "", (!walkable) ? "  <-- WALL" : "");
            if (!walkable) shownWall++;
        }
    }

    printf("\nfirst 64 bytes of room.bin:\n");
    for (int i = 0; i < 64 && i < (int)out.size(); i++) {
        printf("%02X ", out[i]);
        if ((i & 15) == 15) printf("\n");
    }
    printf("\n");

    // ========================================================================
    //  MULTI-ROOM -> rooms.bin  (big-endian, one COMMON coordinate space)
    //
    //  Exports room 0 plus every room reachable through its portals within
    //  2 hops (BFS from room 0, capped at 12 rooms). All rooms share room 0's
    //  world origin so the Jaguar renders them in one space. Per-room verts stay
    //  ROOM-LOCAL (unchanged from room.bin); each room carries a world offset:
    //     offX = (thisRoom.originX - room0.originX) * 256   (WORLD units)
    //     offZ = (thisRoom.originZ - room0.originZ) * 256
    //     yTopDelta = thisRoom.yTop - room0.yTop
    //  Jaguar renders a vertex at (localX+offX, localY+yTopDelta, localZ+offZ).
    //
    //  On-disk Portal record (32B). NOTE: out_32X.h writes the RAW TR1_PC portal
    //  (out_32X.h:1722 f.writeObj(room->portals) -> TR1_PC.h:59-77), NOT the
    //  compressed form. Layout (all big-endian s16):
    //     s16 roomIndex@0, vec3s normal@2 (nx,ny,nz), vec3s vertices[4]@8 (x,y,z each)
    //     portal vertex X,Z = ROOM-LOCAL world units; Y = ABSOLUTE world Y
    //       (room.h:362-364: world = v.x + info->x<<8, v.z + info->z<<8, v.y as-is)
    //  Portals ptr @ RoomInfo+40, portalsCount u8 @ RoomInfo+16.
    // ========================================================================
    {
        const char* roomsOut = "rooms.bin";

        struct RVert { s16 x, y, z; u16 shade; u8 g; };
        struct RQuad { u16 v[4], color, flags; };
        struct RTri  { u16 v[3], color, flags; };
        struct RPortal { int roomIndex, nx, ny, nz; int vx[4], vy[4], vz[4]; };
        struct RoomData {
            int idx;
            s16 infoX, infoZ, yTop, yBottom;
            int xSectors, zSectors;
            std::vector<RVert> verts;
            std::vector<RQuad> quads;
            std::vector<RTri>  tris;
            std::vector<s16>   secFloorY;
            std::vector<u16>   secWalk;
            std::vector<RPortal> portals;
        };

        // read just the portal list of a room (used by BFS)
        auto readPortals = [&](int roomIdx, std::vector<RPortal> &pv) {
            u32 riX = pRoomsInfo + (u32)roomIdx * ROOMINFO_SIZE;
            int pc = gFile[riX + 16];
            u32 pp = be32(riX + 40);
            pv.clear();
            for (int i = 0; i < pc; i++) {
                u32 po = pp + (u32)i * 32;   // 32-byte TR1_PC portal record
                RPortal p;
                p.roomIndex = sbe16(po + 0);
                p.nx = sbe16(po + 2); p.ny = sbe16(po + 4); p.nz = sbe16(po + 6);
                for (int k = 0; k < 4; k++) {
                    p.vx[k] = sbe16(po + 8 + k*6 + 0); // room-local X (world units)
                    p.vy[k] = sbe16(po + 8 + k*6 + 2); // absolute Y
                    p.vz[k] = sbe16(po + 8 + k*6 + 4); // room-local Z (world units)
                }
                pv.push_back(p);
            }
        };

        // full geometry extraction for one room (mirrors the single-room path)
        auto extractRoom = [&](int roomIdx) -> RoomData {
            RoomData R; R.idx = roomIdx;
            u32 riX = pRoomsInfo + (u32)roomIdx * ROOMINFO_SIZE;
            R.infoX   = sbe16(riX + 0);
            R.infoZ   = sbe16(riX + 2);
            R.yBottom = sbe16(riX + 4);
            R.yTop    = sbe16(riX + 6);
            int qc = be16(riX + 8);
            int tc = be16(riX + 10);
            int vc = be16(riX + 12);
            R.xSectors = gFile[riX + 20];
            R.zSectors = gFile[riX + 21];
            u32 pQ = be32(riX + 24);
            u32 pT = be32(riX + 28);
            u32 pV = be32(riX + 32);
            u32 pS = be32(riX + 44);

            R.verts.resize(vc);
            for (int i = 0; i < vc; i++) {
                u32 vo = pV + (u32)i * 4;
                int g = gFile[vo + 3];
                R.verts[i].x = (s16)(gFile[vo + 0] << 8);
                R.verts[i].y = (s16)(gFile[vo + 1] << 8);
                R.verts[i].z = (s16)(gFile[vo + 2] << 8);
                R.verts[i].g = (u8)g;
                int gc = g > 31 ? 31 : g;
                R.verts[i].shade = (u16)((31 - gc) * 255 / 31);
            }
            // quads (delta-decoded int8 indices)
            R.quads.resize(qc);
            {
                int prev = 0;
                for (int i = 0; i < qc; i++) {
                    u32 qo = pQ + (u32)i * 8;
                    u32 fl = be32(qo + 0);
                    int i0 = prev + (s8)gFile[qo + 4];
                    int i1 = i0   + (s8)gFile[qo + 5];
                    int i2 = i1   + (s8)gFile[qo + 6];
                    int i3 = i2   + (s8)gFile[qo + 7];
                    prev = i3;
                    R.quads[i].v[0]=(u16)i0; R.quads[i].v[1]=(u16)i1;
                    R.quads[i].v[2]=(u16)i2; R.quads[i].v[3]=(u16)i3;
                    int ii[4]={i0,i1,i2,i3}, bsum=0;
                    for (int k=0;k<4;k++){ int g=(ii[k]>=0&&ii[k]<vc)?R.verts[ii[k]].g:0;
                                           int gc=g>31?31:g; bsum += (31-gc)*255/31; }
                    R.quads[i].color = faceColor(fl & 0x3FFF, bsum/4);
                    R.quads[i].flags = (u16)(fl & 0xFFFF);
                }
            }
            // tris (absolute uint16 = vertexIndex<<3)
            R.tris.resize(tc);
            for (int i = 0; i < tc; i++) {
                u32 to = pT + (u32)i * 8;
                u16 fl = be16(to + 0);
                int i0 = be16(to + 2) >> 3;
                int i1 = be16(to + 4) >> 3;
                int i2 = be16(to + 6) >> 3;
                R.tris[i].v[0]=(u16)i0; R.tris[i].v[1]=(u16)i1; R.tris[i].v[2]=(u16)i2;
                int ii[3]={i0,i1,i2}, bsum=0;
                for (int k=0;k<3;k++){ int g=(ii[k]>=0&&ii[k]<vc)?R.verts[ii[k]].g:0;
                                       int gc=g>31?31:g; bsum += (31-gc)*255/31; }
                R.tris[i].color = faceColor(fl & 0x3FFF, bsum/3);
                R.tris[i].flags = fl;
            }
            // sectors (floorY in THIS room's local Y space, same as room.bin)
            const int NO_FLOOR_L = -127;
            R.secFloorY.resize((size_t)R.xSectors * R.zSectors);
            R.secWalk.resize((size_t)R.xSectors * R.zSectors);
            for (int sx = 0; sx < R.xSectors; sx++)
            for (int sz = 0; sz < R.zSectors; sz++) {
                u32 so = pS + (u32)(sx * R.zSectors + sz) * 8;
                s8 floor = (s8)gFile[so + 5];
                int walk = (floor != NO_FLOOR_L) ? 1 : 0;
                s16 fy = walk ? (s16)(((s32)floor << 8) - (s32)R.yTop) : 0;
                size_t idx = (size_t)(sx * R.zSectors + sz);
                R.secFloorY[idx] = fy;
                R.secWalk[idx]   = (u16)walk;
            }
            readPortals(roomIdx, R.portals);
            return R;
        };

        // ---- BFS from room 0, within 2 hops, cap 12 rooms ----
        const int MAX_ROOMS = 12;
        const int MAX_HOPS   = 2;
        std::vector<int> order;                 // exported room indices (BFS order)
        std::vector<int> visited(roomsCount, 0);
        std::vector<std::pair<int,int>> q;      // (roomIndex, hop)
        size_t head = 0;
        q.push_back({0, 0});
        while (head < q.size() && (int)order.size() < MAX_ROOMS) {
            int r   = q[head].first;
            int hop = q[head].second;
            head++;
            if (r < 0 || r >= roomsCount || visited[r]) continue;
            visited[r] = 1;
            order.push_back(r);
            if (hop < MAX_HOPS) {
                std::vector<RPortal> pv; readPortals(r, pv);
                for (auto &p : pv) {
                    int nb = p.roomIndex;
                    if (nb >= 0 && nb < roomsCount && !visited[nb])
                        q.push_back({nb, hop + 1});
                }
            }
        }

        // ---- extract all exported rooms ----
        std::vector<RoomData> rd;
        rd.reserve(order.size());
        for (int r : order) rd.push_back(extractRoom(r));

        // room 0 (== rd[0]) defines the common origin
        s16 r0X = rd[0].infoX, r0Z = rd[0].infoZ, r0Y = rd[0].yTop;

        // per-room offsets + combined world extent check
        int cwMinX=1<<30, cwMaxX=-(1<<30), cwMinZ=1<<30, cwMaxZ=-(1<<30);
        int cwMinY=1<<30, cwMaxY=-(1<<30);
        long totV=0, totQ=0, totT=0;
        std::vector<int> offX(rd.size()), offZ(rd.size()), yTopD(rd.size());
        for (size_t k = 0; k < rd.size(); k++) {
            offX[k]  = ((int)rd[k].infoX - (int)r0X) * 256;
            offZ[k]  = ((int)rd[k].infoZ - (int)r0Z) * 256;
            yTopD[k] = (int)rd[k].yTop - (int)r0Y;
            totV += rd[k].verts.size();
            totQ += rd[k].quads.size();
            totT += rd[k].tris.size();
            for (auto &v : rd[k].verts) {
                int wx = v.x + offX[k], wz = v.z + offZ[k], wy = v.y + yTopD[k];
                if (wx<cwMinX)cwMinX=wx; if (wx>cwMaxX)cwMaxX=wx;
                if (wz<cwMinZ)cwMinZ=wz; if (wz>cwMaxZ)cwMaxZ=wz;
                if (wy<cwMinY)cwMinY=wy; if (wy>cwMaxY)cwMaxY=wy;
            }
        }
        bool fitsX = (cwMinX>=-32768 && cwMaxX<=32767);
        bool fitsZ = (cwMinZ>=-32768 && cwMaxZ<=32767);
        bool fitsY = (cwMinY>=-32768 && cwMaxY<=32767);
        // all combined coords non-negative? then they fit UNSIGNED 16-bit (0..65535)
        bool fitsU16 = (cwMinX>=0 && cwMaxX<=65535 && cwMinY>=0 && cwMaxY<=65535 &&
                        cwMinZ>=0 && cwMaxZ<=65535);
        // stored per-block fields must fit s16 (offsets + local verts). Check offsets.
        bool offFit = true;
        for (size_t k=0;k<rd.size();k++)
            if (offX[k]<-32768||offX[k]>32767||offZ[k]<-32768||offZ[k]>32767||
                yTopD[k]<-32768||yTopD[k]>32767) offFit=false;

        // ---- Lara start in COMMON coords ----
        // item pos.x/pos.z are room-local, pos.y absolute (out_32X.h:1921-1923).
        s16 lcX=0, lcY=0, lcZ=0; u16 lAng=0; int laraExpIdx=-1;
        if (laraItemOff != 0 && laraRoom >= 0) {
            u32 lri = pRoomsInfo + (u32)laraRoom * ROOMINFO_SIZE;
            s16 lInfoX = sbe16(lri + 0), lInfoZ = sbe16(lri + 2), lYTop = sbe16(lri + 6);
            s16 wx = sbe16(laraItemOff + 2);
            s16 wy = sbe16(laraItemOff + 4);
            s16 wz = sbe16(laraItemOff + 6);
            u16 iflags = be16(laraItemOff + 10);
            lcX = (s16)(wx + ((int)lInfoX - (int)r0X) * 256);
            lcZ = (s16)(wz + ((int)lInfoZ - (int)r0Z) * 256);
            lcY = (s16)((int)wy - (int)r0Y);   // = localY + (lYTop-r0Y)
            (void)lYTop;
            int qd = (iflags >> 14) & 3;
            lAng = (u16)(((qd - 2) * 0x4000) & 0xFFFF);
            for (size_t k = 0; k < order.size(); k++) if (order[k] == laraRoom) laraExpIdx = (int)k;
        }

        // ---- write rooms.bin (BIG-ENDIAN) ----
        std::vector<u8> ro;
        auto rw16 = [&](u16 v){ ro.push_back(v>>8); ro.push_back(v&0xFF); };
        rw16(0x4D52);                 // 'MR'
        rw16((u16)rd.size());
        rw16((u16)lcX); rw16((u16)lcY); rw16((u16)lcZ);
        rw16(lAng);                   // 12-byte header
        std::vector<size_t> blockOff(rd.size());
        for (size_t k = 0; k < rd.size(); k++) {
            blockOff[k] = ro.size();
            rw16((u16)rd[k].verts.size());
            rw16((u16)rd[k].quads.size());
            rw16((u16)rd[k].tris.size());
            rw16((u16)rd[k].xSectors);
            rw16((u16)rd[k].zSectors);
            rw16((u16)(s16)offX[k]);
            rw16((u16)(s16)offZ[k]);
            rw16((u16)(s16)yTopD[k]);
            rw16(0);                  // pad
            for (auto &v : rd[k].verts) { rw16((u16)v.x); rw16((u16)v.y); rw16((u16)v.z); rw16(v.shade); }
            for (auto &q : rd[k].quads) { rw16(q.v[0]); rw16(q.v[1]); rw16(q.v[2]); rw16(q.v[3]); rw16(q.color); rw16(q.flags); }
            for (auto &t : rd[k].tris)  { rw16(t.v[0]); rw16(t.v[1]); rw16(t.v[2]); rw16(t.color); rw16(t.flags); }
            size_t ns = (size_t)rd[k].xSectors * rd[k].zSectors;
            for (size_t s = 0; s < ns; s++) { rw16((u16)rd[k].secFloorY[s]); rw16(rd[k].secWalk[s]); }
            // per-face WORLD normals (s16 x3, /64 scale) for GPU cull-before-
            // transform. Offset is a translation -> cancels in edge diffs, so
            // computed from local verts. Order: all quads, then all tris.
            auto emitN = [&](int i0, int i1, int i2){
                const auto &a = rd[k].verts[i0], &b = rd[k].verts[i1], &c = rd[k].verts[i2];
                double e1x=b.x-a.x, e1y=b.y-a.y, e1z=b.z-a.z;
                double e2x=c.x-a.x, e2y=c.y-a.y, e2z=c.z-a.z;
                double nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
                double L=std::sqrt(nx*nx+ny*ny+nz*nz); if (L<1e-6) L=1.0;
                auto q64=[&](double v){ long r=std::lround(v/L*64.0); if(r>32767)r=32767; if(r<-32768)r=-32768; return (u16)(s16)r; };
                rw16(q64(nx)); rw16(q64(ny)); rw16(q64(nz));
            };
            for (auto &q : rd[k].quads) emitN(q.v[0], q.v[1], q.v[2]);
            for (auto &t : rd[k].tris)  emitN(t.v[0], t.v[1], t.v[2]);
        }

        FILE* rf = fopen(roomsOut, "wb");
        if (!rf) { fprintf(stderr, "cannot write %s\n", roomsOut); }
        else { fwrite(ro.data(), 1, ro.size(), rf); fclose(rf); }

        // ---- portal alignment sanity check ----
        // Reconstruct each portal's absolute-world bounding box from its 4 vertices,
        // then match room 0's portal (0->N) to N's reverse portal (N->0). They must coincide.
        auto portalBox = [&](const RoomData &Rm, const RPortal &p,
                             int &x0,int &x1,int &y0,int &y1,int &z0,int &z1) {
            x0=y0=z0=1<<30; x1=y1=z1=-(1<<30);
            for (int k = 0; k < 4; k++) {
                int ax = p.vx[k] + (int)Rm.infoX * 256;  // absolute world
                int ay = p.vy[k];                         // already absolute
                int az = p.vz[k] + (int)Rm.infoZ * 256;
                if (ax<x0)x0=ax; if (ax>x1)x1=ax;
                if (ay<y0)y0=ay; if (ay>y1)y1=ay;
                if (az<z0)z0=az; if (az>z1)z1=az;
            }
        };

        int checks=0, matches=0, worstDev=0;   // matches = coincide within TOL world units
        const int TOL = 2;                       // allow rounding (source stores 1023 vs 1024)
        std::string firstReport;
        for (auto &p0 : rd[0].portals) {
            int nb = p0.roomIndex;
            // find nb in exported set
            const RoomData* Rn = nullptr;
            for (auto &r : rd) if (r.idx == nb) { Rn = &r; break; }
            if (!Rn) continue;
            // find nb's reverse portal back to room 0
            for (auto &pn : Rn->portals) {
                if (pn.roomIndex != rd[0].idx) continue;
                int a[6], b[6];
                portalBox(rd[0], p0, a[0],a[1],a[2],a[3],a[4],a[5]);
                portalBox(*Rn,   pn, b[0],b[1],b[2],b[3],b[4],b[5]);
                int dev=0; for (int k=0;k<6;k++){ int d=a[k]-b[k]; if(d<0)d=-d; if(d>dev)dev=d; }
                if (dev>worstDev) worstDev=dev;
                bool same = (dev <= TOL);
                checks++; if (same) matches++;
                if (firstReport.empty()) {
                    char buf[512];
                    snprintf(buf,sizeof(buf),
                      "  room0 portal ->room%d  box X[%d..%d] Y[%d..%d] Z[%d..%d]\n"
                      "  room%d portal ->room0  box X[%d..%d] Y[%d..%d] Z[%d..%d]\n"
                      "  => %s (max deviation %d world units)",
                      nb, a[0],a[1],a[2],a[3],a[4],a[5],
                      nb, b[0],b[1],b[2],b[3],b[4],b[5],
                      same?"COINCIDE within rounding":"MISMATCH", dev);
                    firstReport = buf;
                }
                break;
            }
        }

        // ---- report ----
        printf("\n=== MULTI-ROOM (rooms.bin) ===\n");
        printf("BFS from room 0, <=%d hops, cap %d rooms\n", MAX_HOPS, MAX_ROOMS);
        printf("exported rooms  : %zu  indices = [", rd.size());
        for (size_t k=0;k<order.size();k++) printf("%s%d", k?",":"", order[k]);
        printf("]\n");
        printf("totals          : verts=%ld quads=%ld tris=%ld\n", totV, totQ, totT);
        printf("common origin   : room0 originX=%d originZ=%d yTop=%d\n", r0X, r0Z, r0Y);
        printf("per-room offsets (idx: offX offZ yTopDelta  vc/qc/tc  xS*zS):\n");
        for (size_t k=0;k<rd.size();k++)
            printf("  room %2d: off=(%6d,%6d) yTopD=%6d  %u/%u/%u  %dx%d\n",
                   rd[k].idx, offX[k], offZ[k], yTopD[k],
                   (unsigned)rd[k].verts.size(), (unsigned)rd[k].quads.size(),
                   (unsigned)rd[k].tris.size(), rd[k].xSectors, rd[k].zSectors);
        printf("stored offsets  : %s int16 (offX/offZ/yTopDelta all in range)\n", offFit?"FIT":"EXCEED");
        printf("combined world X: [%d .. %d]  %s int16\n", cwMinX, cwMaxX, fitsX?"FITS":"OVERFLOWS");
        printf("combined world Y: [%d .. %d]  %s int16\n", cwMinY, cwMaxY, fitsY?"FITS":"OVERFLOWS");
        printf("combined world Z: [%d .. %d]  %s signed-int16\n", cwMinZ, cwMaxZ, fitsZ?"FITS":"OVERFLOWS");
        if (!fitsZ)
            printf("  NOTE: Z max %d exceeds s16 (32767). All combined coords >=0 -> %s unsigned-u16.\n"
                   "        Even 1-hop neighbor room 6 alone reaches Z=32768, so reducing hops does NOT\n"
                   "        fix signed-16; use u16 world coords OR compute (vertex-camera) in s32\n"
                   "        (fixed/room.h is already camera-relative). Data stored is unaffected.\n",
                   cwMaxZ, fitsU16?"FITS":"EXCEEDS");
        printf("portal align chk: %d/%d coincide within rounding (worst deviation %d world units)\n",
               matches, checks, worstDev);
        if (!firstReport.empty()) printf("%s\n", firstReport.c_str());
        printf("Lara common pos : (%d,%d,%d) angle=0x%04X ; in room %d",
               lcX, lcY, lcZ, lAng, laraRoom);
        if (laraExpIdx>=0) printf(" (exported index %d)\n", laraExpIdx);
        else               printf(" (NOT in exported set)\n");
        printf("header size     : 12 bytes ; room blocks start at offset 12\n");
        printf("per-block hdr   : 18 bytes {vc,qc,tc,xS,zS (5*u16), offX,offZ,yTopDelta (3*s16), pad (s16)}\n");
        printf("block stride    : 18 + vc*8 + qc*12 + tc*10 + xS*zS*4\n");
        for (size_t k=0;k<rd.size();k++) {
            size_t stride = 18 + rd[k].verts.size()*8 + rd[k].quads.size()*12
                          + rd[k].tris.size()*10 + (size_t)rd[k].xSectors*rd[k].zSectors*4;
            printf("  room %2d block @%-7zu size=%zu\n", rd[k].idx, blockOff[k], stride);
        }
        printf("rooms.bin bytes : %zu\n", ro.size());
        printf("output          : %s\n", roomsOut);
    }

    // ========================================================================
    //  LARA MESH -> laramesh.bin  (big-endian, model-local, world-unit scale)
    // ========================================================================
    {
        const char* laraOut = "laramesh.bin";

        int modelsCount = be16(8);
        u32 pMeshData    = be32(52);
        u32 pMeshOffsets = be32(56);
        u32 pAnims       = be32(60);
        u32 pNodes       = be32(76);
        u32 pFrameData   = be32(80);
        u32 pModels      = be32(84);

        // find Lara's Model (type == ITEM_LARA == 0)
        int laraModel = -1;
        for (int i = 0; i < modelsCount; i++) {
            if (gFile[pModels + i*8] == 0) { laraModel = i; break; }
        }
        if (laraModel < 0) {
            fprintf(stderr, "LARA model (type 0) not found; skipping laramesh.bin\n");
        } else {
            u32 mo = pModels + laraModel*8;
            int count     = (s8)gFile[mo + 1];
            int start     = be16(mo + 2);
            int nodeIndex = be16(mo + 4);
            int animIndex = be16(mo + 6);

            // DEBUG: enumerate Lara's animations to find the run cycle.
            // frameCount computed format-agnostically from consecutive frameOffsets.
            printf("=== LARA ANIMS (base animIndex=%d) ===\n", animIndex);
            for (int ai = 0; ai < 24; ai++) {
                u32 aor  = pAnims + (u32)(animIndex+ai)*32;
                u32 aor2 = pAnims + (u32)(animIndex+ai+1)*32;
                u32 fofs  = be32(aor + 0);
                u32 fofs2 = be32(aor2 + 0);
                int frate = gFile[aor+4];
                int fsize = gFile[aor+5];
                int state = be16(aor+6);
                int nframes = (fsize>0 && fofs2>fofs) ? (int)((fofs2-fofs)/((u32)fsize*2)) : -1;
                printf(" anim %2d: state=%2d nframes=%d rate=%d fsize=%d frameOfs=%u\n",
                       animIndex+ai, state, nframes, frate, fsize, fofs);
            }

            u32 ao = pAnims + (u32)animIndex*32;
            u32 frameOffset = be32(ao + 0);
            u32 fb = pFrameData + frameOffset;           // frame base
            int fPosX = sbe16(fb + 12);
            int fPosY = sbe16(fb + 14);
            int fPosZ = sbe16(fb + 16);
            u32 anglesBase = fb + 20;                    // (uint32*)(angles+1)

            // DECODE_ANGLES (big-endian), returns 16-bit angle fields (0x10000=360)
            auto decodeAngles = [](u32 a, int &x, int &y, int &z) {
                x = (int)((a & 0x3FF0) << 2) & 0xFFFF;
                y = (int)(((a & 0x000F) << 12) | (((a >> 16) & 0xFC00) >> 4)) & 0xFFFF;
                z = (int)(((a >> 16) & 0x03FF) << 6) & 0xFFFF;
            };
            // matrixFrame: translateRel(pos) then rotateYXZ(decode(angle))
            auto matFrame = [&](Mat &m, int px,int py,int pz, u32 angleWord) {
                matTranslateRel(m, px, py, pz);
                int aX,aY,aZ; decodeAngles(angleWord, aX,aY,aZ);
                matRotYXZ(m, aX, aY, aZ);
            };

            // output accumulators
            struct LVert { s16 x,y,z; };
            struct LQuad { u16 v[4], color, flags; };
            struct LTri  { u16 v[3], color, flags; };
            std::vector<LVert> lverts;
            std::vector<LQuad> lquads;
            std::vector<LTri>  ltris;

            // colored-face color: low 8 bits index the palette directly
            auto colorFaceColor = [&](int idx) -> u16 {
                int r5,g5,b5; palRGB(idx & 0xFF, r5,g5,b5);
                return jagRGB16(r5,g5,b5);
            };

            // emit one mesh's verts (+faces only when captureFaces) baked through M
            auto emitMesh = [&](const Mat &M, int meshIndex, bool captureFaces) {
                u32 base = pMeshData + (u32)be32(pMeshOffsets + (u32)meshIndex*4);
                int vCount = gFile[base + 10];
                int qCount = be16(base + 12);
                int tCount = be16(base + 14);
                u32 vo = base + 20;
                u32 qo = vo + (u32)vCount*6;
                u32 to = qo + (u32)qCount*6;

                int vbase = (int)lverts.size();
                for (int j = 0; j < vCount; j++) {
                    double lx = sbe16(vo + j*6 + 0) / 4.0; // onDisk(<<2) -> world units
                    double ly = sbe16(vo + j*6 + 2) / 4.0;
                    double lz = sbe16(vo + j*6 + 4) / 4.0;
                    double wx = M.t[0] + M.r[0][0]*lx + M.r[0][1]*ly + M.r[0][2]*lz;
                    double wy = M.t[1] + M.r[1][0]*lx + M.r[1][1]*ly + M.r[1][2]*lz;
                    double wz = M.t[2] + M.r[2][0]*lx + M.r[2][1]*ly + M.r[2][2]*lz;
                    LVert v;
                    v.x = (s16)lround(wx);
                    v.y = (s16)lround(wy);
                    v.z = (s16)lround(wz);
                    lverts.push_back(v);
                }
                if (!captureFaces) return;   // faces identical across frames
                for (int j = 0; j < qCount; j++) {
                    u32 fo = qo + j*6;
                    u16 flags = be16(fo);
                    int type   = (flags >> 14) & 3;   // 0=F(colored),1=FT,2=FTA
                    int tex    = flags & 0x3FFF;
                    LQuad q;
                    for (int k=0;k<4;k++) q.v[k] = (u16)(vbase + gFile[fo + 2 + k]);
                    q.color = (type == 0) ? colorFaceColor(tex) : faceColor(tex, 255);
                    q.flags = flags;
                    lquads.push_back(q);
                }
                for (int j = 0; j < tCount; j++) {
                    u32 fo = to + j*6;
                    u16 flags = be16(fo);
                    int type   = (flags >> 14) & 3;
                    int tex    = flags & 0x3FFF;
                    LTri t;
                    for (int k=0;k<3;k++) t.v[k] = (u16)(vbase + gFile[fo + 2 + k]);
                    t.color = (type == 0) ? colorFaceColor(tex) : faceColor(tex, 255);
                    t.flags = flags;
                    ltris.push_back(t);
                }
            };

            // ---- bake ALL frames of the RUN animation (anim 0, state=1) ----
            // Frame layout: bbox(12) + pos(6) + pad(2) + count*4 angle bytes.
            int frameSize = 20 + count*4;
            u32 nextFofs  = be32(pAnims + (u32)(animIndex+1)*32 + 0);
            int nframes   = (nextFofs > frameOffset)
                            ? (int)((nextFofs - frameOffset) / (u32)frameSize) : 1;
            if (nframes < 1) nframes = 1;
            if (nframes > 32) nframes = 32;
            // Shared root translation (frame 0's) so the cycle runs IN PLACE -
            // Lara's world position is driven by the engine, not the anim.
            int rootX = fPosX, rootY = fPosY, rootZ = fPosZ;
            const int LARA_N = 4;                 // shade swatch cells (matches atlas)
            std::vector<std::vector<LVert>> frameVerts(nframes);
            std::vector<std::vector<u8>>    frameShades(nframes);  // per-face shade/frame
            for (int f = 0; f < nframes; f++) {
                u32 ffb     = pFrameData + frameOffset + (u32)f*frameSize;
                u32 fAngles = ffb + 20;
                bool cap = (f == 0);            // capture faces once
                lverts.clear();
                Mat M = matIdentity();
                std::vector<Mat> stack;
                matFrame(M, rootX, rootY, rootZ, be32(fAngles + 0));
                emitMesh(M, start + 0, cap);
                for (int i = 1; i < count; i++) {
                    u32 nd = pNodes + (u32)(nodeIndex + (i-1))*8;
                    int nx = sbe16(nd + 0), ny = sbe16(nd + 2), nz = sbe16(nd + 4);
                    int nflags = be16(nd + 6);
                    if (nflags & 1) { M = stack.back(); stack.pop_back(); } // POP
                    if (nflags & 2) { stack.push_back(M); }                 // PUSH
                    matFrame(M, nx, ny, nz, be32(fAngles + (u32)i*4));
                    emitMesh(M, start + i, cap);
                }
                frameVerts[f] = lverts;
                // per-face flat shade for THIS frame: Lara-local face normal . light.
                // Light from upper-front-left (+Y is DOWN, so up = -Y). Baked per
                // frame so limbs shade correctly as they move; zero runtime cost.
                {
                    const double Lx=-0.40, Ly=-0.80, Lz=-0.45;
                    auto shadeOf = [&](int i0,int i1,int i2)->u8 {
                        LVert &a=lverts[i0], &b=lverts[i1], &c=lverts[i2];
                        double e1x=b.x-a.x,e1y=b.y-a.y,e1z=b.z-a.z;
                        double e2x=c.x-a.x,e2y=c.y-a.y,e2z=c.z-a.z;
                        double nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
                        double len=sqrt(nx*nx+ny*ny+nz*nz); if(len<1e-6)len=1;
                        double d=(nx*Lx+ny*Ly+nz*Lz)/len;
                        double bb=0.30 + 0.70*(d>0?d:0.0);   // ambient 0.3
                        int s=(int)(bb*LARA_N); if(s<0)s=0; if(s>=LARA_N)s=LARA_N-1;
                        return (u8)s;
                    };
                    frameShades[f].clear();
                    for (auto &q : lquads) frameShades[f].push_back(shadeOf(q.v[0],q.v[1],q.v[2]));
                    for (auto &t : ltris)  frameShades[f].push_back(shadeOf(t.v[0],t.v[1],t.v[2]));
                }
            }
            // lverts holds the LAST frame; restore frame 0 for the bbox/report
            lverts = frameVerts[0];

            // bounding box
            int lminX=1<<30,lminY=1<<30,lminZ=1<<30,lmaxX=-(1<<30),lmaxY=-(1<<30),lmaxZ=-(1<<30);
            for (auto &v : lverts) {
                if (v.x<lminX)lminX=v.x; if (v.x>lmaxX)lmaxX=v.x;
                if (v.y<lminY)lminY=v.y; if (v.y>lmaxY)lmaxY=v.y;
                if (v.z<lminZ)lminZ=v.z; if (v.z>lmaxZ)lmaxZ=v.z;
            }

            // ---- write laramesh.bin (BIG-ENDIAN) ----
            // Multi-frame format (BIG-ENDIAN):
            //   HEADER 12B: u16 magic(0x4C41), vcount, qcount, tcount, framecount, pad
            //   FRAMES  : framecount * (vcount * 6B  s16 x,y,z)   [run-cycle poses]
            //   QUADS   : qcount * 12B (u16 v0..v3, color, flags) [shared topology]
            //   TRIS    : tcount * 10B (u16 v0,v1,v2, color, flags)
            std::vector<u8> lo;
            auto lw16 = [&](u16 v){ lo.push_back(v>>8); lo.push_back(v&0xFF); };
            u16 vcount = (u16)frameVerts[0].size();
            lw16(0x4C41);                      // 'LA'
            lw16(vcount);
            lw16((u16)lquads.size());
            lw16((u16)ltris.size());
            lw16((u16)nframes);
            lw16(0);                           // pad -> 12-byte header
            for (int f = 0; f < nframes; f++)
                for (auto &v : frameVerts[f]) { lw16((u16)v.x); lw16((u16)v.y); lw16((u16)v.z); }
            for (auto &q : lquads) { lw16(q.v[0]); lw16(q.v[1]); lw16(q.v[2]); lw16(q.v[3]); lw16(q.color); lw16(q.flags); }
            for (auto &t : ltris)  { lw16(t.v[0]); lw16(t.v[1]); lw16(t.v[2]); lw16(t.color); lw16(t.flags); }
            // SHADES: framecount * (qcount+tcount) bytes (per-face shade 0..LARA_N-1)
            for (int f = 0; f < nframes; f++)
                for (u8 s : frameShades[f]) lo.push_back(s);
            printf("RUN ANIM baked  : %d frames x %d verts (frameSize=%d bytes), %d shades/frame\n",
                   nframes, vcount, frameSize, (int)(lquads.size()+ltris.size()));

            FILE* lf = fopen(laraOut, "wb");
            if (!lf) { fprintf(stderr, "cannot write %s\n", laraOut); }
            else { fwrite(lo.data(), 1, lo.size(), lf); fclose(lf); }

            printf("\n=== LARA MESH ===\n");
            printf("lara model idx  : %d  (type ITEM_LARA=0)\n", laraModel);
            printf("mesh count      : %d  (start=%d nodeIndex=%d animIndex=%d)\n", count, start, nodeIndex, animIndex);
            printf("frameOffset     : %u  frame@%u  framePos=(%d,%d,%d)\n", frameOffset, fb, fPosX,fPosY,fPosZ);
            printf("pose            : BAKED per-node ROTATIONS from first frame of first anim (full pose)\n");
            printf("combined        : vertexCount=%zu quadCount=%zu triCount=%zu\n",
                   lverts.size(), lquads.size(), ltris.size());
            printf("coord scale     : mesh-vert world units = onDiskMeshVert>>2 ; 1 Lara unit == 1 room world unit == 1 TR unit (1024/sector)\n");
            printf("axis convention : +Y DOWN (head=minY negative, feet=maxY), same as room verts\n");
            printf("bbox X (units)  : [%d .. %d]  (width  %d)\n", lminX, lmaxX, lmaxX-lminX);
            printf("bbox Y (units)  : [%d .. %d]  (height %d)\n", lminY, lmaxY, lmaxY-lminY);
            printf("bbox Z (units)  : [%d .. %d]  (depth  %d)\n", lminZ, lmaxZ, lmaxZ-lminZ);
            printf("frame stored box: X[%d..%d] Y[%d..%d] Z[%d..%d]  (sanity: should match bbox)\n",
                   sbe16(fb+0),sbe16(fb+2),sbe16(fb+4),sbe16(fb+6),sbe16(fb+8),sbe16(fb+10));
            printf("laramesh bytes  : %zu\n", lo.size());
            printf("output          : %s\n", laraOut);
            printf("\nfirst 64 bytes of laramesh.bin:\n");
            for (int i = 0; i < 64 && i < (int)lo.size(); i++) {
                printf("%02X ", lo[i]);
                if ((i & 15) == 15) printf("\n");
            }
            printf("\n");
        }
    }

    return 0;
}
