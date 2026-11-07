# SbWifiCx robustness steps for the unattended Windows loop (W2o). Windows PowerShell 5.1, elevated.
# Run after the loop has joined its open network (profile in place). Each step prints one line "robust: ...".
# Nothing here prints network names or addresses.

$ErrorActionPreference = 'Continue'
$dev = 'ACPI\STBW4361\0'

function Say($m) { Write-Output ("robust: " + $m) }

function Wait-IPv4([int]$seconds) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $seconds) {
        $ip = Get-NetIPAddress -InterfaceAlias 'Wi-Fi' -AddressFamily IPv4 -ErrorAction SilentlyContinue |
              Where-Object { $_.IPAddress -notlike '169.254.*' }
        if ($ip) { return ((Get-Date) - $t0).TotalSeconds }
        Start-Sleep -Milliseconds 500
    }
    return -1
}

function Gateway-Ping {
    $gw = (Get-NetRoute -InterfaceAlias 'Wi-Fi' -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
           Select-Object -First 1).NextHop
    if (-not $gw) { return 'no gateway' }
    $r = Test-Connection -ComputerName $gw -Count 4 -ErrorAction SilentlyContinue
    if (-not $r) { return 'gateway ping FAILED' }
    $avg = ($r | Measure-Object -Property ResponseTime -Average).Average
    return ('gateway ping {0}/4, rtt avg {1:N0} ms' -f $r.Count, $avg)
}

function BSS-Count {
    # the BSSIDs Windows currently lists (each scan refreshes them)
    return ((netsh wlan show networks mode=bssid) | Select-String -Pattern '^\s*BSSID ').Count
}

# 1. Scans while connected (the driver refuses them only during a join and the 15 s after an open association)
Start-Sleep -Seconds 20
$before = BSS-Count
Start-Sleep -Seconds 40
Say ("scans while connected: BSSIDs listed {0} then {1}; {2}" -f $before, (BSS-Count), (Gateway-Ping))

# 2. Radio off and on through Windows' radio API (what the Wi-Fi switch and airplane mode use)
try {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
    function Await($op, $type) {
        $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op)); $t.Wait(-1) | Out-Null; $t.Result }
    [Windows.Devices.Radios.Radio, Windows.System.Devices, ContentType = WindowsRuntime] | Out-Null
    [Windows.Devices.Radios.RadioAccessStatus, Windows.System.Devices, ContentType = WindowsRuntime] | Out-Null
    Await ([Windows.Devices.Radios.Radio]::RequestAccessAsync()) ([Windows.Devices.Radios.RadioAccessStatus]) | Out-Null
    $radios = Await ([Windows.Devices.Radios.Radio]::GetRadiosAsync()) `
                    ([System.Collections.Generic.IReadOnlyList[Windows.Devices.Radios.Radio]])
    $wifi = $radios | Where-Object { $_.Kind -eq 'WiFi' } | Select-Object -First 1
    if (-not $wifi) {
        Say 'radio: Windows lists no Wi-Fi radio'
    } else {
        for ($i = 1; $i -le 2; $i++) {
            $s = Await ($wifi.SetStateAsync('Off')) ([Windows.Devices.Radios.RadioAccessStatus])
            Start-Sleep -Seconds 5
            $iface = (netsh wlan show interfaces | Select-String -Pattern 'Software').Line
            $s2 = Await ($wifi.SetStateAsync('On')) ([Windows.Devices.Radios.RadioAccessStatus])
            $t = Wait-IPv4 45
            Say ("radio off/on {0}: off {1} ({2}), on {3}, IPv4 back after {4:N1} s; {5}" -f
                 $i, $s, ($iface -replace '\s+', ' ').Trim(), $s2, $t, (Gateway-Ping))
        }
    }
} catch {
    Say ("radio: the radio API failed: " + $_.Exception.Message)
}

# 3. The device disabled and enabled: the driver stops and starts the radio again (the path sleep/wake takes)
for ($i = 1; $i -le 2; $i++) {
    $d = (pnputil /disable-device $dev 2>&1 | Out-String) -replace '\s+', ' '
    Start-Sleep -Seconds 5
    $e = (pnputil /enable-device $dev 2>&1 | Out-String) -replace '\s+', ' '
    $t = Wait-IPv4 90
    Say ("device restart {0}: disable [{1}] enable [{2}], IPv4 back after {3:N1} s; {4}" -f
         $i, $d.Trim().Substring(0, [Math]::Min(60, $d.Trim().Length)),
         $e.Trim().Substring(0, [Math]::Min(60, $e.Trim().Length)), $t, (Gateway-Ping))
}

# 4. Disconnect and reconnect through netsh (the profile the loop made)
$prof = ((netsh wlan show interfaces) | Select-String -Pattern '^\s*Profile\s*:').Line
if ($prof) {
    $name = ($prof -split ':', 2)[1].Trim()
    for ($i = 1; $i -le 3; $i++) {
        netsh wlan disconnect | Out-Null
        Start-Sleep -Seconds 3
        netsh wlan connect name="$name" | Out-Null
        $t = Wait-IPv4 45
        Say ("reconnect {0}: IPv4 back after {1:N1} s; {2}" -f $i, $t, (Gateway-Ping))
    }
} else {
    Say 'reconnect: no profile connected'
}
Say 'done'
