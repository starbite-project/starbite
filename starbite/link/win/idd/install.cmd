@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Starbite Display, install route B (INSTALL.md): the regular PnP install, run once as LocalSystem by the
rem one-shot service "sbiddinst" (armed offline with tools/win/hive-service.py), or by hand from an elevated prompt.
rem Expects the payload in C:\starbite\idd\ (build/link/idd/payload/ on the Mac). Log: C:\starbite\idd\install.log
setlocal
set D=C:\starbite\idd
set L=%D%\install.log
echo ==== %DATE% %TIME% install.cmd >> "%L%"
rem 1. The package's signer must be trusted for a DriverStore install (testsigning does not cover this part).
certutil -f -addstore Root "%D%\starbite-test.cer" >> "%L%" 2>&1
certutil -f -addstore TrustedPublisher "%D%\starbite-test.cer" >> "%L%" 2>&1
rem 2. Root\StarbiteDisplay devnode + driver (stages the INF package in the DriverStore, starts the device).
"%D%\sbdevnode.exe" install "%D%\pkg\StarbiteDisplay.inf" Root\StarbiteDisplay >> "%L%" 2>&1
echo sbdevnode exit code %ERRORLEVEL% >> "%L%"
rem 3. What PnP made of it.
pnputil /enum-devices /class Display /drivers >> "%L%" 2>&1
pnputil /enum-devices /class Monitor >> "%L%" 2>&1
rem 4. One shot: never run again (no-op when started by hand).
sc config sbiddinst start= disabled >> "%L%" 2>&1
echo ==== done %TIME% >> "%L%"
endlocal
