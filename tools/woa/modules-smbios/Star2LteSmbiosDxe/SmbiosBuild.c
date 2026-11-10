/** @file
  The Galaxy S9's SMBIOS records (SMBIOS 3.3 layouts; offsets from DSP0134):

    type 0    firmware   "Starbite", the image's version and date, UEFI supported
    type 1    system     Samsung Galaxy S9, SM-G960F, family "Galaxy S"; no serial number, no UUID
    type 3    enclosure  Samsung, handheld
    type 7    caches     only with Type7: L1 data and L1 instruction, 32 KiB 4-way (per core),
                         L2 unified 512 KiB 16-way (shared by the A55 cluster); no L3. The geometry
                         Star2LteSsdtDxe measured from CLIDR/CCSIDR on this phone (docs/WIN11-USB.md
                         section 10)
    type 4    processor  "Samsung Exynos 9810", ARMv8, MIDR_EL1; the cores Windows runs today: the
                         four Cortex-A55 (numproc 4; the M3 cluster is parked), so 4 cores, 4 enabled,
                         4 threads; 1794 MHz max (the A55's rated clock), 1690 MHz current
                         (sbthermal's default; S-Boot leaves 1053), 26 MHz external clock
    type 127  end of table

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "SmbiosBuild.h"

typedef struct {
  sb_u8   *Buf;
  sb_u32  Cap;
  sb_u32  Len;
  sb_u32  Start;                         // the open record
  sb_u16  Count;
  sb_u16  MaxSize;
  int     Overflow;
} SB_TABLE;

static void
Put8 (SB_TABLE *T, sb_u32 Off, sb_u8 V)
{
  if (T->Start + Off < T->Cap) {
    T->Buf[T->Start + Off] = V;
  }
}

static void
Put16 (SB_TABLE *T, sb_u32 Off, sb_u16 V)
{
  Put8 (T, Off, (sb_u8)V);
  Put8 (T, Off + 1, (sb_u8)(V >> 8));
}

static void
Put32 (SB_TABLE *T, sb_u32 Off, sb_u32 V)
{
  Put16 (T, Off, (sb_u16)V);
  Put16 (T, Off + 2, (sb_u16)(V >> 16));
}

/* Opens a record of Len formatted bytes (zeroed) with the next handle; returns the handle. */
static sb_u16
Begin (SB_TABLE *T, sb_u8 Type, sb_u8 Len)
{
  sb_u32  i;
  sb_u16  Handle = T->Count;

  T->Start = T->Len;
  if (T->Len + Len + 2 > T->Cap) {
    T->Overflow = 1;
    return 0xFFFF;
  }

  for (i = 0; i < Len; i++) {
    T->Buf[T->Len + i] = 0;
  }

  Put8 (T, 0, Type);
  Put8 (T, 1, Len);
  Put16 (T, 2, Handle);
  T->Len += Len;
  return Handle;
}

/* Closes the open record with its strings (NULL-terminated list; none = the two NULs). */
static void
End (SB_TABLE *T, const char *const *Strings)
{
  sb_u32  n = 0;

  if (T->Overflow) {
    return;
  }

  for ( ; Strings != 0 && *Strings != 0; Strings++, n++) {
    const char  *s = *Strings;
    do {
      if (T->Len >= T->Cap) {
        T->Overflow = 1;
        return;
      }

      T->Buf[T->Len++] = (sb_u8)*s;
    } while (*s++ != 0);
  }

  if (n == 0) {
    if (T->Len >= T->Cap) {
      T->Overflow = 1;
      return;
    }

    T->Buf[T->Len++] = 0;
  }

  if (T->Len >= T->Cap) {
    T->Overflow = 1;
    return;
  }

  T->Buf[T->Len++] = 0;
  T->Count++;
  if (T->Len - T->Start > T->MaxSize) {
    T->MaxSize = (sb_u16)(T->Len - T->Start);
  }
}

static sb_u16
Cache (SB_TABLE *T, const char *Name, sb_u16 Level, sb_u16 KiB, sb_u8 Type, sb_u8 Assoc)
{
  const char  *s[2];                    // assigned, not initialised: no memcpy in the firmware build
  sb_u16      h = Begin (T, 7, 0x1B);

  s[0] = Name;
  s[1] = 0;

  Put8 (T, 0x04, 1);                                    // socket designation
  Put16 (T, 0x05, (sb_u16)((Level - 1) | 0x80 | 0x100)); // level, internal, enabled, write-back
  Put16 (T, 0x07, KiB);                                 // maximum size, 1 KiB granularity
  Put16 (T, 0x09, KiB);                                 // installed size
  Put16 (T, 0x0B, 0x0002);                              // supported SRAM type: unknown
  Put16 (T, 0x0D, 0x0002);                              // current SRAM type: unknown
  Put8 (T, 0x10, 0x02);                                 // error correction: unknown
  Put8 (T, 0x11, Type);                                 // 3 instruction, 4 data, 5 unified
  Put8 (T, 0x12, Assoc);                                // 5 4-way, 8 16-way
  Put32 (T, 0x13, KiB);                                 // maximum size 2
  Put32 (T, 0x17, KiB);                                 // installed size 2
  End (T, s);
  return h;
}

sb_u32
SbSmbiosBuild (
  sb_u8   *Buf,
  sb_u32  Cap,
  sb_u32  Midr,
  int     Type7,
  sb_u16  *Count,
  sb_u16  *MaxSize
  )
{
  SB_TABLE  T;
  sb_u16    L1 = 0xFFFF, L2 = 0xFFFF;

  T.Buf      = Buf;
  T.Cap      = Cap;
  T.Len      = 0;
  T.Start    = 0;
  T.Count    = 0;
  T.MaxSize  = 0;
  T.Overflow = 0;

  {   // type 0: firmware
    static const char *const  s[] = { "Starbite", "Starbite UEFI (exynos9810-woa 22621.7582) t32s", "11/10/2026", 0 };
    Begin (&T, 0, 0x1A);
    Put8 (&T, 0x04, 1);                  // vendor
    Put8 (&T, 0x05, 2);                  // version
    Put16 (&T, 0x06, 0);                 // no legacy segment on Arm
    Put8 (&T, 0x08, 3);                  // release date
    Put8 (&T, 0x09, 0x6F);               // 64 KiB * (n + 1) = 7 MiB, the FD
    Put32 (&T, 0x0A, 1U << 3);           // characteristics: "not supported" (bit 3)
    Put8 (&T, 0x13, 1U << 3);            // extension byte 2: UEFI is supported
    Put8 (&T, 0x14, 1);                  // firmware release 1.0
    Put8 (&T, 0x15, 0);
    Put8 (&T, 0x16, 0xFF);               // no embedded controller
    Put8 (&T, 0x17, 0xFF);
    End (&T, s);
  }

  {   // type 1: system (no serial number, UUID all zero = not present)
    static const char *const  s[] = { "Samsung", "Galaxy S9", "SM-G960F", "Galaxy S", 0 };
    Begin (&T, 1, 0x1B);
    Put8 (&T, 0x04, 1);                  // manufacturer
    Put8 (&T, 0x05, 2);                  // product name
    Put8 (&T, 0x06, 3);                  // version
    Put8 (&T, 0x18, 0x06);               // wake-up type: power switch
    Put8 (&T, 0x19, 3);                  // SKU
    Put8 (&T, 0x1A, 4);                  // family
    End (&T, s);
  }

  {   // type 3: enclosure, no contained elements
    static const char *const  s[] = { "Samsung", "SM-G960F", 0 };
    Begin (&T, 3, 0x16);
    Put8 (&T, 0x04, 1);                  // manufacturer
    Put8 (&T, 0x05, 0x0B);               // hand held
    Put8 (&T, 0x06, 2);                  // version
    Put8 (&T, 0x09, 0x03);               // boot-up state: safe
    Put8 (&T, 0x0A, 0x03);               // power supply: safe
    Put8 (&T, 0x0B, 0x03);               // thermal: safe
    Put8 (&T, 0x0C, 0x03);               // security: none
    Put8 (&T, 0x12, 1);                  // power cords
    End (&T, s);
  }

  if (Type7) {
    L1 = Cache (&T, "L1 Data", 1, 32, 4, 5);
    (void)Cache (&T, "L1 Instruction", 1, 32, 3, 5);
    L2 = Cache (&T, "L2", 2, 512, 5, 8);
  }

  {   // type 4: processor (the SMBIOS 3.0 length)
    static const char *const  s[] = { "SoC", "Samsung", "Samsung Exynos 9810", 0 };
    Begin (&T, 4, 0x30);
    Put8 (&T, 0x04, 1);                  // socket
    Put8 (&T, 0x05, 0x03);               // central processor
    Put8 (&T, 0x06, 0xFE);               // family: see family 2
    Put8 (&T, 0x07, 2);                  // manufacturer
    Put32 (&T, 0x08, Midr);              // processor ID on Arm: MIDR_EL1, then the SoC ID (none)
    Put8 (&T, 0x10, 3);                  // version: the name Windows shows
    Put16 (&T, 0x12, 26);                // external clock, MHz
    Put16 (&T, 0x14, 1794);              // max speed
    Put16 (&T, 0x16, 1690);              // current speed
    Put8 (&T, 0x18, 0x41);               // socket populated, CPU enabled
    Put8 (&T, 0x19, 0x06);               // upgrade: none
    Put16 (&T, 0x1A, L1);
    Put16 (&T, 0x1C, L2);
    Put16 (&T, 0x1E, 0xFFFF);            // no L3
    Put8 (&T, 0x23, 4);                  // cores
    Put8 (&T, 0x24, 4);                  // enabled cores
    Put8 (&T, 0x25, 4);                  // threads
    Put16 (&T, 0x26, (1U << 2) | (1U << 3) | (1U << 5)); // 64-bit, multi-core, execute protection
    Put16 (&T, 0x28, 0x0101);            // family 2: ARMv8
    Put16 (&T, 0x2A, 4);
    Put16 (&T, 0x2C, 4);
    Put16 (&T, 0x2E, 4);
    End (&T, s);
  }

  Begin (&T, 127, 4);                    // end of table
  End (&T, 0);

  if (T.Overflow) {
    return 0;
  }

  *Count   = T.Count;
  *MaxSize = T.MaxSize;
  return T.Len;
}

static sb_u8
Sum (const sb_u8 *p, sb_u32 n)
{
  sb_u8  s = 0;

  while (n--) {
    s = (sb_u8)(s + *p++);
  }

  return s;
}

static void
Zero (sb_u8 *p, sb_u32 n)
{
  while (n--) {
    *p++ = 0;
  }
}

void
SbSmbiosEp3 (
  sb_u8   *Ep,
  sb_u64  TableAddr,
  sb_u32  TableLen
  )
{
  sb_u32  i;

  Zero (Ep, SB_EP3_LEN);
  Ep[0] = '_';
  Ep[1] = 'S';
  Ep[2] = 'M';
  Ep[3] = '3';
  Ep[4] = '_';
  Ep[6] = SB_EP3_LEN;
  Ep[7] = SB_SMBIOS_MAJOR;
  Ep[8] = SB_SMBIOS_MINOR;
  Ep[9] = 0;                             // docrev
  Ep[10] = 1;                            // entry point revision: 3.0
  for (i = 0; i < 4; i++) {
    Ep[12 + i] = (sb_u8)(TableLen >> (8 * i));       // table maximum size
  }

  for (i = 0; i < 8; i++) {
    Ep[16 + i] = (sb_u8)(TableAddr >> (8 * i));
  }

  Ep[5] = (sb_u8)(0 - Sum (Ep, SB_EP3_LEN));
}

void
SbSmbiosEp21 (
  sb_u8   *Ep,
  sb_u32  TableAddr,
  sb_u16  TableLen,
  sb_u16  Count,
  sb_u16  MaxSize
  )
{
  sb_u32  i;

  Zero (Ep, SB_EP21_LEN);
  Ep[0] = '_';
  Ep[1] = 'S';
  Ep[2] = 'M';
  Ep[3] = '_';
  Ep[5] = SB_EP21_LEN;
  Ep[6] = SB_SMBIOS_MAJOR;
  Ep[7] = SB_SMBIOS_MINOR;
  Ep[8] = (sb_u8)MaxSize;
  Ep[9] = (sb_u8)(MaxSize >> 8);
  Ep[0x10] = '_';
  Ep[0x11] = 'D';
  Ep[0x12] = 'M';
  Ep[0x13] = 'I';
  Ep[0x14] = '_';
  Ep[0x16] = (sb_u8)TableLen;
  Ep[0x17] = (sb_u8)(TableLen >> 8);
  for (i = 0; i < 4; i++) {
    Ep[0x18 + i] = (sb_u8)(TableAddr >> (8 * i));
  }

  Ep[0x1C] = (sb_u8)Count;
  Ep[0x1D] = (sb_u8)(Count >> 8);
  Ep[0x1E] = (sb_u8)((SB_SMBIOS_MAJOR << 4) | SB_SMBIOS_MINOR);
  Ep[0x15] = (sb_u8)(0 - Sum (Ep + 0x10, 0x0F));     // intermediate checksum
  Ep[4]    = (sb_u8)(0 - Sum (Ep, SB_EP21_LEN));
}
