#ifndef JAG_GD_INPUT_H
#define JAG_GD_INPUT_H

#include <stdint.h>

/* Remote input over the GameDrive: the host writes INPUT.BIN to the SD
 * card (jaggd -wf) and the running game reads it each frame as a pad
 * bitmask (PAD_* bits, big-endian u16). Lets input be driven/verified
 * without a physical controller. gd_input_init returns 1 if a
 * GameDrive BIOS was installed. */
int      gd_input_init(void);
int      gd_input_ready(void);   /* 1 = GD BIOS installed */
uint32_t gd_input_poll(void);

#endif
