/*
 * gdbios.h - Jaguar GameDrive BIOS, the subset this port uses.
 *
 * See gdbios.S (GNU-as port of JagGD's bindings, with bounded SPI
 * waits so gd_install doubles as safe hardware detection). All calls
 * except gd_install return -1 when no BIOS is installed.
 */
#ifndef JAG_GDBIOS_H
#define JAG_GDBIOS_H

/* gd_fopen modes (JagGD gdbios.h values) */
#define GD_FOPEN_READ          0x01
#define GD_FOPEN_OPEN_EXISTING 0x00

/* gd_fread flags */
#define GD_FREAD_CPU 0

/* Install the GD BIOS into buf (4KB, long-aligned). 0 = GameDrive
 * present and ready; negative = absent, old firmware, or timeout
 * (safe on consoles/emulators without the cart). */
int gd_install(void *buf);

int gd_card_in(void);
int gd_fopen(const char *name, unsigned mode);   /* handle or <0 */
int gd_fclose(unsigned handle);
int gd_fread(unsigned handle, void *buf, unsigned n, unsigned flags);
int gd_fsize(unsigned handle);                   /* bytes or <0 */

#endif /* JAG_GDBIOS_H */
