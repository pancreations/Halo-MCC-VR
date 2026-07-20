@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Halo MCC VR mod - safe uninstaller

echo ==============================================
echo   Halo MCC VR mod - safe uninstaller
echo ==============================================
echo.

rem SAFETY RULES:
rem - A target is accepted only when its final folder name is exactly halo3xr,
rem   its parent is a real MCC install, and at least one mod binary is present.
rem - Only the explicit mod-owned files below are deleted.
rem - This script never performs a recursive directory delete.

set "REL=steamapps\common\Halo The Master Chief Collection"
set "EXEREL=MCC\Binaries\Win64\MCC-Win64-Shipping.exe"
set "MODDIR="

rem Prefer an installed copy only when this script is inside a validated
rem halo3xr folder. A ZIP extracted into the MCC root must never select that
rem root merely because halo3xr.dll is next to this script.
call :ValidateModDir "%~dp0"

if not defined MODDIR if /i not "!H3XR_TEST_DISABLE_DISCOVERY!"=="1" (
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
    echo Nothing was removed.
    echo No safely validated MCC\halo3xr mod folder was found.
    echo.
    echo If mod files were manually copied into the main game folder, remove
    echo only the files named in ALPHA-README.txt. Do not delete the game folder.
    pause
    exit /b 0
)

echo This removes only known Halo MCC VR files from:
echo   !MODDIR!
echo.
echo It does not recursively delete this folder or any MCC game files.
echo Unknown files are left untouched.
echo.
set /p "OK=Continue? [y/n]: "
if /i not "!OK!"=="y" exit /b 0

rem Revalidate immediately before deletion. Do not trust a stale path discovered
rem earlier in the script.
set "RECHECK=!MODDIR!"
set "MODDIR="
call :ValidateModDir "!RECHECK!"
if not defined MODDIR (
    echo.
    echo [SAFETY STOP] The mod folder failed revalidation. Nothing was removed.
    pause
    exit /b 1
)

if defined H3XR_TEST_DESKTOP set "DESKTOP=!H3XR_TEST_DESKTOP!"
if not defined DESKTOP for /f "usebackq delims=" %%d in (`powershell -NoProfile -Command "[Environment]::GetFolderPath('Desktop')"`) do set "DESKTOP=%%d"
if defined DESKTOP if exist "!DESKTOP!\Halo MCC VR.lnk" del /f /q "!DESKTOP!\Halo MCC VR.lnk"
if defined DESKTOP if exist "!DESKTOP!\Halo 3 VR.lnk" del /f /q "!DESKTOP!\Halo 3 VR.lnk"

echo Removing known mod files...
for %%F in (
    "halo3xr.dll"
    "halo3xr_launcher.exe"
    "ALPHA-README.txt"
    "BUILD-INFO.txt"
    ".halomccvr-installed"
    "halomccvr.cfg"
    "halo3xr.cfg"
    "halo3xr.log"
    "halo3xr_launcher.log"
) do (
    if exist "!MODDIR!\%%~F" del /f /q "!MODDIR!\%%~F"
)

rem The running batch file is removed by a detached process after this process
rem exits. The cleanup has no recursive flag: the validated halo3xr folder is removed
rem only when empty, and unknown files therefore force it to remain.
set "H3XR_CLEANUP_SELF=!MODDIR!\uninstall.bat"
set "H3XR_CLEANUP_DIR=!MODDIR!"
powershell -NoProfile -WindowStyle Hidden -Command "Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -ArgumentList '-NoProfile','-WindowStyle','Hidden','-Command','Start-Sleep -Milliseconds 1500; Remove-Item -LiteralPath $env:H3XR_CLEANUP_SELF -Force -ErrorAction SilentlyContinue; Remove-Item -LiteralPath $env:H3XR_CLEANUP_DIR -Force -ErrorAction SilentlyContinue'"

echo.
echo Done. Known mod files and shortcuts were removed.
echo If unknown files were present, the halo3xr folder was left in place.
exit /b 0

:TryLib
if defined MODDIR goto :eof
call :ValidateModDir "%~1\%REL%\halo3xr"
goto :eof

:ValidateModDir
if defined MODDIR goto :eof
set "H3XR_CANDIDATE=%~1"
set "VALIDATED="
for /f "usebackq delims=" %%d in (`powershell -NoProfile -Command "$m=[IO.Path]::GetFullPath($env:H3XR_CANDIDATE).TrimEnd([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar); if([IO.Path]::GetFileName($m) -ine 'halo3xr'){exit 10}; $g=[IO.Directory]::GetParent($m); if($null -eq $g){exit 11}; $exe=[IO.Path]::Combine($g.FullName,'MCC','Binaries','Win64','MCC-Win64-Shipping.exe'); if(-not [IO.File]::Exists($exe)){exit 12}; $dll=[IO.Path]::Combine($m,'halo3xr.dll'); $launcher=[IO.Path]::Combine($m,'halo3xr_launcher.exe'); if(-not ([IO.File]::Exists($dll) -or [IO.File]::Exists($launcher))){exit 13}; [Console]::Out.WriteLine($m)"`) do set "VALIDATED=%%d"
set "H3XR_CANDIDATE="
if defined VALIDATED set "MODDIR=!VALIDATED!"
goto :eof
