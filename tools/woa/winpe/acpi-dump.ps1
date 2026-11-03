# Dump every ACPI table the running Windows got from the firmware (run in the phone's
# Windows 11, PowerShell as Administrator):
#   powershell -ExecutionPolicy Bypass -File acpi-dump.ps1 [C:\Starbite\acpi]
# Writes SIG[-n].bin per table + tables.txt (signature, length, OEM ids, checksum ok).
# Purpose (docs/WIN11-24H2-A55.md): see the MADT/PPTT/GTDT the RELEASE DXE's "P" module
# really publishes (MPIDRs, GICC flags, parking mailboxes) before building a 4xA55-only
# MADT. Pull the folder later from TWRP (ntfs-3g read-only) with adb pull. Reads only.
param([string]$Out = 'C:\Starbite\acpi')
$sig = @'
using System;
using System.Runtime.InteropServices;
public static class Fw {
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern uint EnumSystemFirmwareTables(uint provider, byte[] buf, uint size);
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern uint GetSystemFirmwareTable(uint provider, uint id, byte[] buf, uint size);
}
'@
Add-Type -TypeDefinition $sig
$ACPI = 0x41435049   # 'ACPI'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$n = [Fw]::EnumSystemFirmwareTables($ACPI, $null, 0)
$ids = New-Object byte[] $n
[void][Fw]::EnumSystemFirmwareTables($ACPI, $ids, $n)
$seen = @{}
$lines = @()
for ($i = 0; $i -lt $n; $i += 4) {
  $id = [BitConverter]::ToUInt32($ids, $i)
  $name = [Text.Encoding]::ASCII.GetString($ids, $i, 4)
  $len = [Fw]::GetSystemFirmwareTable($ACPI, $id, $null, 0)
  if ($len -eq 0) { continue }
  $buf = New-Object byte[] $len
  [void][Fw]::GetSystemFirmwareTable($ACPI, $id, $buf, $len)
  $k = if ($seen.ContainsKey($name)) { $seen[$name]++; "$name-$($seen[$name])" } else { $seen[$name] = 0; $name }
  [IO.File]::WriteAllBytes((Join-Path $Out "$k.bin"), $buf)
  $sum = 0; foreach ($b in $buf) { $sum = ($sum + $b) -band 0xFF }
  $oem = [Text.Encoding]::ASCII.GetString($buf, 10, 6) + '/' + [Text.Encoding]::ASCII.GetString($buf, 16, 8)
  $lines += ('{0,-8} {1,6} bytes  {2}  checksum {3}' -f $k, $len, $oem, $(if ($sum -eq 0) { 'ok' } else { 'BAD' }))
}
$lines | Set-Content (Join-Path $Out 'tables.txt')
$lines
