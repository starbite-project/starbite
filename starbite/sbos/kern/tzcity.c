/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS cities (Track A, 8a): the search over tzcities.c (World Clock's and Weather's search fields). A city
 * matches when the query is the start of its name, or of a word in it, ignoring case and the usual Latin accents
 * ("zur" finds "Zürich", "york" finds "New York City"). Results in the table's order (by name). Pure C. */
#include "tzcities.h"

/* the next character of s folded to lower-case ASCII where it has an obvious base letter (UTF-8 Latin-1 and
 * Latin Extended-A); others pass through as their first byte. Advances *s */
static unsigned fold(const unsigned char **s)
{
    unsigned c = **s;
    if (!c) return 0;
    if (c < 0x80) {
        ++*s;
        return c >= 'A' && c <= 'Z' ? c + 32 : c;
    }
    if (c == 0xc3 && (*s)[1]) {
        static const char m[65] = "aaaaaaaceeeeiiiidnooooo*ouuuuytsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        unsigned d = (*s)[1] & 0x3f;
        *s += 2;
        return (unsigned char)m[d];
    }
    if ((c == 0xc4 || c == 0xc5) && (*s)[1]) {
        /* U+0100..U+017F: pairs of upper / lower case; the base letters in order */
        static const char m[129] = "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkkllllllllllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
        unsigned cp = ((c & 0x1f) << 6 | ((*s)[1] & 0x3f)) - 0x100;
        *s += 2;
        return cp < 128 ? (unsigned char)m[cp] : '?';
    }
    unsigned n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
    for (unsigned i = 0; i < n && **s; ++i) ++*s;
    return c;
}

static int starts(const unsigned char *name, const unsigned char *q)
{
    while (*q) {
        unsigned a = fold(&q), b = fold(&name);
        if (a != b) return 0;
    }
    return 1;
}

int tzc_find(const char *query, int *out, int max)
{
    int n = 0;
    const unsigned char *q = (const unsigned char *)query;
    if (!q || !*q || max <= 0) return 0;
    for (int i = 0; i < tzcities_n && n < max; ++i) {
        const unsigned char *s = (const unsigned char *)tzcities[i].name;
        int hit = starts(s, q);
        for (const unsigned char *p = s; !hit && *p; ++p)
            if ((*p == ' ' || *p == '-') && p[1]) hit = starts(p + 1, q);
        if (hit) out[n++] = i;
    }
    return n;
}
