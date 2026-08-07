/*
 * vidmain.c - STANDALONE BOOT-TO-VIDEO ROM  (`make vidrom`)
 *
 * User order (2026-08-07): "the video MUST run off Tom; the 68000 should only
 * be doing boot code" - and then (2026-08-08): "take the Eidos video from the
 * disc, have a build that is just that booting until we get it down and
 * working properly, then move on to Core, attract and caves intro videos,
 * each with their own boot to video for testing."
 *
 * WHY A SEPARATE MAIN AT ALL
 * The Tom jvdec kernel runs perfectly at GAME ENTRY (probe J11V0D1) and has
 * never once started from the BOOT video block - through fourteen exonerated
 * theories (kernel bytes, call sites, DRAM vs SRAM handshake, DSP quiesce,
 * disp240, load retries, load stop-settle, kick stop-settle, ROM-source
 * reads). Every one of those cost a full-game build plus an A10 lottery roll
 * plus a ~3 minute boot chain. This image links NO level data and NO title,
 * so it builds in seconds, boots in ~5, and the whole context is 300 lines
 * that I can add ingredients to one at a time until the failure appears.
 *
 * It is also the deliverable: when the clip plays here on Tom, this is the
 * per-clip test ROM the user asked for.
 *
 * CONTEXT SWITCHES (default = the barest context that can play a clip)
 *   VR_GPUINIT   call gpu_init() first (loads the geotex kernel + span self
 *                test) - i.e. exactly what boot does before the video block
 *   VR_JERRY     bring the DSP up (jerry_init)
 *   VR_AUDIO     queue the clip's XA audio (implies VR_JERRY)
 *   VR_EARLYLOAD load the jvdec kernel at init instead of at clip open
 *   VR_NO68K     never fall back to the 68k token walk - a failed Tom decode
 *                stays visible instead of being papered over
 *   VR_CLIP      "EIDOS.JV" (default), "CORE.JV", "INTRO.JV", "CAVES.JV"
 *
 * The read-out is drawn by hand into the top-left of every frame (and onto a
 * hold screen before the clip opens, so a dead GD still yields data):
 *   row 0  heartbeat        row 4  hello magic seen
 *   row 1  gpu_ok           row 5  Tom said DONE this frame
 *   row 2  jvdec load ok    row 6  GPU PC inside F03000..F04000
 *   row 3  SRAM verify == 0 row 7  self-test (zero-length kick) passed
 * plus 32-bit binary rows for the verify mismatch count and the last GPU PC.
 */

#include "jaguar.h"
#include "video.h"
#include "gpu.h"
#include "gdbios.h"
#include "blit.h"

#ifndef VR_CLIP
#define VR_CLIP "EIDOS.JV"
#endif

#if defined(VR_AUDIO) && !defined(VR_JERRY)
#define VR_JERRY
#endif

/* VR_PAD=N: the A10 layout roll, PADTEXT's idea for this image. Small ROMs
 * are NOT immune (measured 2026-08-08: two builds 16 bytes apart, one booted
 * and one wedged the OP so hard that a 440k-iteration counting loop had not
 * finished after 60 seconds). vidmain.o links before video.o, so padding here
 * shifts the A10-critical ISR/OP code exactly the way PADTEXT does in the
 * game. Build the roll set, keep the lit ones. */
#ifdef VR_PAD
__attribute__((used, section(".text")))
static const uint8_t g_vrpad[VR_PAD] = { 1 };
#endif

int  gd_input_init(void);
int  jerry_init(void);
void jerry_sfx(int voice, const void *pcm, uint32_t bytes, int loop);
void jerry_sfx_queue(const void *pcm, uint32_t bytes);
void video_set_disp240(int on);
void video_pin_start(void);
uint32_t joypad_read(void);
uint32_t gpu_jvdec_hello(void);
uint32_t gpu_jvdec_verify(void);
uint32_t gpu_pc_read(void);

/* ---- buffers ----------------------------------------------------------
 * Nothing else lives in this image, so these are plain BSS (in the game they
 * have to borrow rblob - "BSS is nearly full, never add static buffers"). */
#define VB_SIZE   31488u                  /* rolling stream buffer          */
#define STASH     7552u                   /* ~1.5x the worst measured frame */
static uint8_t  vb[VB_SIZE] __attribute__((aligned(16)));
static uint8_t  cbk[4096]   __attribute__((aligned(16)));
static uint8_t  stashA[STASH] __attribute__((aligned(16)));
static uint8_t  stashB[STASH] __attribute__((aligned(16)));
static uint8_t  vshadow[320*240] __attribute__((aligned(16)));
#ifdef VR_AUDIO
static int8_t   aring[6*4096] __attribute__((aligned(16)));
#endif

/* BG CRUMBS. BGEN is on, so the background colour shows through wherever no
 * object paints - i.e. exactly in the failure state this ROM exists to
 * diagnose. A black capture then still says WHERE it stopped, and a capture
 * that shows the picture instead says the object path is alive.
 * Jaguar RGB16 = R<<11 | B<<6 | G<<1. */
#define BGC(c) (*(volatile uint16_t *)0xF00058u = (uint16_t)(c))
#define BG_BLUE   0x07C0
#define BG_GREEN  0x003E
#define BG_RED    0xF800
#define BG_YELLOW 0xF83E
#define BG_MAG    0xF7C0
#define BG_CYAN   0x07FE

/* Hold a crumb colour on screen long enough to be captured, WITHOUT using
 * frame_count - the whole point of the first crumbs is to find out whether
 * the vblank ISR is alive at all. ~440k iterations/s on a 13.3MHz 68000. */
__attribute__((unused))
static void dwell(uint16_t c, uint32_t n)
{
    volatile uint32_t i;
    BGC(c);
    for (i = 0; i < n; i++)
        ;
}
#define DWELL_1S 440000u

/* Same loop, but the background steps through a red ramp as it counts. A
 * capture then reads PROGRESS, not just presence: a varying colour means the
 * 68k is executing and merely slow (bus starvation), a frozen one means it is
 * genuinely stuck - and those two have completely different fixes. */
__attribute__((unused))
static void ramp(uint32_t n)
{
    volatile uint32_t i;
    for (i = 0; i < n; i++)
        if ((i & 0x1FFFu) == 0)
            BGC((uint16_t)((((i >> 13) & 31u) << 11) | 0x0040u));
}

/* ---- diagnostic state -------------------------------------------------- */
static int      d_gpu_ok, d_loaded, d_selftest;
/* how far the clip open got: 1 fopen, 2 size/sector check, 3 header read,
   4 JV04 magic, 5 codebook read, 6 streaming. A boot that shows the hold
   screen forever now says WHICH step refused. */
static uint32_t d_stage;
static uint32_t d_fields;   /* fields elapsed in the clip: fps = frames*60/d_fields */
/* PER-PHASE PROFILE, in VC half-line ticks (~31.7us each, 525 per field).
   frame_count alone is far too coarse to attribute a 250ms frame, and
   guessing between "the GameDrive is slow" and "the 68k block copy is slow"
   would cost a silicon roll per theory. Accumulated over the whole clip and
   painted as bit rows, so one still frame carries the whole breakdown. */
static uint32_t p_read, p_tom, p_copy, p_pace, p_paint;
static uint32_t d_blitfail;   /* phrase-mode copies that had to fall back */
static uint32_t d_phrase;     /* phrase-mode Blitter copy verified byte-exact */

/* Monotonic half-line clock. VC wraps every field, so fold in frame_count -
   which the vblank ISR bumps once per field - to get a running tick. */
static uint32_t vtick(void)
{
    extern volatile uint32_t frame_count;
    uint32_t f = frame_count;
    uint32_t v = VC;
    if (frame_count != f) { f = frame_count; v = VC; }
    return f * 525u + (v & 0x3FFu);
}
static uint32_t d_verify = 0xFFFFFFFFu, d_pc, d_hello, d_tomok, d_frames,
                d_tomfail;

/* Palette for the pre-clip hold screen and for the marker colours. The clip
 * brings its own CLUT; entries 254/255 are re-forced to black/white after it
 * loads (the VIDCAD trick) so the markers stay readable over any frame. */
#define JAG16(r,g,b) (uint16_t)(((r)<<11)|((b)<<6)|((g)<<1))
#define MK_OFF 254
#define MK_ON  255
static uint16_t holdpal[256];

static void pal_init(void)
{
    int i;
    for (i = 0; i < 256; i++) holdpal[i] = 0;
    holdpal[MK_OFF] = JAG16(0,0,0);
    holdpal[MK_ON]  = JAG16(31,63,31);
    holdpal[1]      = JAG16(20,0,0);      /* dim red backdrop for the hold  */
}

static void clut_markers(void)
{
    volatile uint16_t *cl = (volatile uint16_t *)0xF00400u;
    cl[MK_OFF] = JAG16(0,0,0);
    cl[MK_ON]  = JAG16(31,63,31);
}

/* ---- the read-out panel ------------------------------------------------
 * A SELF-CALIBRATING panel, because eyeballing scaled captures does not
 * scale: the first decoder inferred the framebuffer grid from the picture
 * edges and drifted by a row or two near the bottom, quietly turning real
 * profile numbers into zeros. So the panel draws its own reference frame -
 * a solid black field with a 2px white border at a FIXED framebuffer rect -
 * and the host decoder recovers the exact grid from that border instead of
 * guessing. Everything inside is then addressed in framebuffer pixels.
 *
 *   lamps    8 x (14x10) at x=4,  y=4 + i*12
 *   bit rows 12 x 32 bits at x=24, y=102 + i*11, 4px per bit, 8px tall
 */
#define PAN_W 160
/* ☠️ PAN_H must cover the LAST bit row: with BITY0+10*BITDY > PAN_H the
   bottom rows are drawn over the video, outside the border the host decoder
   calibrates from, and they read back as garbage - p_tom/p_copy came back as
   "0ms to copy 76800 bytes" until this was caught. 102 + 12*11 + 2 = 236, rounded to 234 with the last row ending at 231. */
#define PAN_H 234
#define BITX  24
#define BITW  4
#define BITY0 102
#define BITDY 11
#define BITH  8

/* ☠️ LONG stores. The first version filled 36160 bytes one at a time and
   cost more per frame than Tom's entire decode - an instrument that changes
   what it measures. Byte stores are the 68k's worst case under OP
   contention; this is the same fill at a quarter of the accesses. */
/* ☠️ THE INSTRUMENT MUST NOT DOMINATE THE MEASUREMENT. The first panel
   cleared 160x234 and drew every cell as byte stores: 62 ms/frame on
   silicon, second only to Tom's whole decode and a third of the frame.
   So: no background fill at all (every cell paints BOTH states, so nothing
   stale survives), and every write is a LONG - the cells are 4 px wide at
   4-aligned x precisely so one store covers one row of one cell. ~10 ms. */
#define L_ON  0xFFFFFFFFu
#define L_OFF ((uint32_t)MK_OFF * 0x01010101u)

static void panel_border(uint8_t *fb)
{
    int y, x;
    for (y = 0; y < PAN_H; y++) {
        uint32_t *r = (uint32_t *)(fb + y * 320);
        if (y < 2 || y >= PAN_H - 2) {
            for (x = 0; x < PAN_W / 4; x++)
                r[x] = L_ON;
        } else {
            r[0] = L_ON;                    /* 4px left rail  */
            r[PAN_W / 4 - 1] = L_ON;        /* 4px right rail */
        }
    }
}

static void lamp(uint8_t *fb, int row, int on)
{
    int y, x, y0 = 4 + row * 12;
    uint32_t v = on ? L_ON : L_OFF;
    for (y = y0; y < y0 + 10; y++) {
        uint32_t *r = (uint32_t *)(fb + y * 320 + 4);
        for (x = 0; x < 4; x++)             /* 16 px wide, 4-aligned */
            r[x] = v;
    }
}

static void bits32(uint8_t *fb, int row, uint32_t v)
{
    int b, y, y0 = BITY0 + row * BITDY;
    for (y = y0; y < y0 + BITH; y++) {
        uint32_t *r = (uint32_t *)(fb + y * 320 + BITX);
        for (b = 0; b < 32; b++)
            r[b] = ((v >> (31 - b)) & 1) ? L_ON : L_OFF;
    }
}

static void paint_markers(uint8_t *fb)
{
    panel_border(fb);
    lamp(fb, 0, (int)(d_frames & 1));
    lamp(fb, 1, d_gpu_ok);
    lamp(fb, 2, d_loaded);
    lamp(fb, 3, d_verify == 0);
    lamp(fb, 4, d_hello == 0x0A3D0001u);
    lamp(fb, 5, (int)d_tomok);
    lamp(fb, 6, d_pc >= 0xF03000u && d_pc < 0xF04000u);
    lamp(fb, 7, d_selftest);
    bits32(fb, 0, 0xA5A5A5A5u);      /* calibration: fixed alternating pattern */
    bits32(fb, 1, d_frames);
    bits32(fb, 2, d_fields);
    bits32(fb, 3, p_read);
    bits32(fb, 4, p_tom);
    bits32(fb, 5, p_copy);
    bits32(fb, 6, p_pace);
    bits32(fb, 7, p_paint);
    bits32(fb, 8, d_pc);
    bits32(fb, 9, d_tomfail);
    bits32(fb, 10, d_stage | (d_phrase << 8) | (d_blitfail << 16));
    bits32(fb, 11, d_verify);
}

static void hold_screen(int fields)
{
    uint32_t t0;
    extern volatile uint32_t frame_count;
    int n;
    for (n = 0; n < 4; n++) {
        uint8_t *bb = (uint8_t *)video_backbuffer();
        uint32_t *w = (uint32_t *)bb;
        uint32_t k;
        /* ☠️ Fill with MK_OFF, not palette index 1. After a clip the CLUT is
           the CLIP'S - index 1 is one of its colours and came back BRIGHT,
           so the whole panel read as set bits and the decoder's calibration
           word failed. 254/255 are the only two entries we own, and
           clut_markers re-forces them whatever palette is loaded. */
        clut_markers();
        for (k = 0; k < (320u*240u)/4u; k++) w[k] = (uint32_t)MK_OFF * 0x01010101u;
        paint_markers(bb);
        video_flip();
        t0 = frame_count;
        while ((int)(frame_count - t0) < fields / 4)
            ;
    }
}

/* ---- the jvdec kernel: load, verify, and prove it starts ---------------- */
static void jvdec_load_verified(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        gpu_jvdec_load();
        d_verify = gpu_jvdec_verify();
        if (d_verify == 0) break;
    }
    d_loaded = (d_verify == 0);
}

/* ZERO-LENGTH KICK: the real kernel with no data. It loads its params, stores
 * the hello magic, then both passes hit `cmpq #0,r5` on an empty stream and
 * fall straight through to the done store. Nothing about the token walk, the
 * codebook or the framebuffer is involved - so a failure here is purely
 * "Tom did not start", with none of the streaming machinery in the picture. */
static int jvdec_selftest(void)
{
    int ok;
    gpu_jvdec_kick(vb, 0, vb, 0, cbk, vshadow);
    ok = gpu_jvdec_wait();
    d_hello = gpu_jvdec_hello();
    d_pc = gpu_pc_read();
    return ok;
}

/* PHRASE-MODE SELF-TEST. A mis-set Blitter does not politely fail - it can
   run away over DRAM, and the first phrase-copy build came back black with
   no way to tell that from an A10 miss. So prove the mode on EIGHT ROWS into
   a scratch buffer, byte-for-byte on the 68k, BEFORE letting it near the
   framebuffer. Contained: at most 2560 bytes can be written wrongly. */
static int phrase_selftest(void)
{
    uint32_t i;
    uint8_t *src = vshadow, *dst = cbk;
    for (i = 0; i < 320u*8u; i++) src[i] = (uint8_t)(i * 7u + (i >> 5));
    for (i = 0; i < 320u*8u; i++) dst[i] = 0;
    if (!blit_copy_phrase(src, dst, 8))
        return 0;
    for (i = 0; i < 320u*8u; i++)
        if (dst[i] != src[i])
            return 0;
    return 1;
}

/* ---- the clip ---------------------------------------------------------- */
static int play_clip(const char *name)
{
    extern volatile uint32_t frame_count;
    extern volatile uint32_t pending_fb;
    int vh = -1, mi, vw, vhh, vfps, vnf, fi, remain, have, pos;
    uint32_t t0;
    uint8_t *pcur = stashA, *pprev = stashB;
#ifdef VR_AUDIO
    int acc = 0, wslot = 0, rslot = 0, pend = 0, astarted = 0, alen[6];
#endif

    d_stage = 1;
    for (mi = 0; mi < 2 && vh < 0; mi++) {
        char pathbuf[24];
        const char *p = name;
        int k = 0;
        if (mi) pathbuf[k++] = '/';
        while (*p && k < 23) pathbuf[k++] = *p++;
        pathbuf[k] = 0;
        vh = gd_fopen(pathbuf, GD_FOPEN_READ | GD_FOPEN_OPEN_EXISTING);
    }
    if (vh < 0) return 0;

    d_stage = 2;
    remain = gd_fsize((unsigned)vh);
    if (remain < 1024 + 4096 || (remain & 511)) { gd_fclose((unsigned)vh); return 0; }
    d_stage = 3;
    if (gd_fread((unsigned)vh, vb, 1024, GD_FREAD_CPU) != 0) {
        gd_fclose((unsigned)vh); return 0;
    }
    d_stage = 4;
    if (vb[0] != 'J' || vb[1] != 'V' || vb[3] != '4') {
        gd_fclose((unsigned)vh); return 0;
    }
    remain -= 1024;
    vw   = (vb[4] << 8) | vb[5];
    vhh  = (vb[6] << 8) | vb[7];
    vfps = (vb[8] << 8) | vb[9];
    vnf  = (vb[10] << 8) | vb[11];
    if (vw != 320 || vhh != 240 || vfps <= 0) { gd_fclose((unsigned)vh); return 0; }
    video_set_clut((const uint16_t *)(vb + 16));
    clut_markers();
    d_stage = 5;
    if (gd_fread((unsigned)vh, cbk, 4096, GD_FREAD_CPU) != 0) {
        gd_fclose((unsigned)vh);
        return 0;
    }
    remain -= 4096;
    d_stage = 6;

    video_pin_start();
    { uint32_t *fw = (uint32_t *)vshadow, k;
      for (k = 0; k < (320u*240u)/4u; k++) fw[k] = 0; }

    have = 0; pos = 0;
    t0 = frame_count;
    for (fi = 0; fi < vnf; fi++) {
        uint32_t L, AL;
        int need = 8;
        /* --- pull one record into the rolling buffer (sector discipline: 512
           granular reads at 512 positions, 4-aligned destinations) --------- */
        for (;;) {
            if (have - pos >= need) {
                if (need > 8) break;
                L  = ((uint32_t)vb[pos] << 24) | ((uint32_t)vb[pos+1] << 16)
                   | ((uint32_t)vb[pos+2] << 8) | vb[pos+3];
                AL = ((uint32_t)vb[pos+4] << 24) | ((uint32_t)vb[pos+5] << 16)
                   | ((uint32_t)vb[pos+6] << 8) | vb[pos+7];
                if (L == 0 || L > 24576u || AL > 4096u || (AL & 3)) { fi = vnf; break; }
                need = 8 + (int)AL + (int)((L + 3u) & ~3u);
                if (have - pos >= need) break;
            }
            if (pos) {
                /* records are 4-padded, so pos stays 4-aligned and this can
                   move longs; the byte version walked up to 26KB one at a
                   time, every time the buffer compacted */
                int mv = have - pos, k, nl = mv >> 2;
                uint32_t *d4 = (uint32_t *)vb;
                const uint32_t *s4 = (const uint32_t *)(vb + pos);
                for (k = 0; k < nl; k++) d4[k] = s4[k];
                for (k = nl << 2; k < mv; k++) vb[k] = vb[pos + k];
                have = mv; pos = 0;
            }
            { int want = (VB_SIZE - have) & ~511;
              if (want > 24576) want = 24576;
              if (want > remain) want = remain;
              if (want <= 0) { fi = vnf; break; }
              { uint32_t ta = vtick();
                int rr = gd_fread((unsigned)vh, vb + have, (unsigned)want,
                                  GD_FREAD_CPU);
                p_read += vtick() - ta;
                if (rr != 0) { fi = vnf; break; } }
              have += want; remain -= want; }
        }
        if (fi >= vnf) break;
        pos += 8;

#ifdef VR_AUDIO
        if (AL) {
            volatile uint32_t *ncnt = (volatile uint32_t *)0xF1C378u;
            volatile uint32_t *vcnt = (volatile uint32_t *)0xF1C340u;
            if (acc + (int)AL > 4096) {
                if (pend >= 5) { rslot = (rslot + 1) % 6; pend--; }
                alen[wslot] = acc; pend++;
                wslot = (wslot + 1) % 6; acc = 0;
            }
            { const uint32_t *as = (const uint32_t *)(vb + pos);
              uint32_t *ad = (uint32_t *)(aring + wslot*4096 + acc);
              uint32_t k;
              for (k = 0; k < AL >> 2; k++) ad[k] = as[k];
              acc += (int)AL; }
            if (!astarted) {
                if (pend >= 2) {
                    jerry_sfx(0, aring + rslot*4096, (uint32_t)alen[rslot], 0);
                    rslot = (rslot + 1) % 6; pend--;
                    jerry_sfx_queue(aring + rslot*4096, (uint32_t)alen[rslot]);
                    rslot = (rslot + 1) % 6; pend--;
                    astarted = 1;
                }
            } else if (pend > 0 && *ncnt == 0) {
                if (*vcnt == 0)
                    jerry_sfx(0, aring + rslot*4096, (uint32_t)alen[rslot], 0);
                else
                    jerry_sfx_queue(aring + rslot*4096, (uint32_t)alen[rslot]);
                rslot = (rslot + 1) % 6; pend--;
            }
        }
        pos += (int)AL;
#else
        pos += (int)AL;                    /* audio present but not played */
#endif

        /* --- decode ------------------------------------------------------ */
        { uint8_t *tk = vb + pos;
          uint8_t *bb;
          pos += (int)((L + 3u) & ~3u);
          while (pending_fb)
              ;
          bb = (uint8_t *)video_backbuffer();
          d_tomok = 0;
          if (d_loaded) {
              uint32_t k, nw = ((L + 3u) & ~3u) >> 2;
              const uint32_t *ts = (const uint32_t *)tk;
              uint32_t *td = (uint32_t *)pcur;
              for (k = 0; k < nw; k++) td[k] = ts[k];
              { uint32_t ta = vtick();
                gpu_jvdec_kick(pcur, 0, pcur, L, cbk, vshadow);
                d_tomok = (uint32_t)gpu_jvdec_wait();
                p_tom += vtick() - ta; }
              d_hello = gpu_jvdec_hello();
              d_pc = gpu_pc_read();
          }
          if (!d_tomok) {
              d_tomfail++;
#ifndef VR_NO68K
              /* the verified 68k token walk - kept so a Tom failure still
                 shows a picture, but it is NOT the goal and the lamp says so */
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
                            cbe = (const uint32_t *)(cbk + ((uint32_t)*s++ << 4));
                            w0 = (uint32_t *)dA;
                            w0[0] = cbe[0]; w0[80] = cbe[1];
                            w0[160] = cbe[2]; w0[240] = cbe[3];
                        }
                        dA += 4; left--;
                        if (++bx == 80) { bx = 0; dA += 960; }
                    }
                } }
#endif
          }
          /* THE BLITTER MOVES THE FRAME, NOT THE 68k (2026-08-08). The
             19200-long software copy measured 95ms/frame on silicon - three
             times Tom's whole decode and the single largest cost in the
             player. blit_copy is the same image move as hardware DMA, and
             it is what the title screen already uses for exactly this. */
          { uint32_t ta = vtick();
#ifdef VR_PHRASECOPY
            if (!d_phrase || !blit_copy_phrase(vshadow, bb, 240)) {
                d_blitfail++;               /* Blitter never idled: fall back */
                blit_copy(vshadow, bb, 240);
            }
#else
            blit_copy(vshadow, bb, 240);
#endif
            p_copy += vtick() - ta; }
          d_frames++;
#ifdef VR_LIVEPANEL
          /* ☠️ EVERY frame, never amortized. Painting it every 4th frame
             looked like a free 3/4 saving and read as SIX DEAD ROLLS: the
             whole-frame block copy rewrites the back buffer first, so a
             frame that skips the paint has no panel at all - the read-out
             simply vanishes for three frames out of four and every capture
             decodes as "no signal". The instrument has to be redrawn after
             whatever erased it. Use VR_NOPANEL to measure without it. */
          { uint32_t ta = vtick(); paint_markers(bb); p_paint += vtick() - ta; }
#endif
          { uint8_t *sw = pprev; pprev = pcur; pcur = sw; }
        }

        /* --- steady 4KB/frame refill + bulk rescue (v9 pacing) ----------- */
        if (remain > 0 && have - pos < 8192) {
            int want = (have - pos < 4096) ? 24576 : 4096;
            if (pos) { int mv = have - pos, k, nl = mv >> 2;
                uint32_t *d4 = (uint32_t *)vb;
                const uint32_t *s4 = (const uint32_t *)(vb + pos);
                for (k = 0; k < nl; k++) d4[k] = s4[k];
                for (k = nl << 2; k < mv; k++) vb[k] = vb[pos + k];
                have = mv; pos = 0; }
            if (want > (int)((VB_SIZE - have) & ~511))
                want = (VB_SIZE - have) & ~511;
            if (want > remain) want = remain;
            if (want > 0) {
                uint32_t ta = vtick();
                int rr = gd_fread((unsigned)vh, vb + have, (unsigned)want,
                                  GD_FREAD_CPU);
                p_read += vtick() - ta;
                if (rr == 0) { have += want; remain -= want; }
                else remain = 0;
            }
        }

        /* --- pace on the 60Hz VI, then publish -------------------------- */
        { int tgt = (int)(((uint32_t)(fi + 1) * 60u) / (uint32_t)vfps);
          uint32_t ta = vtick();
          while ((int)(frame_count - t0) < tgt)
              ;
          p_pace += vtick() - ta;
          d_fields = frame_count - t0;
          video_flip(); }

        if (joypad_read() & 0x800000u)      /* A skips */
            break;
    }
    gd_fclose((unsigned)vh);
    return 1;
}

int main(void)
{
    pal_init();
    video_init();
    video_set_clut(holdpal);
    /* NOT optional: gd_install() is what maps the GameDrive BIOS in. Without
       it every gd_fopen refuses and the clip silently never opens (the first
       lit vidrom roll sat on the hold screen with stage=1 for exactly this
       reason). File access is boot code, not a context ingredient. */
    gd_input_init();
#ifdef VR_GPUINIT
    d_gpu_ok = gpu_init();
#else
    d_gpu_ok = 1;                          /* no geotex kernel in this image */
#endif
#ifdef VR_JERRY
    jerry_init();
#endif
    video_set_disp240(1);                  /* clips are native 320x240 */
    BGC(BG_BLUE);                          /* BG shows only where nothing
                                              paints: on a lit roll the hold
                                              screen covers it, on a dead one
                                              it is the whole screen */

#ifdef VR_EARLYLOAD
    jvdec_load_verified();
    d_selftest = jvdec_selftest();
#endif

    hold_screen(120);                      /* ~2s of read-out before the clip */

#ifndef VR_EARLYLOAD
    jvdec_load_verified();
    d_selftest = jvdec_selftest();
#endif
    d_phrase = (uint32_t)phrase_selftest();
    /* PLAY ONCE, THEN HOLD - never touch the GameDrive again.
       ☠️ The looping version wedged the USB link twice (LIBUSB_ERROR_TIMEOUT,
       console left on the RetroHQ logo, and every roll after it read dark
       until a power cycle). Same hazard as writing the SD while the game
       runs: this ROM STREAMS from the cart, and a `jaggd` upload races its
       gd_freads. Once the clip ends the file is closed and the frame loop is
       pure repaint, so an upload always lands - and the final panel carries
       the WHOLE-CLIP averages instead of a 40-frame prefix. */
    play_clip(VR_CLIP);
    for (;;)
        hold_screen(120);
    return 0;
}
