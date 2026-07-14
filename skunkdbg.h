/* skunkdbg.h - printf-style debugging over the Skunkboard USB console.
 *
 * Enabled in Skunkboard builds only: `make NOGD=1` defines SKUNK_CONSOLE
 * and links skunk.s / skunkglue.s (vendored from tursilion/skunk_jcp).
 * Attach the PC console with `jcp -c` (or `jcp -c file.cof`). In
 * GameDrive builds every call compiles to nothing.
 *
 * Skunklib: writes poll the EZ-Host across the cart bus and TIME OUT
 * (~ms each) if no console is attached. NOT interrupt-safe: never call
 * from the VI ISR.
 *
 * neogeo-elf (like m68k-linux-gnu) emits ELF symbols without a leading
 * underscore, while skunkglue.s exports `_skunk_*`; the decls carry
 * explicit asm() names to match.
 */
#ifndef SKUNKDBG_H
#define SKUNKDBG_H

#ifdef SKUNK_CONSOLE

void skunk_init(void)           __asm__("_skunk_init");
long skunk_up(void)             __asm__("_skunk_up");
void skunk_puts(const char *s)  __asm__("_skunk_puts");
void skunk_close(void)          __asm__("_skunk_close");

void dbg_str(const char *s);
void dbg_kv(const char *label, long v);
void dbg_kx(const char *label, unsigned long v);

#else

#define skunk_init()     ((void)0)
#define skunk_up()       (0L)
#define skunk_puts(s)    ((void)0)
#define skunk_close()    ((void)0)
#define dbg_str(s)       ((void)0)
#define dbg_kv(l, v)     ((void)0)
#define dbg_kx(l, v)     ((void)0)

#endif /* SKUNK_CONSOLE */

#endif /* SKUNKDBG_H */
