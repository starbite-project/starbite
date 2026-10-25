# W4: build the Windows volume for the S9 (starlte) in the "Starbite Windows" UTM VM.
# PowerShell port of the woa installer's VhdxImageBuilder + ImageBuilder (Stock profile) +
# UnattendXml + BootConfiguration, with our sda geometry (tools/woa/gpt-split.sh) instead of
# star2lte's PartitionMap.cs. Run by tools/woa/vm-build-image.sh (as SYSTEM, via utmctl exec);
# progress in C:\sb\build.log, ends with SB-BUILD-DONE or SB-BUILD-FAIL.
# The VHDX mirrors the phone disk: 4Kn, ESP at WINESP's offset (FAT32), NTFS at WINDOWS' offset,
# so the volumes copy to sda26/sda27 unchanged (NTFS hidden sectors = the real partition start).
param(
    [Parameter(Mandatory)][string]$Iso,
    [Parameter(Mandatory)][long]$DiskBytes,
    [Parameter(Mandatory)][long]$EspOffset, [Parameter(Mandatory)][long]$EspBytes,
    [Parameter(Mandatory)][long]$WinOffset, [Parameter(Mandatory)][long]$WinBytes,
    [Parameter(Mandatory)][string]$DiskGuid, [Parameter(Mandatory)][string]$EspGuid,
    [Parameter(Mandatory)][string]$WinGuid,
    [string]$Vhdx = 'C:\sb\starbite-win.vhdx',
    [string]$Drivers = 'C:\sb\drivers',
    [string]$User = 'starbite', [string]$ComputerName = 'STARBITE-S9',
    [string]$TimeZone = 'FLE Standard Time', [int]$Dpi = 275
)
$ErrorActionPreference = 'Stop'
$log = 'C:\sb\build.log'
# The host polls build.log through the guest agent, which can hold it open for a moment: retry.
function Log($m) {
    $line = "$(Get-Date -Format HH:mm:ss) $m"
    for ($i = 0; $i -lt 50; $i++) {
        try { [IO.File]::AppendAllText($log, "$line`r`n"); return } catch { Start-Sleep -Milliseconds 200 }
    }
}
function Run($exe, [string[]]$a) {
    Log "> $exe $($a -join ' ')"
    $o = & $exe @a 2>&1 | Out-String
    $o.Trim() -split "`n" | Select-Object -Last 4 | ForEach-Object { Log "  $_" }
    if ($LASTEXITCODE -notin 0, 3010) { throw "$exe failed ($LASTEXITCODE)" }
}

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class SbVirtDisk {
    [StructLayout(LayoutKind.Sequential)] public struct StorageType { public uint DeviceId; public Guid VendorId; }
    [StructLayout(LayoutKind.Explicit, Size = 128)] public struct CreateV2 {
        [FieldOffset(0)] public int Version;
        [FieldOffset(8)] public Guid UniqueId;
        [FieldOffset(24)] public ulong MaximumSize;
        [FieldOffset(32)] public uint BlockSizeInBytes;
        [FieldOffset(36)] public uint SectorSizeInBytes;
        [FieldOffset(40)] public uint PhysicalSectorSizeInBytes;
        [FieldOffset(48)] public IntPtr ParentPath;
        [FieldOffset(56)] public IntPtr SourcePath;
        [FieldOffset(64)] public int OpenFlags;
        [FieldOffset(68)] public StorageType ParentType;
        [FieldOffset(88)] public StorageType SourceType;
        [FieldOffset(108)] public Guid ResiliencyGuid;
    }
    [DllImport("virtdisk.dll", CharSet = CharSet.Unicode)]
    static extern int CreateVirtualDisk(ref StorageType t, string path, int access, IntPtr sd, int flags,
        int providerFlags, ref CreateV2 p, IntPtr overlapped, out IntPtr handle);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    // Dynamic VHDX with 4096-byte logical and physical sectors (New-VHD needs Hyper-V, absent on ARM).
    public static void Create4Kn(string path, ulong bytes) {
        var t = new StorageType { DeviceId = 3, VendorId = new Guid("EC984AEC-A0F9-47e9-901F-71415A66345B") };
        var p = new CreateV2 { Version = 2, UniqueId = Guid.NewGuid(), MaximumSize = bytes,
            BlockSizeInBytes = 32u << 20, SectorSizeInBytes = 4096, PhysicalSectorSizeInBytes = 4096 };
        IntPtr h;
        int r = CreateVirtualDisk(ref t, path, 0, IntPtr.Zero, 0, 0, ref p, IntPtr.Zero, out h);
        if (r != 0) throw new System.ComponentModel.Win32Exception(r);
        CloseHandle(h);
    }
}
'@

$isoMounted = $false
try {
    Remove-Item -Force $log -ErrorAction SilentlyContinue
    Log "build-image: $Iso"
    foreach ($l in 'S', 'W') { if (Test-Path "${l}:\") { throw "Drive ${l}: is already in use" } }
    if (Test-Path $Vhdx) { Dismount-DiskImage -ImagePath $Vhdx -ErrorAction SilentlyContinue | Out-Null; Remove-Item -Force $Vhdx }

    Log "creating 4Kn VHDX $DiskBytes bytes"
    [SbVirtDisk]::Create4Kn($Vhdx, [uint64]$DiskBytes)
    $disk = Mount-DiskImage -ImagePath $Vhdx -NoDriveLetter -PassThru | Get-Disk
    if ($disk.LogicalSectorSize -ne 4096) { throw "VHDX is not 4Kn ($($disk.LogicalSectorSize))" }
    Initialize-Disk -Number $disk.Number -PartitionStyle GPT
    Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue | Where-Object Type -eq Reserved | Remove-Partition -Confirm:$false
    $esp = New-Partition -DiskNumber $disk.Number -Offset $EspOffset -Size $EspBytes -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
    $win = New-Partition -DiskNumber $disk.Number -Offset $WinOffset -Size $WinBytes -GptType '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
    if ($esp.Offset -ne $EspOffset -or $esp.Size -ne $EspBytes -or $win.Offset -ne $WinOffset -or $win.Size -ne $WinBytes) {
        throw "partitions do not match WINESP/WINDOWS: esp $($esp.Offset)+$($esp.Size) win $($win.Offset)+$($win.Size)"
    }
    Format-Volume -Partition $esp -FileSystem FAT32 -NewFileSystemLabel 'System' -Confirm:$false | Out-Null
    Format-Volume -Partition $win -FileSystem NTFS -AllocationUnitSize 4096 -NewFileSystemLabel 'Windows' -Confirm:$false | Out-Null
    $esp | Set-Partition -NewDriveLetter S
    $win | Set-Partition -NewDriveLetter W
    Log "disk $($disk.Number): ESP S: $EspOffset+$EspBytes, Windows W: $WinOffset+$WinBytes"

    $isoVol = Mount-DiskImage -ImagePath $Iso -PassThru | Get-Volume
    $isoMounted = $true
    $src = "$($isoVol.DriveLetter):\sources"
    $wim = @("$src\install.wim", "$src\install.esd") | Where-Object { Test-Path $_ } | Select-Object -First 1
    $img = Get-WindowsImage -ImagePath $wim | Where-Object ImageName -eq 'Windows 11 Pro' | Select-Object -First 1
    if (-not $img) { throw "no 'Windows 11 Pro' in $wim" }
    Log "applying $wim index $($img.ImageIndex) ($($img.ImageName))"
    Run dism.exe @('/English', '/Apply-Image', "/ImageFile:$wim", "/Index:$($img.ImageIndex)", '/ApplyDir:W:\')

    foreach ($d in Get-ChildItem $Drivers -Directory) {
        Run dism.exe @('/English', '/Image:W:\', '/Add-Driver', "/Driver:$($d.FullName)", '/Recurse', '/ForceUnsigned')
    }

    # OOBE answer file (UnattendXml.cs): local admin, no password, autologon once, 275 DPI.
    $hive = 'HKU\S9WoaDefaultUser'
    $dpiCmds = @("cmd.exe /c reg.exe load $hive `"%SystemDrive%\Users\Default\NTUSER.DAT`"")
    foreach ($k in $hive, 'HKU\.DEFAULT') {
        if ($k -eq 'HKU\.DEFAULT') { $dpiCmds += "reg.exe unload $hive" }
        $dpiCmds += "reg.exe add `"$k\Control Panel\Desktop`" /v LogPixels /t REG_DWORD /d $Dpi /f"
        $dpiCmds += "reg.exe add `"$k\Control Panel\Desktop`" /v Win8DpiScaling /t REG_DWORD /d 1 /f"
    }
    $comp = 'processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS"'
    $i = 0
    $run = ($dpiCmds | ForEach-Object { $i++; "<RunSynchronousCommand wcm:action=`"add`"><Order>$i</Order><Description>Display scaling $Dpi DPI ($i)</Description><Path>$([Security.SecurityElement]::Escape($_))</Path></RunSynchronousCommand>" }) -join ''
    $xml = @"
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
<settings pass="specialize"><component name="Microsoft-Windows-Deployment" $comp><RunSynchronous>$run</RunSynchronous></component></settings>
<settings pass="oobeSystem">
<component name="Microsoft-Windows-International-Core" $comp><InputLocale>en-US</InputLocale><SystemLocale>en-US</SystemLocale><UILanguage>en-US</UILanguage><UserLocale>en-US</UserLocale></component>
<component name="Microsoft-Windows-Shell-Setup" $comp><ComputerName>$ComputerName</ComputerName><TimeZone>$TimeZone</TimeZone>
<OOBE><HideEULAPage>true</HideEULAPage><HideOEMRegistrationScreen>true</HideOEMRegistrationScreen><HideOnlineAccountScreens>true</HideOnlineAccountScreens><HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE><ProtectYourPC>3</ProtectYourPC><SkipMachineOOBE>true</SkipMachineOOBE><SkipUserOOBE>true</SkipUserOOBE></OOBE>
<UserAccounts><LocalAccounts><LocalAccount wcm:action="add"><Name>$User</Name><DisplayName>$User</DisplayName><Group>Administrators</Group><Password><Value></Value><PlainText>true</PlainText></Password></LocalAccount></LocalAccounts></UserAccounts>
<AutoLogon><Enabled>true</Enabled><LogonCount>1</LogonCount><Username>$User</Username><Password><Value></Value><PlainText>true</PlainText></Password></AutoLogon>
</component></settings>
</unattend>
"@
    New-Item -ItemType Directory -Force W:\Windows\Panther | Out-Null
    [IO.File]::WriteAllText('W:\Windows\Panther\unattend.xml', $xml, (New-Object Text.UTF8Encoding $false))
    $null = [xml]$xml   # well-formed or throw
    Log "unattend written"

    & bcdboot.exe W:\Windows /s S: /f UEFI /offline 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Run bcdboot.exe @('W:\Windows', '/s', 'S:', '/f', 'UEFI') } else { Log "bcdboot /offline ok" }
    $bcd = 'S:\EFI\Microsoft\Boot\BCD'
    foreach ($s in @(@('path', '\Windows\System32\winload.efi'), @('systemroot', '\Windows'), @('testsigning', 'on'),
                     @('nointegritychecks', 'off'), @('numproc', '4'), @('vsmlaunchtype', 'off'),
                     @('hypervisorlaunchtype', 'off'), @('detecthal', 'on'), @('bootstatuspolicy', 'IgnoreAllFailures'))) {
        Run bcdedit.exe @('/store', $bcd, '/set', '{default}', $s[0], $s[1])
    }
    # GPT-qualified devices (BootConfiguration.DeviceScript): boot manager on WINESP, loader on WINDOWS.
    function Call($o, [string]$m, [hashtable]$a) {
        $r = Invoke-CimMethod -InputObject $o -MethodName $m -Arguments $a
        if (-not $r.ReturnValue) { throw "BCD rejected $m" }
        $r
    }
    $store = (Invoke-CimMethod -Namespace root\WMI -ClassName BcdStore -MethodName OpenStore -Arguments @{ File = $bcd }).Store
    $mgr = (Call $store 'OpenObject' @{ Id = '{9dea862c-5cdd-4e70-acc1-f32b344d4795}' }).Object
    $loaderId = (Call $mgr 'GetElement' @{ Type = [uint32]0x23000003 }).Element.Id
    $loader = (Call $store 'OpenObject' @{ Id = $loaderId }).Object
    $null = Call $mgr 'SetQualifiedPartitionDeviceElement' @{ Type = [uint32]0x11000001; PartitionStyle = [uint32]1; DiskSignature = $DiskGuid; PartitionIdentifier = $EspGuid }
    foreach ($t in [uint32]0x11000001, [uint32]0x21000001) {
        $null = Call $loader 'SetQualifiedPartitionDeviceElement' @{ Type = $t; PartitionStyle = [uint32]1; DiskSignature = $DiskGuid; PartitionIdentifier = $WinGuid }
    }
    $null = Call $loader 'SetBooleanElement' @{ Type = [uint32]0x16000060; Boolean = $false }
    $null = Call $mgr 'SetIntegerElement' @{ Type = [uint32]0x25000004; Integer = [uint64]0 }
    & bcdedit.exe /store $bcd /enum all 2>&1 | Out-File -Encoding utf8 C:\sb\bcd.txt
    Log "BCD set: mgr -> $EspGuid, loader -> $WinGuid on $DiskGuid"

    foreach ($f in 'winload.efi', 'ntoskrnl.exe') {
        $p = "W:\Windows\System32\$f"
        Log "$f $((Get-Item $p).VersionInfo.FileVersionRaw) sha256 $((Get-FileHash $p -Algorithm SHA256).Hash.ToLower())"
    }
    $v = Get-Volume -DriveLetter W
    Log "Windows volume used $([math]::Round(($v.Size - $v.SizeRemaining) / 1GB, 2)) GiB of $([math]::Round($v.Size / 1GB, 2))"
    Log 'SB-BUILD-DONE'
} catch {
    Log "ERROR: $($_.Exception.Message) at $($_.InvocationInfo.PositionMessage)"
    Log 'SB-BUILD-FAIL'
} finally {
    if ($isoMounted) { Dismount-DiskImage -ImagePath $Iso -ErrorAction SilentlyContinue | Out-Null }
    Dismount-DiskImage -ImagePath $Vhdx -ErrorAction SilentlyContinue | Out-Null
}
