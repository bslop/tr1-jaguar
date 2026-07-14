# tr2jag_tex — PSX TR1 texture extractor (M3 "textures")

Extracts the textures from a PlayStation Tomb Raider 1 level (`.PSX`) and emits an
**8-bit indexed (paletted)** atlas the Atari Jaguar renderer can use, plus a global
256-colour palette in the renderer's RGB16 format and a per-object-texture UV table.

## Run

```sh
cd OpenLara-master/src/platform/jaguar/tools
python3 tr2jag_tex.py
```

No arguments; paths are hard-coded at the top of the script:
- input  : `tr1_psx/extracted/PSXDATA/LEVEL1.PSX` (Caves, 1448896 bytes, VER_TR1_PSX)
- outputs : `src/platform/jaguar/{textures.bin, texpal.bin, texmap.bin, textures.h}`
- previews: `tr1_psx/tex_preview/*.png`

Pure Python 3 (only stdlib: `struct`, `zlib`). No build step.

## What it produces (all big-endian)

- **textures.bin** — 8bpp indexed atlas, 1 byte/texel (palette index), row-major,
  `TEX_ATLAS_W`×`TEX_ATLAS_H` = 256×13178 for LEVEL1. Tiles are the unique
  object-texture rectangles, shelf-packed at width 256.
- **texpal.bin** — 256 × u16 RGB16, big-endian. RGB16 = `R5<<11 | B5<<6 | G6`
  (blue in the middle). The Jaguar reads an 8-bit texel → looks up here → framebuffer colour.
- **texmap.bin** — table. Header `u16 objCount, atlasW, atlasH`; then one 22-byte
  record per object texture (indexed by a face's texture id):
  `u16 atlasX, atlasY, w, h; u8 u0,v0,u1,v1,u2,v2,u3,v3` (tile-local corner UVs);
  `u16 origTile, origClut, attribute`.
- **textures.h** — `#define`s for the dims/counts + the format doc above.

## Previews (verification)

- `tex_preview/page_00.png … page_08.png` — each source 256×256 page painted from
  its object textures using **each face's own CLUT** (direct RGB, no quantization).
  These verify the CLUT decode and the RGB bit order independent of the palette.
- `tex_preview/atlas_indexed.png` — the packed atlas decoded index→global-palette→RGB.
  Verifies the 8bpp indexing + the 256-colour palette.

Both should show real Caves rock/stone walls (grey-brown) and object/item art
(red health cross, gold bars) with natural colour — not R/B-swapped garbage.

## Key format facts (proven against OpenLara src/)

- PSX read chain replicated: `loadTR1_PSX` (format.h:3349) → offsetTexTiles @ file+16,
  tiles start at `offsetTexTiles+8`: 13 × `Tile4` (32768B each) then 1024 × `CLUT`
  (32B each). Then `readDataArrays` (rooms/floors/meshData/anims/…/staticMeshes) is
  skipped record-by-record to reach `readObjectTex` (1102 records, 16B each,
  format.h:6127).
- `Tile4` (utils.h:1523): 256×256 4-bit indices, `ColorIndex4{a:4,b:4}`.
  Texel(x,y): byte = index[(y*256+x)/2]; nibble = (x&1) ? high : low.
- `CLUT`/`ColorCLUT` (utils.h:1508): 16 × u16 LE, `{r:5,g:5,b:5,a:1}`
  → R=bits0-4, G=5-9, B=10-14, STP=bit15. (R is in the LOW bits.)
- Colour → Jaguar RGB16: `(r5<<11) | (b5<<6) | (g5<<1)` (G6 low bit = 0).
