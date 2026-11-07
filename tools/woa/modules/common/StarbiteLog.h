/** @file
  Logging shared by the Starbite DXE modules (Star2LteUsbDxe, Star2LteSsdtDxe); copied into each
  module by tools/woa/patches/gen-0024.py. Every line goes to three places:

  1. the pmsg ring at 0xFED14000 (patch 0003's format): read in TWRP as pmsg-ramoops-0. Windows'
     UFS driver fills this ring during a Windows session, so after Windows it is usually gone;
  2. the evidence ring at 0xFED10000, ramoops zone 0 (same "DBGC" header, start, size format):
     Windows doesn't write it. Capped at 0x2FF4 bytes of data so it ends at 0xFED13000, below the
     woa firmware's diagnostic block there (the release UFS module writes 0xFED13000.., the release
     BDS 0xFED136D0, 0xFED13800, 0xFED13B00, 0xFED13F00; RWD1 0xFED13D80, P3 0xFED13E80). Never
     wraps: the first line that doesn't fit is replaced by "=SB log full" and the ring then stops
     for the rest of the boot. Reset by the first line of each boot. Read it at the loader menu
     with "fastboot oem log dmesg" (fastboot_dev.c ring_text), BEFORE TWRP: TWRP's ramoops discards a zone-0 record without a kmsg header. The loader's crash
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
#define SB_EVLOG_CAP   0x2FF4u     // data 0xFED1000C..0xFED13000
#define SB_EVLOG_FULL  "=SB log full\n"
#define SB_DBGC        0x43474244u

//
// Installed (NULL interface) by the first module that logs in a boot: later modules append.
//
STATIC EFI_GUID  mSbEvlogBootGuid = {
  0xdb4e476d, 0xe4d0, 0x4515, { 0x8d, 0x4a, 0x41, 0x4a, 0xa6, 0xfb, 0x8f, 0x7a }
};

//
// Installed when the ring filled up this boot: nothing more is appended.
//
STATIC EFI_GUID  mSbEvlogFullGuid = {
  0xc739b3fe, 0x239b, 0x4197, { 0x94, 0xc7, 0x3c, 0x79, 0xa5, 0xdd, 0x0f, 0x5b }
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

  if (!EFI_ERROR (gBS->LocateProtocol (&mSbEvlogFullGuid, NULL, &Flag))) {
    return;                         // stopped this boot
  }

  Pos = W[2];
  if (Pos + NumberOfBytes + sizeof (SB_EVLOG_FULL) - 1 > SB_EVLOG_CAP) {
    //
    // Full: the marker instead (room for it is always kept), then stop for this boot, so the
    // ring never has a silent gap and never reaches the firmware's block at 0xFED13000.
    //
    Buffer        = SB_EVLOG_FULL;
    NumberOfBytes = sizeof (SB_EVLOG_FULL) - 1;
    Handle        = NULL;
    gBS->InstallProtocolInterface (&Handle, &mSbEvlogFullGuid, EFI_NATIVE_INTERFACE, NULL);
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
