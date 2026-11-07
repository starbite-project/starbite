# USB host report for the phone's Windows 11 (docs/WIN11-USB.md section 9). Reads only.
# Run at logon by usb-report.cmd in the Startup folder, or by hand:
#   powershell -ExecutionPolicy Bypass -File C:\Starbite\usb\usb-report.ps1
# Writes C:\Starbite\usb\report-<yyyyMMdd-HHmmss>.txt; the host pulls it from TWRP
# (ntfs-3g read-only). Waits 60 s first so devices plugged in at boot have enumerated,
# then writes a second report 3 minutes later (after hot-plug tests).
param([string]$Out = 'C:\Starbite\usb', [int]$Delay = 60, [int]$Again = 180)
New-Item -ItemType Directory -Force -Path $Out | Out-Null

function Section($title, [scriptblock]$body) {
  "==== $title"
  try { & $body 2>&1 | Out-String -Width 200 } catch { "error: $_" }
}

function Report {
  $f = Join-Path $Out ('report-{0}.txt' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
  & {
    "Starbite USB report $(Get-Date -Format o)"
    "build $([Environment]::OSVersion.Version)  uptime $([int]((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds) s"
    Section 'the controller (ACPI\PNP0D10)' {
      Get-PnpDevice -InstanceId 'ACPI\PNP0D10\*' -ErrorAction SilentlyContinue |
        Format-List FriendlyName, InstanceId, Status, Problem, ProblemDescription, Service, Class
    }
    Section 'controller properties' {
      Get-PnpDevice -InstanceId 'ACPI\PNP0D10\*' -ErrorAction SilentlyContinue | ForEach-Object {
        Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath',
          'DEVPKEY_Device_DriverVersion', 'DEVPKEY_Device_ProblemCode', 'DEVPKEY_Device_ProblemStatus',
          'DEVPKEY_Device_HardwareIds', 'DEVPKEY_Device_CompatibleIds' -ErrorAction SilentlyContinue |
          Format-Table KeyName, Data -AutoSize
      }
    }
    Section 'controller resources (memory, interrupt)' {
      Get-CimInstance Win32_PnPAllocatedResource -ErrorAction SilentlyContinue |
        Where-Object { $_.Dependent -match 'PNP0D10' } | ForEach-Object { $_.Antecedent.ToString() }
    }
    Section 'USB devices (present)' {
      Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^(USB|USBSTOR|HID|ACPI\\PNP0D10)' } |
        Sort-Object InstanceId | Format-Table Status, Class, FriendlyName, InstanceId -AutoSize
    }
    Section 'USB devices with problems (all, including not present)' {
      Get-PnpDevice | Where-Object { $_.InstanceId -match '^(USB|USBSTOR|ACPI\\PNP0D10)' -and $_.Status -ne 'OK' } |
        Format-Table Status, Problem, FriendlyName, InstanceId -AutoSize
    }
    Section 'pnputil /enum-devices /connected /class USB' { pnputil /enum-devices /connected /class USB }
    Section 'disks' { Get-Disk | Format-Table Number, FriendlyName, BusType, Size, OperationalStatus -AutoSize }
    Section 'volumes' { Get-Volume | Format-Table DriveLetter, FileSystemLabel, FileSystem, Size, SizeRemaining -AutoSize }
    Section 'usbxhci.inf: the ACPI IDs it matches' {
      Select-String -Path "$env:windir\INF\usbxhci.inf" -Pattern 'ACPI\\' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Line.Trim() }
    }
    Section 'kernel/USB events of this boot (System log, last 60)' {
      Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime } -ErrorAction SilentlyContinue |
        Where-Object { $_.ProviderName -match 'USB|xhci|Kernel-PnP|disk|partmgr|Ntfs|volmgr' } |
        Select-Object -Last 60 | Format-Table TimeCreated, Id, ProviderName, Message -Wrap
    }
    Section 'ACPI devices (status, ACPI path): USB0 = ours, TSP0/BTNS = the release GPIO handoff (touch, keys)' {
      Get-PnpDevice -InstanceId 'ACPI\*' -ErrorAction SilentlyContinue | ForEach-Object {
        $p = Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_BiosDeviceName' -ErrorAction SilentlyContinue
        '{0,-8} {1,-14} {2,-40} {3}' -f $_.Status, $p.Data, $_.InstanceId, $_.FriendlyName
      }
    }
  } | Set-Content -Path $f -Encoding UTF8
}

Start-Sleep -Seconds $Delay
Report
if ($Again -gt 0) { Start-Sleep -Seconds $Again; Report }
