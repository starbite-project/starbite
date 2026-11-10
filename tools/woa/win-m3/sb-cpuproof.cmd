@echo off
rem sb-cpuproof.cmd - one-shot Startup item (docs/WIN11-M3-CLEAN.md): which CPUs is this Windows running on?
rem Output: C:\starbite\auto\cpuproof.txt, appended step by step so an early restart keeps what was written.
rem The registry's CentralProcessor\N "CP 4000" value is MIDR_EL1: 531F0020 = Samsung Exynos M3, 410FD05x = Cortex-A55.
set O=C:\starbite\auto\cpuproof.txt
echo === cpuproof %date% %time% > "%O%"
echo NUMBER_OF_PROCESSORS=%NUMBER_OF_PROCESSORS% >> "%O%"
echo PROCESSOR_IDENTIFIER=%PROCESSOR_IDENTIFIER% >> "%O%"
echo PROCESSOR_REVISION=%PROCESSOR_REVISION% >> "%O%"
reg query "HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor" /s >> "%O%" 2>&1
echo === wmi %time% >> "%O%"
powershell -NoProfile -Command "Get-CimInstance Win32_Processor | Format-List Name,Manufacturer,Description,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed,CurrentClockSpeed" >> "%O%" 2>&1
echo === sbcpu %time% >> "%O%"
if exist C:\starbite\auto\sbcpu-arm64.exe C:\starbite\auto\sbcpu-arm64.exe >> "%O%" 2>&1
echo === done %time% >> "%O%"
del "%~f0"
