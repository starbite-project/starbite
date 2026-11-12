/* SPDX-License-Identifier: MIT
 * The offline TLS self-check's data (tls/tls_chains.c, made by tls/mk-selftest.sh) and the check itself
 * (tls/tls_check.c): BearSSL's hashes, HMAC, AES-GCM and ECDH P-256 against known answers, and two
 * real certificate chains through the X.509 engine with the trust anchors in tls/ta.c. No network. */
#ifndef SBTLS_SELFTEST_H
#define SBTLS_SELFTEST_H
#include <stdint.h>
struct sbtls_cert { const unsigned char *der; unsigned len; };
struct sbtls_chain { const char *host; const struct sbtls_cert *certs; unsigned n; int ecdsa; };
extern const struct sbtls_chain sbtls_selftest_chains[2];
extern const uint64_t sbtls_selftest_time;
/* 0 passed, else the number of the first check that failed; log (optional) gets one line per check */
int sbtls_selfcheck(void (*log)(void *ctx, const char *line), void *ctx, uint64_t (*now_ms)(void *ctx));
#endif
