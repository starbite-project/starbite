/* SPDX-License-Identifier: GPL-2.0-only
 * A function the relocation test (starbite/tools/reloc-test.c) calls inside the relocated image:
 * it walks static tables of function and string pointers, which only work once crt0.S (here: the
 * test, with the same sb_reloc) has fixed them. Pure: no globals written, no hardware.
 */
#include <stdint.h>

static int st_inc(int x)
{
    return x + 1;
}

static int st_triple(int x)
{
    return x * 3;
}

static int st_square(int x)
{
    return x * x;
}

struct st_ent {
    const char *name;
    int (*fn)(int);
};

static const struct st_ent st_tab[] = {
    { "inc", st_inc },
    { "triple", st_triple },
    { "square", st_square },
};

static const char *const st_strs[] = { "alpha", "beta", "gamma" };
static const struct st_ent *const st_ptr = &st_tab[1];

/* inc(10) * 1000 + 'i' + triple(10) * 1000 + 't' + square(10) * 1000 + 's' + 'l' + 'e' + 'a'
 * + triple(7) via st_ptr = 11105 + 30116 + 100115 + 108 + 101 + 97 + 21 = 141663 */
int64_t sb_selftest(void)
{
    /* launder the table addresses so the compiler cannot fold the loads at build time */
    const struct st_ent *tab = st_tab;
    const char *const *strs = st_strs;
    const struct st_ent *const *pp = &st_ptr;
    __asm__ volatile("" : "+r"(tab), "+r"(strs), "+r"(pp));
    int64_t s = 0;
    for (unsigned i = 0; i < 3; ++i)
        s += (int64_t)tab[i].fn(10) * 1000 + tab[i].name[0];
    for (unsigned i = 0; i < 3; ++i)
        s += strs[i][1];
    s += (*pp)->fn(7);
    return s;
}
