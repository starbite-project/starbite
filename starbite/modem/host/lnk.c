/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm host check on real traffic: decodes the "mif: LNK-RX(n): f8 eb ..." lines Samsung's kernel logged on this
 * phone (build/test-boots/, never in git) through sbm's SIPC5 + IPC parsers and the cell state. The kernel shows
 * at most 16 bytes of a frame; longer frames ("...") are checked for their header only. Prints counts per
 * message and the resulting state, never payload bytes. Exit 1 if a complete frame does not parse.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sbm.h"

struct tally { uint8_t dir, type; uint16_t cmd; unsigned n; };
static struct tally tl[512];
static unsigned ntl;

static void count(int dir, uint16_t cmd, uint8_t type)
{
    for (unsigned i = 0; i < ntl; ++i)
        if (tl[i].dir == dir && tl[i].cmd == cmd && tl[i].type == type) {
            tl[i].n++;
            return;
        }
    if (ntl < 512)
        tl[ntl++] = (struct tally){ (uint8_t)dir, type, cmd, 1 };
}

int main(int argc, char **argv)
{
    unsigned frames = 0, complete = 0, truncated = 0, bad = 0, fmt = 0;
    for (int a = 1; a < argc; ++a) {
        FILE *f = fopen(argv[a], "r");
        if (!f)
            continue;
        struct sbm_cell_state st;
        sbm_cell_init(&st);
        char line[1024];
        unsigned here = 0;
        while (fgets(line, sizeof(line), f)) {
            char *p = strstr(line, "LNK-");
            if (!p || (strncmp(p + 4, "RX(", 3) && strncmp(p + 4, "TX(", 3)))
                continue;
            int rx = p[4] == 'R';
            unsigned n = (unsigned)strtoul(p + 7, &p, 10);
            if (strncmp(p, "):", 2))
                continue;
            p += 2;
            uint8_t b[64];
            unsigned k = 0;
            while (k < sizeof(b)) {
                while (*p == ' ')
                    ++p;
                char *e;
                unsigned long v = strtoul(p, &e, 16);
                if (e != p + 2)
                    break;
                b[k++] = (uint8_t)v;
                p = e;
            }
            frames++;
            here++;
            struct sbm_sipc5 fr;
            if (k < n) {                 /* truncated by the kernel: the header must still agree */
                truncated++;
                if (k < 4 || (b[0] & 0xF8) != 0xF8 || sbm_le16(b + 2) != n) {
                    bad++;
                    printf("lnk: %s: bad truncated frame header (%u of %u bytes)\n", argv[a], k, n);
                }
                if (k >= 11 && sbm_ch_is_fmt(b[1]))
                    count(rx, SBM_CMD(b[8], b[9]), b[10]);
                continue;
            }
            complete++;
            struct sbm_ipc_msg m;
            if (sbm_sipc5_parse(b, k, &fr) != 0 || fr.frame_len != n) {
                bad++;
                printf("lnk: %s: frame does not parse (%u bytes)\n", argv[a], n);
                continue;
            }
            if (!sbm_ch_is_fmt(fr.ch))
                continue;
            fmt++;
            if (sbm_ipc_parse(fr.payload, fr.payload_len, &m) != 0 || m.len != fr.payload_len) {
                bad++;
                printf("lnk: %s: IPC message does not parse (%u bytes)\n", argv[a], n);
                continue;
            }
            count(rx, m.cmd, m.type);
            if (rx)
                sbm_cell_update(&st, &m);
        }
        fclose(f);
        if (here) {
            char s[200];
            sbm_cell_line(&st, s, sizeof(s));
            const char *base = strrchr(argv[a], '/');
            const char *dir = base;
            while (dir && dir > argv[a] && dir[-1] != '/')
                --dir;
            printf("lnk: %.*s %u frames -> %s\n", base && dir ? (int)(base - dir) + (int)strlen(base) : 0,
                   dir ? dir : "", here, s);
        }
    }
    for (unsigned i = 0; i < ntl; ++i) {
        char nb[12];
        printf("lnk:   %s %-20s %-5s x%u\n", tl[i].dir ? "RX" : "TX", sbm_ipc_name(tl[i].cmd, nb),
               sbm_ipc_type_name(tl[i].type, tl[i].dir), tl[i].n);
    }
    printf("lnk: %u frames (%u complete, %u FMT; %u truncated by the kernel), %u bad\n", frames, complete, fmt,
           truncated, bad);
    return bad ? 1 : 0;
}
