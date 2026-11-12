/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw: SAE (WPA3-Personal's authentication, IEEE 802.11-2020 12.4) for group 19 (NIST P-256), on the host.
 *
 * sae.c does the arithmetic with BearSSL (tls/bearssl: br_ec_all_m31's P-256, br_i31 modular integers, SHA-256,
 * HMAC); this header does not need BearSSL, so link.c can carry the state without it. A build that links sae.c
 * calls bcmw_sae_enable() once after bcmw_init(); without it (the Windows drivers, the host preview) the core
 * refuses WPA3-only networks with a log line and joins transition networks with WPA2-PSK.
 *
 * Both ways to the password element: hunting-and-pecking (12.4.4.2.2: 40 rounds at least, the found round's
 * values picked by constant-time selection, the quadratic-residue test blinded) and hash-to-element (12.4.4.2.3:
 * PT by SSWU from the network's name and the password, then PWE = val * PT). Frames are the Authentication
 * frame's body from the algorithm number on (algorithm 3, sequence 1 commit / 2 confirm, status).
 * Nothing here logs; the password, PWE, rand, KCK and PMK are wiped by bcmw_sae_clear().
 */
#ifndef BCMW_SAE_H
#define BCMW_SAE_H
#include <stdint.h>

#define BCMW_SAE_GROUP          19
#define BCMW_SAE_FRAME_MOST     400     /* a commit with an anti-clogging token of up to 256 bytes */
#define BCMW_SAE_TOKEN_MOST     256

/* status codes (IEEE 802.11-2020 9.4.1.9) used by SAE */
#define BCMW_SAE_ST_OK                  0
#define BCMW_SAE_ST_UNSPECIFIED         1
#define BCMW_SAE_ST_TOKEN_REQUIRED      76      /* ANTI_CLOGGING_TOKEN_REQUIRED */
#define BCMW_SAE_ST_GROUP_REFUSED       77      /* FINITE_CYCLIC_GROUP_NOT_SUPPORTED */
#define BCMW_SAE_ST_UNKNOWN_ID          123     /* UNKNOWN_PASSWORD_IDENTIFIER */
#define BCMW_SAE_ST_H2E                 126     /* SAE_HASH_TO_ELEMENT */

struct bcmw_sae {
    /* the caller's */
    int (*random)(void *ctx, uint8_t *out, uint32_t n);     /* 0 on success; never NULL */
    void *random_ctx;
    /* sae.c's */
    uint8_t own[6], peer[6];
    int h2e;                        /* the PWE by hash-to-element (commit status 126) */
    int state;                      /* BCMW_SAE_* below */
    uint8_t pwe[64];                /* x || y */
    uint8_t rand[32];
    uint8_t scalar[32], element[64];            /* our commit */
    uint8_t peer_scalar[32], peer_element[64];  /* the peer's */
    uint8_t kck[32], pmk[32], pmkid[16];
    uint16_t send_confirm;
    uint8_t token[BCMW_SAE_TOKEN_MOST];         /* the anti-clogging token the peer asked us to send */
    unsigned token_len, token_rounds;
    int bad_confirm;                /* the exchange failed on the peer's confirm (a wrong password gives this) */
};
enum { BCMW_SAE_NOTHING, BCMW_SAE_COMMITTED, BCMW_SAE_CONFIRMED, BCMW_SAE_ACCEPTED };

/* bcmw_sae_sta_rx's results */
enum {
    BCMW_SAE_FAIL = -1,     /* the exchange failed: *why says why; send nothing more */
    BCMW_SAE_IGNORE = 0,    /* not for us / dropped (*why may say why) */
    BCMW_SAE_SEND = 1,      /* send the frame in out */
    BCMW_SAE_DONE = 2,      /* the peer's confirm verified: pmk and pmkid are ready (nothing to send) */
};

/* The functions, through a table so that link.c does not link them by name (see above). */
struct bcmw_sae_ops {
    /* H2E's PT (x || y) of a network's name and a password (and a password identifier, or NULL) */
    int (*pt)(const uint8_t *ssid, unsigned ssid_len, const uint8_t *pw, unsigned pw_len, const uint8_t *id,
              unsigned id_len, uint8_t pt[64]);
    /* the PWE for this pair of addresses: from pt (hash-to-element) or, pt NULL, by hunting-and-pecking from the
     * password; then a fresh commit (rand, mask). 0 or -1. */
    int (*prepare)(struct bcmw_sae *s, const uint8_t own[6], const uint8_t peer[6], const uint8_t *pw, unsigned pw_len,
                   const uint8_t *pt);
    /* our commit frame (with the anti-clogging token if one was asked for); its length */
    unsigned (*commit_frame)(const struct bcmw_sae *s, uint8_t *out, unsigned cap);
    /* the peer's commit (the frame from the algorithm number on): 0 taken (KCK, PMK, PMKID made), -2 a reflection
     * of our own (to be dropped silently), else the status code to refuse it with */
    int (*peer_commit)(struct bcmw_sae *s, const uint8_t *frame, unsigned len);
    /* our confirm frame (send-confirm counted up); its length */
    unsigned (*confirm_frame)(struct bcmw_sae *s, uint8_t *out, unsigned cap);
    /* the peer's confirm frame: 0 it verifies, -1 not */
    int (*peer_confirm)(struct bcmw_sae *s, const uint8_t *frame, unsigned len);
    /* the station's side: one received frame in, at most one frame out */
    int (*sta_rx)(struct bcmw_sae *s, const uint8_t *frame, unsigned len, uint8_t *out, unsigned *out_len,
                  const char **why);
    void (*clear)(struct bcmw_sae *s);
};

/* sae.c (links BearSSL) */
const struct bcmw_sae_ops *bcmw_sae_get_ops(void);
struct bcmw;
void bcmw_sae_enable(struct bcmw *w);           /* after bcmw_init: WPA3 joins with the host's SAE */
/* for the host test: a commit from given rand and mask (the standard's examples) instead of random ones */
int bcmw_sae_commit_fixed(struct bcmw_sae *s, const uint8_t rand[32], const uint8_t mask[32]);

#endif
