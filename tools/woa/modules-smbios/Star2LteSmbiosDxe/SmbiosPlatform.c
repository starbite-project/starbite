/** @file
  Star2LteSmbiosDxe: SMBIOS for Windows on the Galaxy S9 (SM-G960F, Exynos 9810).

  The woa release has no SMBIOS (no SmbiosDxe; the =FWTS A21 trace lists acpi20 only), so Windows'
  Task Manager shows an empty CPU name and System Information no model. edk2's SmbiosDxe does not fit
  the release's fixed slot for the compressed DXE volume (with it the volume is 0x9C0 bytes over even
  with SerialDxe blanked), and nothing in UEFI consumes the SMBIOS protocol: Windows reads the
  configuration tables. So this module builds the table itself (SmbiosBuild.c, host-tested with
  dmidecode) and publishes both entry points:

    gEfiSmbios3TableGuid -> the SMBIOS 3.0 (64-bit) entry point, what Windows on Arm reads
    gEfiSmbiosTableGuid  -> the SMBIOS 2.1 (32-bit) entry point to the same table

  All in one page below 4 GiB of EfiRuntimeServicesData (as edk2's SmbiosDxe: both entry points and the
  table survive ExitBootServices). Records: SmbiosBuild.c; the cache records (type 7) only with
  STARBITE_SMBIOS_TYPE7=1 (the PPTT with the same caches bugchecked Windows 0x7E, unexplained).
  Nothing here touches hardware except reading MIDR_EL1; any failure leaves the boot as it was.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Guid/SmBios.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "SmbiosBuild.h"

#ifndef STARBITE_SMBIOS_TYPE7
#define STARBITE_SMBIOS_TYPE7  1
#endif

#define EP3_OFF    0x00
#define EP21_OFF   0x20
#define TABLE_OFF  0x40

EFI_STATUS
EFIAPI
Star2LteSmbiosEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_PHYSICAL_ADDRESS  Page;
  UINT8                 *P;
  UINT64                Midr;
  UINT32                Len;
  UINT16                Count;
  UINT16                MaxSize;

  Page = 0xFFFFFFFFULL;
  if (gBS->AllocatePages (AllocateMaxAddress, EfiRuntimeServicesData, 1, &Page) != EFI_SUCCESS) {
    return EFI_SUCCESS;                  // never fail the boot
  }

  P = (UINT8 *)(UINTN)Page;
  ZeroMem (P, EFI_PAGE_SIZE);
  Midr = 0;
  __asm__ volatile ("mrs %0, midr_el1" : "=r" (Midr));
  Len = SbSmbiosBuild (P + TABLE_OFF, EFI_PAGE_SIZE - TABLE_OFF, (UINT32)Midr, STARBITE_SMBIOS_TYPE7, &Count, &MaxSize);
  if (Len == 0) {
    gBS->FreePages (Page, 1);
    return EFI_SUCCESS;
  }

  SbSmbiosEp3 (P + EP3_OFF, Page + TABLE_OFF, Len);
  SbSmbiosEp21 (P + EP21_OFF, (UINT32)(Page + TABLE_OFF), (UINT16)Len, Count, MaxSize);
  gBS->InstallConfigurationTable (&gEfiSmbios3TableGuid, P + EP3_OFF);
  gBS->InstallConfigurationTable (&gEfiSmbiosTableGuid, P + EP21_OFF);
  return EFI_SUCCESS;
}
