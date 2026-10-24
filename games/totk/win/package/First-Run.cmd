@echo off
rem Double-click: runs First-Run.ps1 in this folder (it asks for the game file and prod.keys).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0First-Run.ps1" %*
echo.
pause
