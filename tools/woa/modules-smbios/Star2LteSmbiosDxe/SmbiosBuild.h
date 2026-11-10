/** @file
  The Galaxy S9's SMBIOS structure table and entry points, byte by byte (no edk2 structs, no library
  calls), so the same code builds in the DXE module and in the host test (test/run.sh, which checks
  the result with dmidecode --from-dump).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef SMBIOS_BUILD_H_
#define SMBIOS_BUILD_H_

#ifdef SB_HOST
#include <stdint.h>
typedef uint8_t   sb_u8;
typedef uint16_t  sb_u16;
typedef uint32_t  sb_u32;
typedef uint64_t  sb_u64;
#else
#include <Base.h>
typedef UINT8   sb_u8;
typedef UINT16  sb_u16;
typedef UINT32  sb_u32;
typedef UINT64  sb_u64;
#endif

#define SB_SMBIOS_MAJOR  3
#define SB_SMBIOS_MINOR  3
#define SB_EP3_LEN       0x18
#define SB_EP21_LEN      0x1F

/* The structure table into Buf (Cap bytes). Returns its length, or 0 when it does not fit.
   Midr: MIDR_EL1 of the boot CPU (type 4's processor ID). Type7: 1 adds the cache records.
   *Count: structures written (the end-of-table one included); *MaxSize: the largest one. */
sb_u32
SbSmbiosBuild (
  sb_u8   *Buf,
  sb_u32  Cap,
  sb_u32  Midr,
  int     Type7,
  sb_u16  *Count,
  sb_u16  *MaxSize
  );

/* SMBIOS 3.0 (64-bit) entry point, SB_EP3_LEN bytes. */
void
SbSmbiosEp3 (
  sb_u8   *Ep,
  sb_u64  TableAddr,
  sb_u32  TableLen
  );

/* SMBIOS 2.1 (32-bit) entry point, SB_EP21_LEN bytes; the table must lie below 4 GiB. */
void
SbSmbiosEp21 (
  sb_u8   *Ep,
  sb_u32  TableAddr,
  sb_u16  TableLen,
  sb_u16  Count,
  sb_u16  MaxSize
  );

#endif
