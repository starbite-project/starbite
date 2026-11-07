# Starbite unattended Wi-Fi join test (Windows 11 on the S9). Started by sb-autotest.cmd; logs to C:\starbite\auto\run.log.
# Joins the strongest OPEN network in range through a temporary profile named "starbite-autotest" (so no network name or
# address is ever written to the log), records the connection state, the IP configuration and simple reachability,
# then removes the profile. Never touches secured networks or saved profiles.
$log = 'C:\starbite\auto\run.log'
function L($m) { "$(Get-Date -Format 'HH:mm:ss.fff') $m" | Out-File -Append -Encoding ascii $log }
function IfLines {
    netsh wlan show interfaces | ForEach-Object { $_.Trim() } |
        Where-Object { $_ -match '^(State|Radio type|Authentication|Cipher|Connection mode|Channel|Receive rate|Transmit rate|Signal|Band)\s*:' } |
        ForEach-Object { $_ -replace '\s+:\s+', '=' }
}
L "autotest.ps1 start (pid $PID)"
$a = $null
for ($i = 0; $i -lt 45; $i++) {
    $a = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -like '*Starbite Wi-Fi*' }
    if ($a) { break }; Start-Sleep 2
}
if (-not $a) { L 'no Starbite Wi-Fi adapter'; exit 2 }
L "adapter: status=$($a.Status) media=$($a.MediaConnectionState) ifIndex=$($a.ifIndex)"
Start-Sleep 25    # let WLAN AutoConfig scan a few times
$best = $null; $cur = $null; $n = 0
foreach ($line in (netsh wlan show networks mode=bssid)) {
    if ($line -match '^SSID \d+ : (.*)$') { $cur = @{ name = $Matches[1]; auth = ''; enc = ''; sig = 0; ch = '' }; if ($cur.name) { $n++ } }
    elseif ($cur -and $line -match '^\s+Authentication\s+:\s+(.*)$') { $cur.auth = $Matches[1].Trim() }
    elseif ($cur -and $line -match '^\s+Encryption\s+:\s+(.*)$') { $cur.enc = $Matches[1].Trim() }
    elseif ($cur -and $line -match '^\s+Signal\s+:\s+(\d+)%') {
        $s = [int]$Matches[1]
        if ($s -gt $cur.sig) { $cur.sig = $s }
        if ($cur.name -and $cur.auth -eq 'Open' -and $cur.enc -eq 'None' -and (-not $best -or $cur.sig -gt $best.sig)) { $best = $cur }
    }
    elseif ($cur -and $line -match '^\s+Channel\s+:\s+(\d+)') { if ($best -eq $cur) { $cur.ch = $Matches[1] } }
}
L "scan: $n named networks; open candidate: $(if ($best) { "signal $($best.sig)%, channel $($best.ch)" } else { 'none' })"
if (-not $best) { L 'no open network in range: nothing to join'; exit 3 }
$esc = [System.Security.SecurityElement]::Escape($best.name)
$xml = @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
<name>starbite-autotest</name>
<SSIDConfig><SSID><name>$esc</name></SSID></SSIDConfig>
<connectionType>ESS</connectionType>
<connectionMode>manual</connectionMode>
<MSM><security><authEncryption><authentication>open</authentication><encryption>none</encryption><useOneX>false</useOneX></authEncryption></security></MSM>
</WLANProfile>
"@
$pf = 'C:\starbite\auto\p.xml'
$xml | Out-File -Encoding ascii $pf
$r = netsh wlan add profile filename="$pf" user=current; Remove-Item -Force $pf
L "add profile: $(($r -join ' ').Trim() -replace [regex]::Escape($best.name), '<net>')"
foreach ($try in 1..2) {
    $before = Get-NetAdapterStatistics -Name $a.Name -ErrorAction SilentlyContinue
    $r = netsh wlan connect name=starbite-autotest interface="$($a.Name)"
    L "try ${try}: connect: $(($r -join ' ').Trim())"
    $ip = $null
    for ($t = 0; $t -lt 30; $t++) {
        Start-Sleep 3
        $st = (IfLines) -join ', '
        $ip = Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Select-Object -First 1
        $m = (Get-NetAdapter -InterfaceIndex $a.ifIndex).MediaConnectionState
        L "try $try +$(3 * ($t + 1))s: media=$m ipv4=$($ip.IPAddress)/$($ip.PrefixOrigin) | $st"
        if ($ip -and $ip.IPAddress -notlike '169.254.*') { break }
    }
    $after = Get-NetAdapterStatistics -Name $a.Name -ErrorAction SilentlyContinue
    L "try $try stats: sent bytes $($after.SentBytes - $before.SentBytes), received bytes $($after.ReceivedBytes - $before.ReceivedBytes), sent unicast $($after.SentUnicastPackets - $before.SentUnicastPackets), received unicast $($after.ReceivedUnicastPackets - $before.ReceivedUnicastPackets)"
    if ($ip -and $ip.IPAddress -notlike '169.254.*') {
        $gw = (Get-NetRoute -InterfaceIndex $a.ifIndex -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Select-Object -First 1).NextHop
        L "try ${try}: GOT IPv4 (DHCP); gateway present: $([bool]$gw)"
        if ($gw) { $p = Test-Connection -ComputerName $gw -Count 3 -Quiet -ErrorAction SilentlyContinue; L "ping gateway: $p" }
        try { $d = Resolve-DnsName -Name www.msftconnecttest.com -Type A -ErrorAction Stop | Select-Object -First 1; L "dns: ok" } catch { L "dns: FAILED $($_.Exception.Message)" }
        try { $w = Invoke-WebRequest -UseBasicParsing -TimeoutSec 10 http://www.msftconnecttest.com/connecttest.txt; L "http: $($w.StatusCode) '$($w.Content.Trim())'" } catch { L "http: FAILED $($_.Exception.Message)" }
        break
    }
    netsh wlan disconnect interface="$($a.Name)" | Out-Null
    Start-Sleep 5
}
netsh wlan disconnect interface="$($a.Name)" | Out-Null
$r = netsh wlan delete profile name=starbite-autotest
L "delete profile: $(($r -join ' ').Trim())"
L 'autotest.ps1 done'
