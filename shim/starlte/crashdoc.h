/* SPDX-License-Identifier: GPL-2.0-only
 * The crash report as a document, and its keys (crashdoc.c, docs/STARBITE-HEALTH.md 4.4). No
 * hardware: the host test builds the report from real captures and drives the keys with real
 * timing (tools/loader-test). crash.c draws it (fbtext.c star_fb_mini) and polls the keys.
 *
 * The document is a list of rows (at most CD_COLS characters each, pointing into the report's
 * text or into a buffer of composed lines) in sections: the header, PANIC MESSAGE, CONTEXT,
 * REGISTERS, CALL TRACE, RESET AND WATCHDOG REGISTERS, LOG, OTHER RING. Every level ends with the
 * complete log, so the report always scrolls; the levels choose the sections above it:
 *   1 summary     header, message, the call trace's first 12 lines
 *   2 details     + context, registers, the whole call trace
 *   3 everything  + all 16 saved registers, the other ramoops ring (Linux)
 */
#pragma once
#include <stdint.h>
#include "sbfmt.h"

#define CD_COLS 76U                     /* characters a row */
#define CD_SECT_MAX 12U

enum { CD_WHITE, CD_MUTED, CD_GOLD, CD_RED, CD_GREEN, CD_LOG, CD_CYAN };   /* row colours */

struct cd_row {
    uint64_t p;                         /* the row's text (not NUL-terminated) */
    uint16_t n;                         /* its length */
    uint8_t color, sect, indent, pad[3];
};

struct cd_doc {
    struct cd_row *rows;                /* the caller's buffers */
    unsigned max;
    char *txt;
    unsigned txt_cap;
    uint8_t *kinds;
    unsigned kinds_max;
    /* the result */
    unsigned n;                         /* rows */
    unsigned txt_n, nsect, sect;
    unsigned sect_row[CD_SECT_MAX];     /* each section's first row (its blank row before the title) */
    unsigned log_row, log_lines;        /* where the LOG section starts, its lines */
    struct sb_crashinfo ci;             /* what the parser found */
};

struct cd_shown {
    const char *text;                   /* the text the sections and the log come from */
    unsigned n;
    int live;                           /* 1: everything captured this boot; 0: the saved excerpt */
    const char *other;                  /* Linux: the other ramoops zone, or 0 */
    unsigned other_n, other_src;
    int slot;                           /* the report's slot in the crash log, -1 not saved */
};

/* Build the report at level 1..3 into d (its buffers set by the caller). */
void cd_build(struct cd_doc *d, const struct cl_report *r, unsigned level, const struct cd_shown *sh);

/* ---- the keys: a line, a page / section, the level, continue ----------------------------------
 * Volume Up / Down: one line once held CS_FIRST_MS, again after CS_REPEAT_MS, then every
 * CS_RATE_MS; held CS_FAST_MS: CS_FAST_STEP lines a step. Bixby: released before CS_LEVEL_MS =
 * a page on, stopping at the next section's start (at the end: back to the top); held CS_LEVEL_MS
 * = the next detail level (CS_EV_LEVEL: the caller rebuilds and calls cs_doc). Power held
 * CS_FIRST_MS: continue. Times in ms (any origin; 0 is allowed). */
#define CS_K_UP     1U                  /* = hw.h K_UP, K_DOWN, K_BIXBY, K_POWER */
#define CS_K_DOWN   2U
#define CS_K_BIXBY  4U
#define CS_K_POWER  8U
#define CS_FIRST_MS   20U
#define CS_REPEAT_MS  300U
#define CS_RATE_MS    35U
#define CS_FAST_MS    1500U
#define CS_FAST_STEP  8U
#define CS_LEVEL_MS   700U
struct cs_nav {
    unsigned top, rows, page, nsect;
    unsigned sect[CD_SECT_MAX];
    int dir;                            /* the Volume key held: -1 up, 1 down, 0 none */
    uint64_t dir_t, rep_t, bix_t, pow_t;   /* ms + 1 when it went down, 0 = up */
    uint8_t first, bix_long;
};
#define CS_EV_MOVED 1U                  /* top changed: redraw */
#define CS_EV_KEY   2U                  /* a key acted: the screen's timeout starts again */
#define CS_EV_LEVEL 4U                  /* Bixby held: the next detail level */
#define CS_EV_EXIT  8U                  /* Power */
void cs_init(struct cs_nav *n, unsigned page);
/* The document changed (built, or rebuilt for another level): rows, sections; top is clamped. */
void cs_doc(struct cs_nav *n, const struct cd_doc *d);
unsigned cs_max_top(const struct cs_nav *n);
unsigned cs_page_on(const struct cs_nav *n, unsigned top);
unsigned cs_poll(struct cs_nav *n, unsigned keys, uint64_t now_ms);
