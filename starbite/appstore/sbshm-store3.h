/* SPDX-License-Identifier: GPL-2.0-only
 * The App Store's v3 additions to the Linux guest contract (../guest/sbshm.h v2): written by sbstore (the guest's
 * store daemon, starbite/appstore/sbstore.py) under the SAME seqlock as struct sbshm_store (store->seq), read by
 * the Starbite OS store UI (shell_appstore.inc). Additive: a v2 reader (sbpkg-era UI) keeps working.
 *
 *   SBSHM_STORE_OFF       0x1020000  struct sbshm_store (v2; 64 + 160 x 248 bytes, ends at 0x10299C0)
 *   SBSHM_STORE_TEXT_OFF  0x102A000  the long descriptions: NUL-terminated UTF-8, pkg3.desc_off/len index it
 *   SBSHM_STORE3_OFF      0x1030000  struct sbshm_store3 (64 + 160 x 256 bytes, ends before the icon pool)
 *   SBSHM_ICON_OFF        0x1040000  the icon pool (v2)
 *
 * Ops (host -> guest, SBG_MSG_PKG): a = store index, b = op | (method << 8), c = the store seq it was read at.
 *   method 0 = the entry's default (pkg3.def_method); else SBG_PKG_M_*.
 *   SBG_PKG_OP_CHECK with a = SBSHM_NONE checks every installed app; with an index, every method of that app.
 */
#ifndef SBSHM_STORE3_H
#define SBSHM_STORE3_H
#include "../guest/sbshm.h"

#define SBSHM_STORE_TEXT_OFF   0x102A000U
#define SBSHM_STORE_TEXT_SIZE  0x6000U
#define SBSHM_STORE3_OFF       0x1030000U
#define SBSHM_STORE3_MAGIC     0x33525453U      /* "STR3" */

enum { SBG_PKG_OP_UPDATE = 4, SBG_PKG_OP_ROLLBACK = 5, SBG_PKG_OP_CHECK = 6 };
enum { SBG_PKG_M_DEFAULT = 0, SBG_PKG_M_DISTRO = 1, SBG_PKG_M_RELEASE = 2, SBG_PKG_M_SOURCE = 3 };
#define SBG_PKG_OPWORD(op, method) ((uint32_t)(op) | ((uint32_t)(method) << 8))
/* sbshm_pkg3.phase (sbshm_pkg.state stays INSTALLING for these, so v2 readers show a plain progress bar) */
enum { SBG_PKG_DOWNLOADING = 5, SBG_PKG_BUILDING = 6, SBG_PKG_VERIFYING = 7 };
/* more sbshm_pkg.flags */
enum {
    SBSHM_PKG_UPDATE = 1U << 5,     /* a newer version is known for the installed method */
    SBSHM_PKG_ROLLBACK = 1U << 6,   /* the previous version is kept: rollback possible */
    SBSHM_PKG_NEEDS_NET = 1U << 7,  /* the selected method needs the network, it is down, and the mirror lacks it */
};
/* sbshm_pkg3.needs */
enum { SBSHM_NEED_NETWORK = 1, SBSHM_NEED_AUDIO = 2, SBSHM_NEED_GPU = 4, SBSHM_NEED_CAMERA = 8,
       SBSHM_NEED_FILES = 16, SBSHM_NEED_KEYBOARD = 32 };
/* sbshm_pkg3.flags3 */
enum { SBSHM_PKG3_VERIFIED = 1, SBSHM_PKG3_MIRROR = 4, SBSHM_PKG3_TERMINAL = 8 };

/* sbshm_store3.net_why */
enum { SBSHM_NET_OK = 0, SBSHM_NET_NO_CONNECTION = 1, SBSHM_NET_NO_DNS = 2, SBSHM_NET_NO_ANSWER = 3,
       SBSHM_NET_OFFLINE_MODE = 4, SBSHM_NET_UNKNOWN = 5 };

struct sbshm_pkg3 {                 /* index = the store index */
    uint32_t methods;               /* bit (1 << SBG_PKG_M_*) for each method the app offers */
    uint32_t method;                /* installed with (0: not installed by the store) */
    uint32_t def_method;            /* preselected on the app page: installed method, the user's last pick, or the recipe's default */
    uint32_t phase;                 /* SBG_PKG_* incl. DOWNLOADING / BUILDING / VERIFYING */
    uint32_t done_kb, total_kb;     /* the current download */
    uint32_t eta_s;                 /* rough seconds left (0: unknown) */
    uint32_t build_min;             /* typical minutes to compile on the M3s (0: no source method) */
    uint32_t touch;                 /* 0 unknown, 1 poor, 2 ok, 3 good */
    uint32_t needs;                 /* SBSHM_NEED_* */
    uint32_t flags3;                /* SBSHM_PKG3_* */
    uint32_t desc_off, desc_len;    /* the long description in the text area */
    uint32_t hint_off;              /* after a failure: one plain sentence on what to do, in the text area */
    uint32_t rel_size_mb;           /* installed size of the release */
    uint32_t hint_len;              /* (0: no hint; was pad) */
    char ver_inst[24];              /* installed version ("" = not installed) */
    char ver_new[24];               /* newest known for the installed (or preselected) method */
    char ver_release[24], ver_distro[24], ver_source[24];   /* newest known per method ("" = unknown) */
    char licence[24];               /* SPDX */
    char source[48];                /* "github.com/mpv-player/mpv", "Debian (mpv)" */
};                                  /* 256 bytes */
struct sbshm_store3 {
    uint32_t magic;                 /* SBSHM_STORE3_MAGIC: v3 present */
    uint32_t version;               /* 1 */
    uint32_t n;                     /* = store->n */
    uint32_t net_ok;                /* the guest reaches the network */
    uint32_t mirror_ok;             /* the offline mirror is there */
    uint32_t checked_at;            /* unix time of the last update check (0: never; the guest's clock may be 1970) */
    uint32_t text_used;
    uint32_t net_why;               /* SBSHM_NET_*: why net_ok is 0 (was pad[0]) */
    uint32_t busy;                  /* 1 while a job runs or waits (incl. update checks): keep Linux up (was pad[1]) */
    uint32_t pad[7];
    struct sbshm_pkg3 pkg[SBSHM_PKGS_MAX];
};
_Static_assert(sizeof(struct sbshm_pkg3) == 256, "sbshm_pkg3");
_Static_assert(SBSHM_STORE_OFF + sizeof(struct sbshm_store) <= SBSHM_STORE_TEXT_OFF, "store / text");
_Static_assert(SBSHM_STORE3_OFF + sizeof(struct sbshm_store3) <= SBSHM_ICON_OFF, "store3 / icons");
#endif
