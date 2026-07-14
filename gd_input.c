/*
 * gd_input.c - remote input over the GameDrive SD file API.
 *
 * The host writes INPUT.BIN to the SD card with `jaggd -wf INPUT.BIN`
 * (which uses the control endpoint and does NOT reset the running
 * game). Each frame the game opens/reads/closes INPUT.BIN and uses
 * its first big-endian u16 as a PAD_* bitmask. Open+close per frame
 * (not a held handle) so a host rewrite is always picked up fresh.
 *
 * Uses the bounded-wait gdbios.S bindings, so gd_install also
 * serves as safe detection (0 = GameDrive present).
 */
#include "gdbios.h"
#include "gd_input.h"

static uint32_t gd_workbuf[1024];               /* 4KB, long-aligned */
static uint16_t inbuf[2] __attribute__((aligned(4)));
static int gd_ready;

int gd_input_init(void)
{
    gd_ready = (gd_install(gd_workbuf) == 0);
    return gd_ready;
}

int gd_input_ready(void) { return gd_ready; }

uint32_t gd_input_poll(void)
{
    int h;

    if (!gd_ready)
        return 0;

    h = gd_fopen("INPUT.BIN", GD_FOPEN_READ | GD_FOPEN_OPEN_EXISTING);
    if (h < 0)
        h = gd_fopen("/INPUT.BIN", GD_FOPEN_READ | GD_FOPEN_OPEN_EXISTING);
    if (h < 0)
        return 0;                                /* not written yet */

    inbuf[0] = 0;
    gd_fread((unsigned)h, inbuf, 4, GD_FREAD_CPU);   /* even size/buffer */
    gd_fclose((unsigned)h);

    return inbuf[0];                             /* big-endian u16 PAD mask */
}
