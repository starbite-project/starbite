/* SPDX-License-Identifier: MIT
 * SbWifiCx's frame helpers, shared with the host test (host/test.c): WiFiCx's data path carries 802.11 data frames
 * with an LLC/SNAP header, the radio (msgbuf) Ethernet frames; and the EAPOL-Key message classification for the log.
 * Plain C (memcpy/memset only), no addresses kept. */
#ifndef SB_DOT11_H
#define SB_DOT11_H
#include <stdint.h>
#include <string.h>

// ---- 802.11 <-> Ethernet (W2l): WiFiCx's data path carries 802.11 data frames with an LLC/SNAP header, as WDI's
// did (W2j-a: Windows' first frame was 60 bytes = an ARP in 802.11 form, read as Ethernet its "ethertype" was
// e1f5, two bytes of an address; Windows counted 0 bytes received from the Ethernet frames handed up). The radio
// (msgbuf) takes and gives Ethernet frames.

// the 802.11 data header's length, or 0 when this is not a data frame with an LLC/SNAP header
static inline uint32_t Dot11Header(const uint8_t *f, uint32_t len)
{
    if (len < 24 + 8 || (f[0] & 0x0c) != 0x08)
        return 0;                               // not a data frame
    uint32_t h = 24;
    if ((f[1] & 3) == 3)
        h += 6;                                 // four addresses
    if (f[0] & 0x80) {
        h += 2;                                 // QoS control
        if (f[1] & 0x80)
            h += 4;                             // HT control (order bit on a QoS frame)
    }
    if (len < h + 8 || f[h] != 0xaa || f[h + 1] != 0xaa || f[h + 2] != 0x03)
        return 0;
    return h;
}

// an 802.11 data frame (to the AP) as the Ethernet frame the radio takes; 0 if it cannot be
static inline uint32_t Dot11ToEth(const uint8_t *f, uint32_t len, uint8_t *out)
{
    uint32_t h = Dot11Header(f, len);
    if (!h || (f[1] & 0x40))
        return 0;                               // not convertible, or already protected
    const uint8_t *da, *sa;
    switch (f[1] & 3) {
    case 1: da = f + 16, sa = f + 10; break;    // to the DS: A1 BSSID, A2 SA, A3 DA
    case 2: da = f + 4, sa = f + 16; break;     // from the DS
    case 3: da = f + 16, sa = f + 24; break;
    default: da = f + 4, sa = f + 10; break;
    }
    memcpy(out, da, 6);
    memcpy(out + 6, sa, 6);
    out[12] = f[h + 6], out[13] = f[h + 7];     // the ethertype after the SNAP header
    memcpy(out + 14, f + h + 8, len - h - 8);
    return 14 + len - h - 8;
}

// an Ethernet frame from the radio as an 802.11 data frame from the AP (FromDS: A1 DA, A2 BSSID, A3 SA)
static inline uint32_t EthToDot11(const uint8_t *e, uint32_t len, const uint8_t *bssid, uint8_t *out, uint32_t cap)
{
    if (len < 14 || len - 14 + 32 > cap)
        return 0;
    uint16_t t = (uint16_t)(e[12] << 8 | e[13]);
    memset(out, 0, 32);
    out[0] = 0x08;                              // data
    out[1] = 0x02;                              // from the DS
    memcpy(out + 4, e, 6);
    memcpy(out + 10, bssid, 6);
    memcpy(out + 16, e + 6, 6);
    out[24] = 0xaa, out[25] = 0xaa, out[26] = 0x03;
    if (t == 0x80f3 || t == 0x8137)
        out[29] = 0xf8;                         // bridge tunnel encapsulation (802.1H)
    out[30] = e[12], out[31] = e[13];
    memcpy(out + 32, e + 14, len - 14);
    return 32 + len - 14;
}

// an EAPOL-Key frame's key info and which of the four-way handshake's messages it is (0: not a key frame)
static inline int EapolKey(const uint8_t *f, uint32_t len, uint16_t *info)
{
    if (len < 14 + 4 + 3 || f[12] != 0x88 || f[13] != 0x8e)
        return -1;                                  // not EAPOL
    if (f[15] != 3)
        return 0;                                   // EAPOL, not a key frame
    uint16_t ki = (uint16_t)(f[19] << 8 | f[20]);
    *info = ki;
    if (!(ki & 0x0008))
        return 5;                                   // group key handshake
    if ((ki & 0x0080) && !(ki & 0x0100))
        return 1;
    if ((ki & 0x0080) && (ki & 0x0040))
        return 3;
    if (!(ki & 0x0080) && (ki & 0x0100))
        return (ki & 0x0200) ? 4 : 2;
    return 0;
}

#endif
