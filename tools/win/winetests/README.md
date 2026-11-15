# Wine's D3D test suites as our driver's conformance run

`docs/WIN11-GPU-FULL-DRIVER.md` item 2. Wine's `d3d10core`, `d3d11` and `dxgi` tests check thousands of API
behaviours against what real Windows drivers do. `build.sh [SUITE...]` fetches the suite's source from Wine 10.0
(sha256-pinned; LGPL-2.1, kept in `build/win/winetests/src`, not in this repo), patches a copy so every test
function has a name, and builds it for ARM64 Windows with the VM's clang-cl against the Windows SDK
(`shim/wine/*.h` stand in for the two Wine-only headers). `sbwt.exe` (sbwt.c) then runs each test function in its
own process with a timeout and writes one PASS / FAIL / CRASH / TIMEOUT line per test plus the first failing
checks, so a crash or hang costs one test, not the run.

    tools/win/winetests/build.sh d3d10core        # -> build/win/winetests/d3d10core_test.exe, sbwt.exe
    sbwt.exe d3d10core_test.exe d3d10core LOG [TIMEOUT_S [FIRST [COUNT]]]     (on the phone, sb-wt.bat)

On Windows the tests' `todo_wine` marks do not apply and `broken()` results are accepted, i.e. the expectations
are those of real drivers.
