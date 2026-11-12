/* SPDX-License-Identifier: GPL-2.0-only
 * Makes fixtures/sbfw/sbfw-crashlog.bin with the LOADER's own encoder (shim/starlte/sbfmt.c on branch claude/track-j:
 * cl_head_encode / cl_report_encode), so sbfw-store-test checks libsb's sbfw_api.c decoding against the loader's
 * bytes. Three 4 KiB blocks: the crash log's head (next slot 2, 2 reports) and report slots 0 and 1 (the crash log's
 * blocks 1 and 2). Built and run by sbfw-store-test.sh when SBFW_SHIM names the loader's shim/starlte:
 *   cc -I$SBFW_SHIM sbfw-crashlog-gen.c $SBFW_SHIM/sbfmt.c -o gen && ./gen OUT.bin
 */
#include <stdio.h>
#include <string.h>
#include "sbfmt.h"

static void text(struct cl_report *r, const char *s)
{
    r->text_len = (uint16_t)strlen(s);
    memcpy(r->text, s, r->text_len);
    r->text_lines = 0;
    for (const char *p = s; *p; ++p)
        r->text_lines += *p == '\n';
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    static uint8_t out[3 * CL_BLOCK];
    struct cl_head h;
    cl_head_init(&h);
    h.next = 2;
    h.count = 2;
    h.boots = 9;
    h.launch_seq = 41;
    h.launch_state = CL_IDLE;
    h.method = 2;
    strcpy(h.key, "linux-pmos");
    strcpy(h.name, "postmarketOS");
    h.launch_epoch = 1794470000ULL;
    h.hflags = CLH_CONSUMES;
    cl_head_encode(out, &h);

    /* slot 0, the older one: an XNU hang with no reason (the first text line is the message) */
    struct cl_report r;
    memset(&r, 0, sizeof(r));
    r.seq = 1;
    r.kind = CK_SWHANG;
    r.severity = SEV_CRASH;
    r.method = 1;
    r.source = RS_PMSG;
    r.epoch_detect = 1794400100ULL;
    r.epoch_launch = 1794400000ULL;
    r.epoch_event = 0;
    strcpy(r.key, "ios26");
    strcpy(r.name, "iOS 26");
    r.reg[CR_SB_RR] = 0xffffffffU;
    text(&r, "[...]\nSTARBITE: sweetreboot timer (no panic)\nlaunchd: still waiting\n");
    cl_report_encode(out + CL_BLOCK, &r);

    /* slot 1, the newest: a Linux panic with its reason */
    memset(&r, 0, sizeof(r));
    r.seq = 2;
    r.kind = CK_PANIC;
    r.severity = SEV_CRASH;
    r.method = 2;
    r.source = RS_DMESG;
    r.epoch_detect = 1794470300ULL;
    r.epoch_launch = 1794470000ULL;
    r.epoch_event = 1794470123ULL;
    strcpy(r.key, "linux-pmos");
    strcpy(r.name, "postmarketOS");
    strcpy(r.reason, "panic: Attempted to kill init! exitcode=0x0000000b");
    text(&r, "Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b\nCPU: 4 PID: 1 Comm: init\n");
    cl_report_encode(out + 2 * CL_BLOCK, &r);

    FILE *f = fopen(argv[1], "wb");
    if (!f || fwrite(out, 1, sizeof(out), f) != sizeof(out))
        return 1;
    fclose(f);
    return 0;
}
