/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Display: the monitor's identity (EDID) and mode list. Plain C++17, no Windows headers, so the same
 * tables are checked on the Mac by test/modes-test.cpp (EDID checksum, decoding, mode timings).
 *
 * The phone draws in software (no GPU driver, 4 A55 cores), so the preferred mode is 1920x1080 at 30 Hz.
 * Timings are the real CTA/DMT ones (1080p: 2200x1125, 720p: 1650x750, 1600x900 reduced blanking:
 * 1800x1000), so the pixel rates the OS sees are sensible; for an indirect display they only rank modes.
 */
#ifndef SB_IDD_MODES_H
#define SB_IDD_MODES_H

#include <stdint.h>

namespace sbidd {

struct Mode {
    uint32_t w, h;      // active
    uint32_t tw, th;    // total (active + blanking)
    uint32_t hz;        // vertical refresh
};

// Index 0 is the preferred mode (also the EDID's first detailed timing).
static constexpr Mode kModes[] = {
    {1920, 1080, 2200, 1125, 30},
    {1920, 1080, 2200, 1125, 60},
    {1600,  900, 1800, 1000, 30},
    {1600,  900, 1800, 1000, 60},
    {1280,  720, 1650,  750, 30},
    {1280,  720, 1650,  750, 60},
};
static constexpr unsigned kModeCount = sizeof(kModes) / sizeof(kModes[0]);
static constexpr unsigned kPreferredMode = 0;

constexpr uint64_t PixelRate(const Mode &m) { return uint64_t(m.tw) * m.th * m.hz; }
constexpr uint32_t HSyncHz(const Mode &m) { return m.th * m.hz; }

// EDID 1.4, 128 bytes, no extension. Manufacturer "SBT" (not a registered PNP id; only this driver uses it),
// product 0x5B01, serial 1, week 45 of 2026. Display product name descriptor: the EDID allows 13 characters, so
// the name is "Starbite"; the adapter and endpoint carry the full "Starbite Display" (driver.cpp, INF).
// Byte 127 (checksum) is filled in by EdidWithChecksum() so the table stays readable.
static constexpr uint8_t kEdidNoSum[128] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,   // header
    0x4C, 0x54,                                       // "SBT": 0 10011 00010 10100
    0x01, 0x5B,                                       // product code 0x5B01 (LE)
    0x01, 0x00, 0x00, 0x00,                           // serial 1
    45, 2026 - 1990,                                  // week, year
    0x01, 0x04,                                       // EDID 1.4
    0xA0,                                             // digital input, 8 bits per colour, interface undefined
    53, 30,                                           // 53 x 30 cm (24", 16:9: 1080p at 100% scaling)
    120,                                              // gamma 2.2
    0x06,                                             // RGB 4:4:4, sRGB default, preferred timing is native
    0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54,   // sRGB chromaticity
    0x00, 0x00, 0x00,                                 // no established timings
    // standard timings: 1920x1080@60, 1600x900@60, 1280x720@60 (16:9 = 0xC0 | (hz - 60)), rest unused
    0xD1, 0xC0, 0xA9, 0xC0, 0x81, 0xC0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // descriptor 1: detailed timing 1920x1080 @ 30 Hz (CTA VIC 34): 74.25 MHz, H 1920+280 (88/44), V 1080+45
    // (4/5), +H +V, image 531 x 299 mm
    0x01, 0x1D, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0x13, 0x2B, 0x21, 0x00, 0x00, 0x1E,
    // descriptor 2: detailed timing 1920x1080 @ 60 Hz (CTA VIC 16): 148.5 MHz, same blanking
    0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0x13, 0x2B, 0x21, 0x00, 0x00, 0x1E,
    // descriptor 3: range limits: V 24-75 Hz, H 15-80 kHz, 150 MHz max, range limits only (no GTF/CVT)
    0x00, 0x00, 0x00, 0xFD, 0x00, 24, 75, 15, 80, 15, 0x01, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    // descriptor 4: display product name "Starbite" (LF-terminated, space-padded to 13)
    0x00, 0x00, 0x00, 0xFC, 0x00, 'S', 't', 'a', 'r', 'b', 'i', 't', 'e', 0x0A, 0x20, 0x20, 0x20, 0x20,
    0x00,                                             // no extension blocks
    0x00,                                             // checksum (filled in below)
};

struct Edid {
    uint8_t b[128];
};

constexpr Edid EdidWithChecksum()
{
    Edid e{};
    unsigned sum = 0;
    for (unsigned i = 0; i < 127; i++) {
        e.b[i] = kEdidNoSum[i];
        sum += kEdidNoSum[i];
    }
    e.b[127] = uint8_t(0x100 - (sum & 0xFF)) & 0xFF;
    return e;
}

static constexpr Edid kEdid = EdidWithChecksum();

constexpr bool EdidSumOk(const Edid &e)
{
    unsigned s = 0;
    for (unsigned i = 0; i < 128; i++)
        s += e.b[i];
    return (s & 0xFF) == 0;
}
static_assert(EdidSumOk(kEdid), "EDID checksum");
static_assert(sizeof(kEdidNoSum) == 128, "EDID size");

}  // namespace sbidd

#endif
