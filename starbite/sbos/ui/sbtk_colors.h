/* SPDX-License-Identifier: MIT
 * Starbite OS Next's system palette (GAP-LIST #1; docs/design/IOS-REFERENCE.md §15, HIG "Color", iOS 26).
 * One table of named colours with a light and a dark column. The shell is dark first: SBC_DARK picks the dark
 * column; the light one is defined for the Appearance switch later (GAP-LIST #27) and is not used yet.
 * Use the names, not literals: C_ACCENT, C_RED ..., C_GROUND (an app's page), C_CARD (inset-grouped cards),
 * C_SEP (separators), C_LABEL / C_LABEL2 / C_LABEL3 (primary / secondary / tertiary text).
 * sbca(i, a) is a colour at alpha a (tints, washes); sbc_rgb(i) its 0xRRGGBB (opaque solids, raw fills).
 * Glass tints are not here: tk_glass.c's constants are the glass. Icons and Emergency's own red set
 * (shell_survival.inc's SV_*) keep their colours. */
#ifndef SBTK_COLORS_H
#define SBTK_COLORS_H
#include "sbtk.h"

#define SBC_DARK 1

enum {
    SBC_ACCENT,       /* systemBlue: links, carets, buttons, the selection */
    SBC_RED,
    SBC_ORANGE,
    SBC_YELLOW,
    SBC_GREEN,
    SBC_PINK,
    SBC_GRAY,         /* systemGray */
    SBC_GRAY2,
    SBC_GRAY3,
    SBC_GRAY4,
    SBC_GRAY5,
    SBC_GRAY6,
    SBC_GROUND,       /* systemBackground: an app's page */
    SBC_GROUPED,      /* systemGroupedBackground: under inset-grouped lists */
    SBC_CARD,         /* secondarySystemGroupedBackground: the inset-grouped cards */
    SBC_SEP,          /* separator */
    SBC_LABEL,
    SBC_LABEL2,       /* secondaryLabel */
    SBC_LABEL3,       /* tertiaryLabel (placeholders, disabled) */
    SBC_LABEL4,       /* quaternaryLabel */
    SBC_FILL,         /* systemFill (an off switch, a disabled button) */
    SBC_N
};

typedef struct { uint32_t rgb; float a; } sbc_v;
static const struct { sbc_v light, dark; } SBC[SBC_N] = {
    [SBC_ACCENT]  = { { 0x0088ff, 1 },   { 0x0091ff, 1 } },
    [SBC_RED]     = { { 0xff383c, 1 },   { 0xff4245, 1 } },
    [SBC_ORANGE]  = { { 0xff8d28, 1 },   { 0xff9230, 1 } },
    [SBC_YELLOW]  = { { 0xffcc00, 1 },   { 0xffd600, 1 } },
    [SBC_GREEN]   = { { 0x34c759, 1 },   { 0x30d158, 1 } },
    [SBC_PINK]    = { { 0xff2d55, 1 },   { 0xff375f, 1 } },
    [SBC_GRAY]    = { { 0x8e8e93, 1 },   { 0x8e8e93, 1 } },
    [SBC_GRAY2]   = { { 0xaeaeb2, 1 },   { 0x636366, 1 } },
    [SBC_GRAY3]   = { { 0xc7c7cc, 1 },   { 0x48484a, 1 } },
    [SBC_GRAY4]   = { { 0xd1d1d6, 1 },   { 0x3a3a3c, 1 } },
    [SBC_GRAY5]   = { { 0xe5e5ea, 1 },   { 0x2c2c2e, 1 } },
    [SBC_GRAY6]   = { { 0xf2f2f7, 1 },   { 0x1c1c1e, 1 } },
    [SBC_GROUND]  = { { 0xffffff, 1 },   { 0x000000, 1 } },
    [SBC_GROUPED] = { { 0xf2f2f7, 1 },   { 0x000000, 1 } },
    [SBC_CARD]    = { { 0xffffff, 1 },   { 0x1c1c1e, 1 } },
    [SBC_SEP]     = { { 0x3c3c43, .29f }, { 0x545458, .6f } },
    [SBC_LABEL]   = { { 0x000000, 1 },   { 0xffffff, 1 } },
    [SBC_LABEL2]  = { { 0x3c3c43, .6f }, { 0xebebf5, .6f } },
    [SBC_LABEL3]  = { { 0x3c3c43, .3f }, { 0xebebf5, .3f } },
    [SBC_LABEL4]  = { { 0x3c3c43, .18f }, { 0xebebf5, .16f } },
    [SBC_FILL]    = { { 0x787880, .2f }, { 0x787880, .36f } },
};
static inline sbc_v sbc_get(int i) { return SBC_DARK ? SBC[i].dark : SBC[i].light; }
static inline tk_col sbc(int i) { sbc_v v = sbc_get(i); return tk_hex(v.rgb, v.a); }
static inline tk_col sbca(int i, float a) { return tk_hex(sbc_get(i).rgb, a); }
static inline uint32_t sbc_rgb(int i) { return sbc_get(i).rgb; }
static inline uint32_t sbc_argb(int i) { return 0xff000000u | sbc_get(i).rgb; }   /* opaque, premultiplied */

#define C_ACCENT sbc(SBC_ACCENT)
#define C_RED sbc(SBC_RED)
#define C_ORANGE sbc(SBC_ORANGE)
#define C_YELLOW sbc(SBC_YELLOW)
#define C_GREEN sbc(SBC_GREEN)
#define C_GROUND sbc(SBC_GROUND)
#define C_CARD sbc(SBC_CARD)
#define C_SEP sbc(SBC_SEP)
#define C_LABEL sbc(SBC_LABEL)
#define C_LABEL2 sbc(SBC_LABEL2)
#define C_LABEL3 sbc(SBC_LABEL3)
#define C_FILL sbc(SBC_FILL)
#endif
