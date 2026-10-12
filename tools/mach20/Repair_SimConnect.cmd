@echo off
setlocal EnableExtensions
title Repair FSX-SE SimConnect (official bundled MSI)
echo FSX MACH BOOST - SIMCONNECT INSTALLER LOCATOR
echo.
echo This script only uses SimConnect.msi included in your own FSX Steam Edition.
echo It will not download, copy, or register random DLL files.
echo.
set "FSXROOT=C:\Program Files (x86)\Steam\steamapps\common\FSX"
set "MSI=%FSXROOT%\SDK\Core Utilities Kit\SimConnect SDK\lib\SimConnect.msi"
if exist "%MSI%" goto Found
echo Default Steam path not found.
set /p "FSXROOT=Enter the FULL folder containing fsx_softth.exe (or fsx.exe): "
set "MSI=%FSXROOT%\SDK\Core Utilities Kit\SimConnect SDK\lib\SimConnect.msi"
if exist "%MSI%" goto Found
echo.
echo SimConnect.msi was not found at:
echo "%MSI%"
echo.
echo In Steam Library right-click Microsoft Flight Simulator X: Steam Edition,
echo choose Properties, Installed Files, Verify integrity of game files.
echo Then rerun this helper. If FSX is on another disk, enter that FSX folder.
pause
exit /b 2
:Found
echo Official FSX MSI found:
echo "%MSI%"
echo.
echo Exit FSX before installing SimConnect.
choice /C YN /M "Run Windows Installer now"
if errorlevel 2 exit /b 0
start "" /wait msiexec.exe /i "%MSI%"
echo.
echo Windows Installer finished. Reopen FSX, load a flight, then run FSX_Mach20Boost.exe.
pause
