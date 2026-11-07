/** @file
  Logging shared by the Starbite DXE modules (Star2LteUsbDxe, Star2LteSsdtDxe); copied into each
  module by tools/woa/patches/gen-0024.py. Every line goes to three places:

  1. the pmsg ring at 0xFED14000 (patch 0003's format): read in TWRP as pmsg-ramoops-0. Windows'
     UFS driver fills this ring during a Windows session, so after Windows it is usually gone;
  2. the evidence ring at 0xFED10000, ramoops zone 0 (same "DBGC" header, start, size format):
     Windows doesn't write it. Capped at 0x3700 bytes of data so it ends at 0xFED1370C, below the
     records the woa firmware keeps from 0xFED13800 up (low WDT 0xFED13800, RWD1 0xFED13D80, P3
     0xFED13E80); never wraps (later lines are dropped when full); reset by the first line of each
     boot. Read it at the loader menu with "fastboot oem log dmesg" (fastboot_dev.c ring_text),
     BEFORE TWRP: TWRP's ramoops discards a zone-0 record without a kmsg header. The loader's crash
     check only treats zone 0 as Linux evidence when it starts with "====" after a Linux launch,
     so these lines (they start "=SB", "=USB", "=SSDT") never count as a crash;
  3. DEBUG (serial).

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#ifndef STARBITE_LOG_H_
#define STARBITE_LOG_H_

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define SB_PMSG_BASE   0xFED14000UL
#define SB_PMSG_CAP    0x3F00u
#define SB_EVLOG_BASE  0xFED10000UL
#define SB_EVLOG_CAP   0x3700u
#define SB_DBGC        0x43474244u

//
// Installed (NULL interface) by the first module that logs in a boot: later modules append.
//
STATIC EFI_GUID  mSbEvlogBootGuid = {
  0xdb4e476d, 0xe4d0, 0x4515, { 0x8d, 0x4a, 0x41, 0x4a, 0xa6, 0xfb, 0x8f, 0x7a }
};

STATIC
VOID
SbRingWrite (
  IN CONST CHAR8  *Buffer,
  IN UINTN        NumberOfBytes
  )
{
  volatile UINT32  *W = (volatile UINT32 *)SB_PMSG_BASE;
  volatile UINT8   *B = (volatile UINT8 *)(SB_PMSG_BASE + 12);
  UINTN            Index;
  UINT32           Pos;

  if ((W[0] != SB_DBGC) || (W[1] >= SB_PMSG_CAP) || (W[2] > SB_PMSG_CAP)) {
    W[0] = SB_DBGC;
    W[1] = 0;
    W[2] = 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    if (W[2] < SB_PMSG_CAP) {
      Pos  = W[2];
      W[2] = Pos + 1;
      W[1] = (Pos + 1) % SB_PMSG_CAP;
    } else {
      Pos  = W[1];
      W[1] = (Pos + 1) % SB_PMSG_CAP;
    }

    B[Pos] = (UINT8)Buffer[Index];
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
SbEvlogWrite (
  IN CONST CHAR8  *Buffer,
  IN UINTN        NumberOfBytes
  )
{
  volatile UINT32  *W = (volatile UINT32 *)SB_EVLOG_BASE;
  volatile UINT8   *B = (volatile UINT8 *)(SB_EVLOG_BASE + 12);
  EFI_HANDLE       Handle;
  VOID             *Flag;
  UINTN            Index;
  UINT32           Pos;

  if (EFI_ERROR (gBS->LocateProtocol (&mSbEvlogBootGuid, NULL, &Flag)) ||
      (W[0] != SB_DBGC) || (W[2] > SB_EVLOG_CAP) || (W[1] != W[2]))
  {
    W[0]   = SB_DBGC;
    W[1]   = 0;
    W[2]   = 0;
    Handle = NULL;
    gBS->InstallProtocolInterface (&Handle, &mSbEvlogBootGuid, EFI_NATIVE_INTERFACE, NULL);
  }

  Pos = W[2];
  if (Pos + NumberOfBytes > SB_EVLOG_CAP) {
    return;                         // full: never wrap into the firmware's records
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    B[Pos + Index] = (UINT8)Buffer[Index];
  }

  W[2] = Pos + (UINT32)NumberOfBytes;
  W[1] = W[2];                      // start == size: a ring that never wrapped
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
EFIAPI
SbLog (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Line[200];
  VA_LIST  Args;
  UINTN    Len;

  VA_START (Args, Format);
  Len = AsciiVSPrint (Line, sizeof (Line) - 1, Format, Args);
  VA_END (Args);
  Line[Len++] = '\n';
  Line[Len]   = 0;
  SbRingWrite (Line, Len);
  SbEvlogWrite (Line, Len);
  DEBUG ((DEBUG_INFO, "%a", Line));
}

#endif
