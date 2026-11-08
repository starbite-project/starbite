# T11a (docs/WIN11-GPU.md 6.7): Mesa 26.2.6's D3D10 user-mode driver, cross-built for ARM64, loaded by the
# phone's own D3D11 runtime as a software adapter (D3D_DRIVER_TYPE_SOFTWARE), drawing a triangle at feature
# level 10_0. User mode only: no driver is installed and nothing touches the GPU (the Panfrost build finds no
# Mali device and must fall back to softpipe). Run through autotest-cycle.sh --robust (output lands in
# run.log) or by hand: powershell -ExecutionPolicy Bypass -File C:\starbite\auto\d3d\d3dtest.ps1
$d = 'C:\starbite\auto\d3d'
foreach ($cfg in 'softpipe', 'panfrost') {
    $dll = "$d\libgallium_d3d10-$cfg.dll"
    if (-not (Test-Path $dll)) { "d3dtest ${cfg}: missing $dll"; continue }
    $t0 = Get-Date
    $p = Start-Process -FilePath "$d\triangle.exe" -ArgumentList "`"$dll`"" -WorkingDirectory $d -NoNewWindow -PassThru `
        -RedirectStandardOutput "$d\$cfg.out" -RedirectStandardError "$d\$cfg.err"
    if (-not $p.WaitForExit(180000)) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        "d3dtest ${cfg}: HUNG, killed after 180 s"
    } else {
        "d3dtest ${cfg}: exit $($p.ExitCode) in $([int]((Get-Date) - $t0).TotalMilliseconds) ms (0 = the triangle's pixels are right)"
    }
    Get-Content "$d\$cfg.out", "$d\$cfg.err" -ErrorAction SilentlyContinue | Select-Object -Last 30 | ForEach-Object { "  $_" }
    if (Test-Path "$d\triangle.ppm") { Move-Item -Force "$d\triangle.ppm" "$d\triangle-$cfg.ppm" }
}
