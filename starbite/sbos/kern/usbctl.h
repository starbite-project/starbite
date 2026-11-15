/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: USB Control (Track A, 9f; the notes' usb-control-proposal.md). The USB console (usb.c's CDC-ACM port)
 * answers nothing but the handshake until a computer proves it holds a key the user approved:
 *
 *   phone -> host  "SBCTL1 <nonce: 64 hex> <phone id: 16 hex>"      when the port opens (and on "hello")
 *   host -> phone  "AUTH <public key: 130 hex, P-256 uncompressed 04|X|Y> <signature: DER, hex>"
 *                  the signature (ECDSA P-256, SHA-256) over "sbctl1|<nonce>|<phone id>|<public key hex>"
 *   phone -> host  "OK <fingerprint>" (a session), "PENDING ..." (a new key: the question is on the unlocked phone),
 *                  "REFUSED <why>" or "WAIT <s>" (the back-off after 3 bad signatures: 60 s, doubling to 15 min)
 *
 * The phone keeps public keys only (at most USBCTL_KEYS, prefs blob "usbctl.keys"): a copied card gives no access.
 * A new key is approved only on an unlocked phone, with the usual hold; on a locked phone it is refused, and the next
 * unlock's notification says a computer tried. A nonce is used once: a recorded session cannot be replayed. A session
 * ends when the port closes, the cable goes, after 30 minutes without a command, or on "bye".
 * Settings > Privacy > USB Control: the switch (pref dev.usb, 8b's Developer switch; off until the user turns it on: the
 * USB link is not even started before) and the approved computers (Revoke).
 * Never reachable here: the passcode, the settings and prefs, Notes, Contacts, Photos, Messages, Wi-Fi passwords, any
 * path on the card; no unlock, no touches or keys, no screenshots, no calls. */
#ifndef SBOS_USBCTL_H
#define SBOS_USBCTL_H
#include <stdint.h>

#define USBCTL_KEYS 4
#define USBCTL_KEY_LEN 65                   /* 04 | X | Y */
#define USBCTL_LINE_MAX 400                 /* an AUTH line: 6 + 130 + 1 + up to 144 hex + slack */
#define USBCTL_IDLE_MS (30U * 60U * 1000U)

typedef void (*usbctl_put_fn)(const char *s);

/* usb.c's console */
void usbctl_open(usbctl_put_fn put);        /* a terminal opened the port: the challenge */
void usbctl_close(const char *why);         /* the port closed or the cable went: the session ends */
int usbctl_authed(void);
void usbctl_line(const char *line, usbctl_put_fn put);   /* before the session: the handshake */
int usbctl_exec(const char *line, usbctl_put_fn put);    /* in a session: 1 handled here, 0 the console's own */
void usbctl_tick(uint64_t now, usbctl_put_fn put);       /* the deferred restart, the pairing answer, idle */

/* Settings (kern_next.c's sh_usbctl_*) */
int usbctl_enabled(void);
void usbctl_set_enabled(int on);            /* the link with it (usb_enable) */
int usbctl_key_count(void);
void usbctl_key_fp(int i, char out[17]);    /* the first 8 bytes of SHA-256(key), hex */
int usbctl_revoke(int i);                   /* 0 done; a session of that key ends */
int usbctl_session(char fp[17]);            /* 1 a session is open (its key's fingerprint) */
void usbctl_init(void);                     /* main.c: the link when the switch is on */

/* pure, for the host test: the signed text, and the verdict on an AUTH line against a nonce (1 a valid signature;
 * *key the 65 bytes) */
int usbctl_signed_text(char *out, unsigned cap, const char *nonce_hex, const char *id_hex, const char *key_hex);
int usbctl_check_auth(const char *line, const char *nonce_hex, const char *id_hex, uint8_t key[USBCTL_KEY_LEN]);
#endif
