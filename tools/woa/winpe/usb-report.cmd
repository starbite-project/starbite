@echo off
rem Startup-folder launcher for the USB host report (docs/WIN11-USB.md section 9). Reads only.
rem Copy to C:\Users\starbite\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup\
rem with usb-report.ps1 in C:\Starbite\usb\. Delete both to undo.
start "" /min powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Starbite\usb\usb-report.ps1
