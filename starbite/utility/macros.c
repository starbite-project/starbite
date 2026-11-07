/* SPDX-License-Identifier: GPL-2.0-only
 * The macro pad's buttons from /starbite/macros.toml on the card (docs/UTILITY-OS.md), one per line:
 *     "Copy" = "cmd+c"
 *     "Lock screen" = "ctrl+cmd+q"
 *     "Sign-off" = "text:Best regards"
 * Keys: modifiers cmd (command, gui), ctrl (control), alt (option, opt), shift; then one key: a-z, 0-9,
 * enter (return), esc (escape), backspace (delete), tab, space, minus, equal, comma, period, slash,
 * up, down, left, right, f1-f12. Lines starting with # and blank lines are skipped; a bad line is skipped.
 * Pure C (the host test parses examples). */
#include "macros.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int eqn(const char *a, unsigned n, const char *b)
{
    unsigned i = 0;
    for (; i < n && b[i]; ++i)
        if (lower(a[i]) != b[i])
            return 0;
    return i == n && !b[i];
}

static int keycode(const char *k, unsigned n, uint8_t *out)
{
    static const struct { const char *name; uint8_t code; } named[] = {
        { "enter", 0x28 }, { "return", 0x28 }, { "esc", 0x29 }, { "escape", 0x29 }, { "backspace", 0x2a },
        { "delete", 0x2a }, { "tab", 0x2b }, { "space", 0x2c }, { "minus", 0x2d }, { "equal", 0x2e },
        { "comma", 0x36 }, { "period", 0x37 }, { "slash", 0x38 }, { "right", 0x4f }, { "left", 0x50 },
        { "down", 0x51 }, { "up", 0x52 },
    };
    if (n == 1) {
        int c = lower(k[0]);
        if (c >= 'a' && c <= 'z') { *out = (uint8_t)(0x04 + c - 'a'); return 1; }
        if (c >= '1' && c <= '9') { *out = (uint8_t)(0x1e + c - '1'); return 1; }
        if (c == '0') { *out = 0x27; return 1; }
    }
    if ((n == 2 || n == 3) && lower(k[0]) == 'f') {
        unsigned v = 0;
        for (unsigned i = 1; i < n; ++i) {
            if (k[i] < '0' || k[i] > '9')
                return 0;
            v = v * 10 + (unsigned)(k[i] - '0');
        }
        if (v >= 1 && v <= 12) { *out = (uint8_t)(0x3a + v - 1); return 1; }
        return 0;
    }
    for (unsigned i = 0; i < sizeof(named) / sizeof(named[0]); ++i)
        if (eqn(k, n, named[i].name)) { *out = named[i].code; return 1; }
    return 0;
}

static int quoted(const char **p, const char *end, char *out, unsigned cap)
{
    const char *s = *p;
    while (s < end && (*s == ' ' || *s == '\t'))
        ++s;
    if (s >= end || *s != '"')
        return 0;
    ++s;
    unsigned n = 0;
    while (s < end && *s != '"') {
        if (n + 1 < cap)
            out[n++] = *s;
        ++s;
    }
    if (s >= end)
        return 0;
    out[n] = 0;
    *p = s + 1;
    return 1;
}

static int spec(const char *v, struct macro *m)
{
    if (v[0] == 't' && v[1] == 'e' && v[2] == 'x' && v[3] == 't' && v[4] == ':') {
        unsigned i = 0;
        for (const char *s = v + 5; *s && i + 1 < sizeof(m->text); ++s)
            m->text[i++] = *s;
        m->text[i] = 0;
        return i > 0;
    }
    m->mod = 0;
    const char *s = v;
    for (;;) {
        const char *e = s;
        while (*e && *e != '+')
            ++e;
        unsigned n = (unsigned)(e - s);
        if (!*e) {                                    /* the last token: the key */
            return n && keycode(s, n, &m->key);
        }
        if (eqn(s, n, "cmd") || eqn(s, n, "command") || eqn(s, n, "gui")) m->mod |= 0x08;
        else if (eqn(s, n, "ctrl") || eqn(s, n, "control")) m->mod |= 0x01;
        else if (eqn(s, n, "alt") || eqn(s, n, "option") || eqn(s, n, "opt")) m->mod |= 0x04;
        else if (eqn(s, n, "shift")) m->mod |= 0x02;
        else return 0;
        s = e + 1;
    }
}

int macros_parse(const char *src, unsigned len, struct macro *out, int max)
{
    int n = 0;
    const char *p = src, *end = src + len;
    if (len >= 3 && (uint8_t)src[0] == 0xef && (uint8_t)src[1] == 0xbb && (uint8_t)src[2] == 0xbf)
        p += 3;                                       /* a UTF-8 byte order mark (Windows editors) */
    while (p < end && n < max) {
        const char *eol = p;
        while (eol < end && *eol != '\n')
            ++eol;
        const char *q = p;
        while (q < eol && (*q == ' ' || *q == '\t' || *q == '\r'))
            ++q;
        if (q < eol && *q != '#') {
            struct macro m;
            for (unsigned i = 0; i < sizeof(m); ++i)
                ((char *)&m)[i] = 0;
            char val[64];
            const char *r = q;
            if (quoted(&r, eol, m.label, sizeof(m.label))) {
                while (r < eol && (*r == ' ' || *r == '\t'))
                    ++r;
                if (r < eol && *r == '=') {
                    ++r;
                    if (quoted(&r, eol, val, sizeof(val)) && spec(val, &m))
                        out[n++] = m;
                }
            }
        }
        p = eol + 1;
    }
    return n;
}
