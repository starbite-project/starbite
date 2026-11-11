/* SPDX-License-Identifier: GPL-2.0-only
 * sbcl: the crash ledger (docs/WIN11-GPU.md 6.45). One 4 KiB page of the UEFI Device window that survives warm
 * resets (0xFED1E000, beside the pmsg ring 0xFED14000 and the big-core guard's arbiter page 0xFED1F000; inside
 * Samsung's no-map snapshot-after-pstore region, which nothing writes on our boots). sbwdog (boot start) marks
 * each Windows boot in it, keeps its uptime current, writes the stop code and parameters from a bugcheck callback
 * (resolved to module+offset against its own snapshot of the loaded modules), and marks a clean shutdown. At the
 * next Windows boot it reads what the previous boot left and writes it to the registry, so the stop code survives
 * even when Windows boots again before TWRP (the pmsg ring does not: every boot overwrites it).
 * Portable (host-tested): layout, check word, the module lookup and the text.
 */
#ifndef SBCL_CORE_H
#define SBCL_CORE_H

#ifdef SB_HOST
#include <stdint.h>
typedef uint32_t sbcl_u32;
typedef uint64_t sbcl_u64;
#else
typedef unsigned int sbcl_u32;
typedef unsigned long long sbcl_u64;
#endif

/* T39: T35-T38 used 0xFED1E000, which does not survive the restart (T38: the clean boot found no record; the
 * loader keeps only the ramoops zones 0xFED10000..0xFED18000, which it snapshots and never writes). The record now
 * sits in the last KiB of zone 0 (the dmesg zone; Windows boots never write it, Linux writes it only on a panic). */
#define SBCL_PA 0xFED13000ULL             /* the page mapped */
#define SBCL_OFF 0xC00U                   /* the record's offset in it */
#define SBCL_SIZE 0x1000U
#define SBCL_MAGIC 0x4C434253U            /* 'SBCL' */
#define SBCL_VERSION 1U

enum { SBCL_NONE = 0, SBCL_RUNNING = 1, SBCL_SHUTDOWN = 2, SBCL_BUGCHECK = 3 };

#define SBCL_WHERE 48
struct sbcl_page {
    sbcl_u32 magic, version;
    sbcl_u32 boot_seq;                    /* Windows boots seen by this ledger */
    sbcl_u32 state;                       /* SBCL_* of the boot that wrote it */
    sbcl_u32 uptime_s;                    /* since sbwdog started, refreshed every feed (5 s) */
    sbcl_u32 bc_uptime_s;
    sbcl_u32 bc_code;
    sbcl_u32 mods;                        /* modules in the snapshot at the bugcheck (0: none yet) */
    sbcl_u64 p[4];
    char where[4][SBCL_WHERE];            /* p[i] as module+0xoffset, or "" */
    sbcl_u32 shutdown_uptime_s;
    sbcl_u32 check;                       /* sbcl_check over everything before it */
};

struct sbcl_mod {
    sbcl_u64 base;
    sbcl_u32 size;
    char name[28];
};

sbcl_u32 sbcl_check(const struct sbcl_page *p);
void sbcl_seal(struct sbcl_page *p);
/* A record a previous boot left: magic, version, check word and a known state. */
int sbcl_valid(const struct sbcl_page *p);
/* "name+0x1a2b" when addr lies in one of the n modules, else "" (out holds SBCL_WHERE bytes). */
void sbcl_resolve(const struct sbcl_mod *m, unsigned n, sbcl_u64 addr, char *out);
/* The bugcheck's name, or "" for an unknown code. */
const char *sbcl_bc_name(sbcl_u32 code);
/* One line for the registry: how the previous boot ended. Returns the length. */
unsigned sbcl_describe(const struct sbcl_page *p, char *out, unsigned cap);

#endif
