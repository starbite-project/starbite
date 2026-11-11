/* SPDX-License-Identifier: MIT
 * BearSSL's <string.h> in freestanding builds (Starbite OS, libsb test images): the five functions it uses, which
 * libsb provides. Host builds use the C library's own header instead. */
#ifndef SB_BEARSSL_STRING_H
#define SB_BEARSSL_STRING_H
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif
