/* SPDX-License-Identifier: GPL-2.0-only
 * The loader's text buffer (shim/starlte/fastboot.h), which its USB drivers use for diagnostics. */
#pragma once
#include <stdint.h>

struct tb { char *b; unsigned n, cap; };
void tb_init(struct tb *t, char *buf, unsigned cap);
void tb_c(struct tb *t, char c);
void tb_s(struct tb *t, const char *s);
void tb_u(struct tb *t, uint64_t v);
void tb_hex(struct tb *t, uint64_t v, unsigned digits);     /* "0x" and digits */
void tb_reg(struct tb *t, const char *name, uint32_t v);    /* "name 0x12345678\n" */
