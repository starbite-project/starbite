/* SPDX-License-Identifier: GPL-2.0-only
 * The macro pad's buttons from /starbite/macros.toml (macros.c). */
#pragma once
#include <stdint.h>

struct macro {
    char label[16];
    uint8_t mod, key;          /* a key combination, or */
    char text[48];             /* text to type (US layout) */
};

/* Parse up to max macros from src (the file's bytes); returns how many. */
int macros_parse(const char *src, unsigned len, struct macro *out, int max);
