/* skunkdbg.c — formatted debug output over the Skunkboard USB console.
 * Only compiled into SKUNK_CONSOLE builds; see skunkdbg.h. No stdlib on
 * this target, so number formatting is done by hand into a static line
 * buffer (skunklib is not interrupt-safe and neither is this — main loop
 * only). */
#ifdef SKUNK_CONSOLE

#include "skunkdbg.h"

static char line[96];

static int put_s(int o, const char *s)
{
    while (*s && o < (int)sizeof(line) - 2)
        line[o++] = *s++;
    return o;
}

static int put_dec(int o, long v)
{
    char tmp[12];
    int  n = 0;
    if (v < 0) { if (o < (int)sizeof(line) - 2) line[o++] = '-'; v = -v; }
    do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && n < 11);
    while (n > 0 && o < (int)sizeof(line) - 2)
        line[o++] = tmp[--n];
    return o;
}

static int put_hex(int o, unsigned long v)
{
    static const char hd[] = "0123456789ABCDEF";
    int i;
    if (o < (int)sizeof(line) - 2) line[o++] = '$';
    for (i = 28; i >= 0; i -= 4)
        if (o < (int)sizeof(line) - 2)
            line[o++] = hd[(v >> i) & 15];
    return o;
}

static void flush(int o)
{
    line[o++] = '\n';
    line[o]   = 0;
    skunk_puts(line);
}

void dbg_str(const char *s)
{
    flush(put_s(0, s));
}

void dbg_kv(const char *label, long v)
{
    int o = put_s(0, label);
    o = put_s(o, "=");
    o = put_dec(o, v);
    flush(o);
}

void dbg_kx(const char *label, unsigned long v)
{
    int o = put_s(0, label);
    o = put_s(o, "=");
    o = put_hex(o, v);
    flush(o);
}

#endif /* SKUNK_CONSOLE */
