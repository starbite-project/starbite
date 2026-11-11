/* SPDX-License-Identifier: MIT
 * sbtk asset pack (tools/sbtk-pack.py): "SBTKPAK1", u32 count, u32 0, then count 64-byte entries
 * {char name[32]; u32 type, w, h, off, size; i32 aux; u32 0, 0} and the data at 64-byte aligned offsets.
 * type 1 a TrueType font (aux = the packed face, sbtk.h TK_TX400 ..), 2 its layout table, 3 a premultiplied 0xAARRGGBB image (aux = the
 * margin baked around it, px x 256). The pack is used in place: images are views into it. */
#include "sbtk.h"

#define MAXICON 160
static struct { const char *name; tk_bmp b; float pad; } I[MAXICON];
static int nicon;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int tk_pack_open(const uint8_t *pak, uint32_t len)
{
    if (len < 16 || pak[0] != 'S' || pak[7] != '1') return 0;
    uint32_t n = rd32(pak + 8);
    const uint8_t *ttf[TK_NFACE] = { 0 }, *lay[TK_NFACE] = { 0 };
    uint32_t laylen[TK_NFACE] = { 0 };
    nicon = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = pak + 16 + 64 * i;
        uint32_t type = rd32(e + 32), w = rd32(e + 36), h = rd32(e + 40), off = rd32(e + 44), size = rd32(e + 48);
        int32_t aux = (int32_t)rd32(e + 52);
        if (off + size > len) return 0;
        if (type == 1 && aux >= 0 && aux < TK_NFACE) ttf[aux] = pak + off;
        if (type == 2 && aux >= 0 && aux < TK_NFACE) { lay[aux] = pak + off; laylen[aux] = size; }
        if (type == 3 && nicon < MAXICON) {
            I[nicon].name = (const char *)e;
            I[nicon].b.w = (int)w; I[nicon].b.h = (int)h; I[nicon].b.stride = (int)w;
            I[nicon].b.px = (uint32_t *)(uintptr_t)(pak + off);
            I[nicon].pad = aux / 256.0f;
            nicon++;
        }
    }
    for (int f = 0; f < TK_NFACE; f++)
        if (!ttf[f] || !tk_font_load(f, ttf[f], lay[f], laylen[f])) return 0;
    return 1;
}

const tk_bmp *tk_icon(const char *name)
{
    for (int i = 0; i < nicon; i++) if (!tk_strcmp(I[i].name, name)) return &I[i].b;
    return 0;
}
float tk_icon_pad(const char *name)
{
    for (int i = 0; i < nicon; i++) if (!tk_strcmp(I[i].name, name)) return I[i].pad;
    return 0;
}
