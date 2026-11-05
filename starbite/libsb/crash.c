/* SPDX-License-Identifier: GPL-2.0-only
 * The crash screen: an unexpected exception (crt0.S vectors) or a fatal start-up error. Drawn
 * straight into the framebuffer with the loader's 5x7 font (shim/starlte/fbtext.c), so it needs
 * nothing that may be broken: no fonts, no arena, no back buffer. Shows the reason (ESR class),
 * ELR and FAR as addresses and as offsets into the image (look them up with nm engineer.elf),
 * every register and the last log lines, and writes the same to the pmsg ring for TWRP.
 * Keys: Power reboots to the menu, Volume Up starts Engineer OS again (boot-next), Volume Down
 * powers off.
 */
#include "sb.h"

#define SCALE 4U
#define CW (6U * SCALE)
#define CH (10U * SCALE)
#define LEFT 32U
#define C_BG 0xff1a0606U
#define C_FG 0xffffffffU
#define C_HI 0xffff6a5aU
#define C_DIM 0xffb0a0a0U

struct glyph { char ch; uint8_t rows[7]; };
static const struct glyph font[] = {
    {' ',{0,0,0,0,0,0,0}}, {'-',{0,0,0,31,0,0,0}},
    {':',{0,4,4,0,4,4,0}}, {'?',{14,17,1,2,4,0,4}},
    {'.',{0,0,0,0,0,12,12}}, {'=',{0,0,31,0,31,0,0}},
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,30,1,1,17,14}},
    {'6',{6,8,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,2,12}},
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,14}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{14,4,4,4,4,4,14}}, {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
    {'(',{2,4,8,8,8,4,2}}, {')',{8,4,2,2,2,4,8}},
    {'/',{1,1,2,4,8,16,16}}, {'>',{8,4,2,1,2,4,8}},
    {'<',{2,4,8,16,8,4,2}}, {',',{0,0,0,0,12,4,8}},
    {'+',{0,4,4,31,4,4,0}}, {'%',{24,25,2,4,8,19,3}},
    {'_',{0,0,0,0,0,0,31}}, {'*',{0,4,21,14,21,4,0}},
    {'!',{4,4,4,4,4,0,4}}, {'[',{14,8,8,8,8,8,14}},
    {']',{14,2,2,2,2,2,14}}, {'#',{10,10,31,10,31,10,10}},
};

static const uint8_t *glyph_for(char ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - ('a' - 'A'));
    for (unsigned i = 0; i < sizeof(font) / sizeof(font[0]); ++i)
        if (font[i].ch == ch)
            return font[i].rows;
    return font[3].rows;
}

static void text(unsigned row, const char *s, uint32_t fg)
{
    volatile uint32_t *fb = (volatile uint32_t *)SB_FB;
    unsigned top = 120U + row * CH;
    if (top + CH > SB_FB_H)
        return;
    for (unsigned col = 0; s[col] && LEFT + (col + 1U) * CW <= SB_FB_W; ++col) {
        const uint8_t *g = glyph_for(s[col]);
        for (unsigned y = 0; y < 7; ++y)
            for (unsigned x = 0; x < 5; ++x) {
                if (!(g[y] & (1U << (4 - x))))
                    continue;
                for (unsigned sy = 0; sy < SCALE; ++sy)
                    for (unsigned sx = 0; sx < SCALE; ++sx)
                        fb[(top + y * SCALE + sy) * SB_FB_W + LEFT + col * CW + x * SCALE + sx] = fg;
            }
    }
}

static const char *esr_class(uint64_t esr)
{
    switch ((esr >> 26) & 0x3fU) {
    case 0x00: return "UNKNOWN INSTRUCTION";
    case 0x01: return "WFI/WFE TRAP";
    case 0x07: return "FP/SIMD ACCESS TRAP";
    case 0x0e: return "ILLEGAL EXECUTION STATE";
    case 0x15: return "SVC";
    case 0x17: return "SMC TRAP";
    case 0x18: return "SYSTEM REGISTER TRAP";
    case 0x20: case 0x21: return "INSTRUCTION ABORT";
    case 0x22: return "PC ALIGNMENT";
    case 0x24: case 0x25: return "DATA ABORT";
    case 0x26: return "SP ALIGNMENT";
    case 0x2f: return "SERROR";
    case 0x3c: return "BRK";
    default: return "EXCEPTION";
    }
}

static void line_hex(struct ub *u, const char *k, uint64_t v)
{
    ub_s(u, k);
    ub_hexn(u, v, 16);
}

void sb_crash_show(const char *why, const uint64_t *regs)
{
    volatile uint32_t *fb = (volatile uint32_t *)SB_FB;
    struct ub u;
    __asm__ volatile("msr daifset, #15");
    *(volatile uint32_t *)0x16030070UL = 0x1281U;    /* DECON free-running: reach the panel */
    for (unsigned i = 0; i < SB_FB_W * SB_FB_H; ++i)
        fb[i] = C_BG;
    for (unsigned y = 0; y < 24; ++y)
        for (unsigned x = 0; x < SB_FB_W; ++x)
            fb[y * SB_FB_W + x] = C_HI;
    unsigned row = 0;
    text(row++, "ENGINEER OS STOPPED", C_HI);
    if (!why)
        why = regs ? esr_class(regs[34]) : "FATAL";
    text(row++, why, C_FG);
    sb_pmsg_put("eng: CRASH ");
    sb_pmsg_put(why);
    sb_pmsg_put("\n");
    row++;
    if (regs) {
        uint64_t base = sb_boot.base;
        ub_init(&u);
        line_hex(&u, "ESR ", regs[34]);
        text(row++, u.b, C_FG);
        ub_init(&u);
        line_hex(&u, "ELR ", regs[32]);
        text(row++, u.b, C_FG);
        ub_init(&u);
        ub_s(&u, "    = IMAGE+");
        ub_hexn(&u, regs[32] - base, 8);
        text(row++, u.b, C_HI);
        ub_init(&u);
        line_hex(&u, "FAR ", regs[35]);
        text(row++, u.b, C_FG);
        ub_init(&u);
        line_hex(&u, "SPSR ", regs[33]);
        text(row++, u.b, C_DIM);
        ub_init(&u);
        line_hex(&u, "SP ", regs[31]);
        text(row++, u.b, C_DIM);
        ub_init(&u);
        line_hex(&u, "LR ", regs[30]);
        ub_s(&u, " +");
        ub_hexn(&u, regs[30] - base, 6);
        text(row++, u.b, C_DIM);
        for (unsigned i = 0; i < 30; i += 2) {
            ub_init(&u);
            ub_c(&u, 'X');
            ub_dec2(&u, i);
            ub_c(&u, ' ');
            ub_hexn(&u, regs[i], 16);
            ub_s(&u, "  X");
            ub_dec2(&u, i + 1);
            ub_c(&u, ' ');
            ub_hexn(&u, regs[i + 1], 16);
            text(row++, u.b, C_DIM);
        }
        struct ub p;
        ub_init(&p);
        line_hex(&p, "eng: CRASH ESR ", regs[34]);
        line_hex(&p, " ELR ", regs[32]);
        line_hex(&p, " FAR ", regs[35]);
        line_hex(&p, " BASE ", base);
        ub_c(&p, '\n');
        sb_pmsg_put(p.b);
    }
    row++;
    ub_init(&u);
    line_hex(&u, "IMAGE BASE ", sb_boot.base);
    text(row++, u.b, C_DIM);
    row++;
    text(row++, "LAST LOG LINES", C_HI);
    unsigned n = sb_log_count(), first = n > 12 ? n - 12 : 0;
    for (unsigned i = first; i < n && row < 64; ++i)
        text(row++, sb_log_line(i), C_DIM);
    text(69, "POWER: MENU   VOL UP: AGAIN   VOL DOWN: OFF", C_FG);
    sb_keys_release_wait();
    for (;;) {
        unsigned k = sb_keys();
        if (k & SB_K_POWER)
            sb_reboot(SB_INFORM_NORMAL);
        if (k & SB_K_UP)
            sb_boot_next("engineer");
        if (k & SB_K_DOWN) {
            sb_power_off();
            sb_reboot(SB_INFORM_NORMAL);
        }
        sb_mdelay(20);
    }
}
