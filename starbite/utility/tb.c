/* SPDX-License-Identifier: GPL-2.0-only
 * The loader's text buffer for the copied USB drivers (fastboot.h). Always NUL terminated. */
#include "fastboot.h"

void tb_init(struct tb *t, char *buf, unsigned cap)
{
    t->b = buf;
    t->n = 0;
    t->cap = cap;
    if (cap)
        buf[0] = 0;
}

void tb_c(struct tb *t, char c)
{
    if (t->n + 1 < t->cap) {
        t->b[t->n++] = c;
        t->b[t->n] = 0;
    }
}

void tb_s(struct tb *t, const char *s)
{
    while (s && *s)
        tb_c(t, *s++);
}

void tb_u(struct tb *t, uint64_t v)
{
    char d[21];
    int i = 0;
    do {
        d[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i)
        tb_c(t, d[--i]);
}

void tb_hex(struct tb *t, uint64_t v, unsigned digits)
{
    tb_s(t, "0x");
    for (int i = (int)digits - 1; i >= 0; --i)
        tb_c(t, "0123456789abcdef"[(v >> (4 * i)) & 0xfU]);
}

void tb_reg(struct tb *t, const char *name, uint32_t v)
{
    tb_s(t, name);
    tb_c(t, ' ');
    tb_hex(t, v, 8);
    tb_c(t, '\n');
}
