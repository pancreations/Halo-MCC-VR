@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Halo MCC VR mod - uninstaller

echo ==============================================
echo   Halo MCC VR mod - uninstaller
echo ==============================================
echo.

rem If this script sits inside an installed copy, remove that. Otherwise
rem search for the game the same way the installer does.
set "MODDIR="
if exist "%~dp0halo3xr.dll" set "MODDIR=%~dp0"

set "REL=steamapps\common\Halo The Master Chief Collection"
set "EXEREL=MCC\Binaries\Win64\MCC-Win64-Shipping.exe"

if not defined MODDIR (
    set "STEAMDIR="
    for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| findstr /i "SteamPath"') do set "STEAMDIR=%%b"
    if defined STEAMDIR set "STEAMDIR=!STEAMDIR:/=\!"
    if defined STEAMDIR call :TryLib "!STEAMDIR!"
    if not defined MODDIR if defined STEAMDIR if exist "!STEAMDIR!\steamapps\libraryfolders.vdf" (
        for /f usebackq^ tokens^=4^ delims^=^" %%p in (`findstr /c:"\"path\"" "!STEAMDIR!\steamapps\libraryfolders.vdf"`) do (
            set "LIB=%%p"
            set "LIB=!LIB:\\=\!"
            call :TryLib "!LIB!"
        )
    )
    if not defined MODDIR call :TryLib "C:\Program Files (x86)\Steam"
)

if not defined MODDIR (
    echo Nothing to remove: no installed copy of the mod was found.
    pause
    exit /b 0
)

rem strip a trailing backslash so quoting stays sane
if "!MODDIR:~-1!"=="\" set "MODDIR=!MODDIR:~0,-1!"

echo This removes the Halo MCC VR mod folder:
echo   !MODDIR!
echo and the "Halo MCC VR" desktop shortcut. No game files are touched.
echo.
set /p "OK=Continue? [y/n]: "
if /i not "!OK!"=="y" exit /b 0

for /f "usebackq delims=" %%d in (`powershell -NoProfile -Command "[Environment]::GetFolderPath('Desktop')"`) do set "DESKTOP=%%d"
if defined DESKTOP if exist "!DESKTOP!\Halo MCC VR.lnk" del "!DESKTOP!\Halo MCC VR.lnk"
rem also clear the shortcut name used by pre-rename test builds
if defined DESKTOP if exist "!DESKTOP!\Halo 3 VR.lnk" del "!DESKTOP!\Halo 3 VR.lnk"

echo Removing files... (this window may close by itself - that is normal)
cd /d "%TEMP%"
rd /s /q "!MODDIR!"
echo Done. The mod has been removed.
pause
exit /b 0

:TryLib
if defined MODDIR goto :eof
if exist "%~1\%REL%\halo3xr\halo3xr.dll" set "MODDIR=%~1\%REL%\halo3xr"
goto :eof
