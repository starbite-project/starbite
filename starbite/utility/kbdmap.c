/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS keyboard layouts: character -> HID usage (HUT 1.12 section 10) and modifiers, for the
 * computer's layout. The phone sends key positions; the computer's layout setting decides the character,
 * so text is typed as the key presses that give it on that layout.
 *
 * Finnish (the same keys as Swedish): letters and digits sit where US has them; ISO keys: 0x64 left of Z
 * (< >), 0x31 left of Enter (' *; 0x31 and 0x32 are the same key to macOS, Windows and Linux), 0x35 left
 * of 1 (section sign). Dead keys (´ ` ^ ¨ ~) type the accent followed by Space.
 * Mac Finnish: Option for @ £ $ [ ] | ~, Shift+Option for { } \, Shift+4 is the euro sign.
 * PC Finnish: AltGr for @ £ $ € { [ ] } \ | ~ (Windows and Linux's fi layout). */
#include "kbdmap.h"

const char *const kbd_layout_name[KL_N] = { "US", "Finnish (Mac)", "Finnish (PC)" };
const char *const kbd_layout_key[KL_N] = { "us", "fi-mac", "fi-pc" };

#define S KM_SHIFT
#define A KM_ALTGR
#define DEAD 0x80U                      /* in the table's mod: a dead key, Space follows */

struct km { uint16_t cp; uint8_t mod, key; };

/* US punctuation (letters, digits, space, Enter, Tab are common to every layout) */
static const struct km us[] = {
    { '!', S, 0x1e }, { '@', S, 0x1f }, { '#', S, 0x20 }, { '$', S, 0x21 }, { '%', S, 0x22 }, { '^', S, 0x23 },
    { '&', S, 0x24 }, { '*', S, 0x25 }, { '(', S, 0x26 }, { ')', S, 0x27 }, { '-', 0, 0x2d }, { '_', S, 0x2d },
    { '=', 0, 0x2e }, { '+', S, 0x2e }, { '[', 0, 0x2f }, { '{', S, 0x2f }, { ']', 0, 0x30 }, { '}', S, 0x30 },
    { '\\', 0, 0x31 }, { '|', S, 0x31 }, { ';', 0, 0x33 }, { ':', S, 0x33 }, { '\'', 0, 0x34 }, { '"', S, 0x34 },
    { '`', 0, 0x35 }, { '~', S, 0x35 }, { ',', 0, 0x36 }, { '<', S, 0x36 }, { '.', 0, 0x37 }, { '>', S, 0x37 },
    { '/', 0, 0x38 }, { '?', S, 0x38 },
};

/* Finnish, both: what Shift and the plain ISO keys give */
static const struct km fi[] = {
    { '!', S, 0x1e }, { '"', S, 0x1f }, { '#', S, 0x20 }, { '%', S, 0x22 }, { '&', S, 0x23 }, { '/', S, 0x24 },
    { '(', S, 0x25 }, { ')', S, 0x26 }, { '=', S, 0x27 }, { '+', 0, 0x2d }, { '?', S, 0x2d },
    { 0xb4, DEAD, 0x2e }, { '`', DEAD | S, 0x2e }, { 0xe5, 0, 0x2f }, { 0xc5, S, 0x2f },
    { 0xa8, DEAD, 0x30 }, { '^', DEAD | S, 0x30 }, { '\'', 0, 0x31 }, { '*', S, 0x31 },
    { 0xf6, 0, 0x33 }, { 0xd6, S, 0x33 }, { 0xe4, 0, 0x34 }, { 0xc4, S, 0x34 }, { 0xa7, 0, 0x35 },
    { ',', 0, 0x36 }, { ';', S, 0x36 }, { '.', 0, 0x37 }, { ':', S, 0x37 }, { '-', 0, 0x38 }, { '_', S, 0x38 },
    { '<', 0, 0x64 }, { '>', S, 0x64 },
};

static const struct km fi_mac[] = {
    { 0x20ac, S, 0x21 }, { '@', A, 0x1f }, { 0xa3, A, 0x20 }, { '$', A, 0x21 }, { '|', A, 0x24 },
    { '\\', S | A, 0x24 }, { '[', A, 0x25 }, { ']', A, 0x26 }, { '{', S | A, 0x25 }, { '}', S | A, 0x26 },
    { '~', DEAD | A, 0x30 }, { 0xb0, S, 0x35 },
};

static const struct km fi_pc[] = {
    { 0xa4, S, 0x21 }, { '@', A, 0x1f }, { 0xa3, A, 0x20 }, { '$', A, 0x21 }, { 0x20ac, A, 0x22 },
    { '{', A, 0x24 }, { '[', A, 0x25 }, { ']', A, 0x26 }, { '}', A, 0x27 }, { '\\', A, 0x2d },
    { '|', A, 0x64 }, { '~', DEAD | A, 0x30 }, { 0xbd, S, 0x35 }, { 0xb5, A, 0x10 },
};

static int look(const struct km *t, unsigned n, uint32_t cp, struct kstroke s[2])
{
    for (unsigned i = 0; i < n; ++i) {
        if (t[i].cp != cp)
            continue;
        s[0].mod = (uint8_t)(t[i].mod & ~DEAD);
        s[0].key = t[i].key;
        if (!(t[i].mod & DEAD))
            return 1;
        s[1].mod = 0;
        s[1].key = 0x2c;                /* Space after a dead key: the accent itself */
        return 2;
    }
    return 0;
}

#define N(t) (unsigned)(sizeof(t) / sizeof(t[0]))

int kbd_char(int layout, uint32_t cp, struct kstroke s[2])
{
    s[0].mod = 0;
    if (cp >= 'a' && cp <= 'z') {
        s[0].key = (uint8_t)(0x04 + cp - 'a');
        return 1;
    }
    if (cp >= 'A' && cp <= 'Z') {
        s[0].mod = S;
        s[0].key = (uint8_t)(0x04 + cp - 'A');
        return 1;
    }
    if (cp >= '1' && cp <= '9') {
        s[0].key = (uint8_t)(0x1e + cp - '1');
        return 1;
    }
    switch (cp) {
    case '0': s[0].key = 0x27; return 1;
    case ' ': s[0].key = 0x2c; return 1;
    case '\n': s[0].key = 0x28; return 1;
    case '\t': s[0].key = 0x2b; return 1;
    default: break;
    }
    if (layout == KL_US)
        return look(us, N(us), cp, s);
    if (layout != KL_FI_MAC && layout != KL_FI_PC)
        return 0;
    int n = layout == KL_FI_MAC ? look(fi_mac, N(fi_mac), cp, s) : look(fi_pc, N(fi_pc), cp, s);
    return n ? n : look(fi, N(fi), cp, s);
}

unsigned kbd_utf8(const char *s, uint32_t *cp)
{
    const uint8_t *b = (const uint8_t *)s;
    if (b[0] < 0x80) {
        *cp = b[0];
        return 1;
    }
    if ((b[0] & 0xe0) == 0xc0 && (b[1] & 0xc0) == 0x80) {
        *cp = (uint32_t)(b[0] & 0x1f) << 6 | (b[1] & 0x3f);
        return 2;
    }
    if ((b[0] & 0xf0) == 0xe0 && (b[1] & 0xc0) == 0x80 && (b[2] & 0xc0) == 0x80) {
        *cp = (uint32_t)(b[0] & 0x0f) << 12 | (uint32_t)(b[1] & 0x3f) << 6 | (b[2] & 0x3f);
        return 3;
    }
    *cp = 0xfffd;
    return 1;
}

int kbd_layout_find(const char *k, unsigned n)
{
    for (int l = 0; l < KL_N; ++l) {
        const char *t = kbd_layout_key[l];
        unsigned i = 0;
        for (; i < n && t[i]; ++i) {
            char c = k[i] >= 'A' && k[i] <= 'Z' ? (char)(k[i] + 32) : k[i];
            if (c != t[i])
                break;
        }
        if (i == n && !t[i])
            return l;
    }
    return -1;
}
