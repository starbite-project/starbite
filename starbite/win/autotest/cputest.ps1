# Starbite CPU + x86/x64 emulator check (docs/WIN11-24H2-A55.md, "Test program"). Run by autotest.ps1 when present,
# before the Wi-Fi test; writes C:\starbite\auto\cpu.log. Needs C:\starbite\auto\sbcpu-{arm64,x64,x86}.exe.
# Never fails the caller: every step is wrapped, so the Wi-Fi part still runs.
$log = 'C:\starbite\auto\cpu.log'
function L($m) { "$(Get-Date -Format 'HH:mm:ss.fff') $m" | Out-File -Append -Encoding ascii $log }
L 'cputest.ps1 start'
try {
    $cs = Get-CimInstance Win32_ComputerSystem
    L "Win32_ComputerSystem: logical $($cs.NumberOfLogicalProcessors) packages $($cs.NumberOfProcessors)"
    foreach ($p in Get-CimInstance Win32_Processor) {
        L "Win32_Processor: $($p.DeviceID) cores $($p.NumberOfCores) logical $($p.NumberOfLogicalProcessors) enabled $($p.NumberOfEnabledCore) L2 $($p.L2CacheSize) L3 $($p.L3CacheSize) MHz $($p.MaxClockSpeed) '$($p.Name)'"
    }
} catch { L "CIM failed: $($_.Exception.Message)" }
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
foreach ($a in 'arm64', 'x64', 'x86') {
    $exe = "C:\starbite\auto\sbcpu-$a.exe"
    if (-not (Test-Path $exe)) { L "sbcpu-$a.exe missing"; continue }
    try {
        $p = Start-Process -FilePath $exe -ArgumentList $log -NoNewWindow -PassThru -Wait
        L ("sbcpu-{0}: exit code {1} (0x{1:x8})" -f $a, $p.ExitCode)
    } catch { L "sbcpu-$a start failed: $($_.Exception.Message)" }
}
# a second x86 check through the in-box WoW64 cmd.exe (the same path Steam's 32-bit installer takes)
try {
    $p = Start-Process -FilePath 'C:\Windows\SysWOW64\cmd.exe' -ArgumentList '/c', 'exit 7' -NoNewWindow -PassThru -Wait
    L ("SysWOW64 cmd.exe: exit code {0} (7 = ran)" -f $p.ExitCode)
} catch { L "SysWOW64 cmd.exe failed: $($_.Exception.Message)" }
Start-Sleep 3
try {
    $ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $boot } -ErrorAction SilentlyContinue
    L "Application error events this boot: $(@($ev).Count)"
    foreach ($e in $ev | Select-Object -First 20) {
        $d = $e.Properties | ForEach-Object { $_.Value }
        L "  1000: app $($d[0]) module $($d[3]) $($d[4]) code $($d[6]) offset $($d[7])"
    }
} catch { L "event read failed: $($_.Exception.Message)" }
L 'cputest.ps1 done'
