@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Halo 3 VR alpha - installer

echo ==============================================
echo   Halo 3 VR - ALPHA TEST BUILD
echo   Installer for Halo 3 in MCC on Steam
echo ==============================================
echo.
echo This copies the mod into a "halo3xr" folder inside the game's install
echo folder and puts a "Halo 3 VR" shortcut on your desktop.
echo No game files are modified.
echo This is an early test build. See ALPHA-README.txt for tested features,
echo requirements, known limitations, and the logs to send after a failure.
echo.

rem --- sanity: are the mod files next to this script? ---
if not exist "%~dp0halo3xr.dll" (
    echo [ERROR] halo3xr.dll was not found next to this script.
    echo         Run install.bat from the folder you unzipped, with all files intact.
    echo.
    pause
    exit /b 1
)
if not exist "%~dp0halo3xr_launcher.exe" (
    echo [ERROR] halo3xr_launcher.exe was not found next to this script.
    echo.
    pause
    exit /b 1
)

set "GAMEDIR="
set "REL=steamapps\common\Halo The Master Chief Collection"
set "EXEREL=MCC\Binaries\Win64\MCC-Win64-Shipping.exe"

rem --- 1) Steam's install path from the registry ---
set "STEAMDIR="
for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| findstr /i "SteamPath"') do set "STEAMDIR=%%b"
if defined STEAMDIR set "STEAMDIR=!STEAMDIR:/=\!"
if defined STEAMDIR call :TryLib "!STEAMDIR!"

rem --- 2) every extra Steam library drive listed in libraryfolders.vdf ---
if not defined GAMEDIR if defined STEAMDIR if exist "!STEAMDIR!\steamapps\libraryfolders.vdf" (
    for /f usebackq^ tokens^=4^ delims^=^" %%p in (`findstr /c:"\"path\"" "!STEAMDIR!\steamapps\libraryfolders.vdf"`) do (
        set "LIB=%%p"
        set "LIB=!LIB:\\=\!"
        call :TryLib "!LIB!"
    )
)

rem --- 3) common fallback location ---
if not defined GAMEDIR call :TryLib "C:\Program Files (x86)\Steam"

rem --- 4) ask the user ---
if not defined GAMEDIR (
    echo Could not find Halo: The Master Chief Collection automatically.
    echo.
    echo Paste the full path of the game folder below and press Enter.
    echo It is the folder that contains the "MCC" folder, usually like:
    echo   D:\SteamLibrary\steamapps\common\Halo The Master Chief Collection
    echo.
    set /p "GAMEDIR=Path: "
    if not exist "!GAMEDIR!\!EXEREL!" (
        echo.
        echo [ERROR] That folder does not contain !EXEREL!
        echo         Install not completed.
        pause
        exit /b 1
    )
)

echo Found the game at:
echo   !GAMEDIR!
echo.

set "MODDIR=!GAMEDIR!\halo3xr"
if not exist "!MODDIR!" mkdir "!MODDIR!"
copy /y "%~dp0halo3xr.dll" "!MODDIR!\" >nul || goto :copyfail
copy /y "%~dp0halo3xr_launcher.exe" "!MODDIR!\" >nul || goto :copyfail
if exist "%~dp0uninstall.bat" copy /y "%~dp0uninstall.bat" "!MODDIR!\" >nul

rem --- desktop shortcut (works with OneDrive-redirected desktops too) ---
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ws = New-Object -ComObject WScript.Shell; $lnk = $ws.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Halo 3 VR.lnk')); $lnk.TargetPath = '!MODDIR!\halo3xr_launcher.exe'; $lnk.WorkingDirectory = '!MODDIR!'; $lnk.IconLocation = '!GAMEDIR!\!EXEREL!,0'; $lnk.Description = 'Halo MCC with the VR mod (anti-cheat off)'; $lnk.Save()" >nul
if errorlevel 1 (
    echo [WARNING] Could not create the desktop shortcut. You can still start the
    echo           mod by double-clicking: !MODDIR!\halo3xr_launcher.exe
)

echo.
echo ==============================================
echo   Installed successfully.
echo ==============================================
echo.
echo   How to play in VR:
echo    1. Start Steam.
echo    2. Connect your headset and start SteamVR.
echo    3. Double-click the "Halo 3 VR" shortcut on your desktop.
echo.
echo   Notes:
echo    - Only that shortcut loads the mod (with anti-cheat off).
echo      Launching from Steam gives you the normal, unmodded game.
echo    - Test Halo 3 campaign first. ODST, online play, custom games,
echo      Forge, and other modes are not yet validated for this alpha.
echo    - Never use the mod in anti-cheat-enabled matchmaking.
echo    - To remove the mod completely, run uninstall.bat.
echo.
pause
exit /b 0

:copyfail
echo [ERROR] Could not copy the mod files into "!MODDIR!".
echo         Is the game running? Close it and run install.bat again.
pause
exit /b 1

:TryLib
if defined GAMEDIR goto :eof
if exist "%~1\%REL%\%EXEREL%" set "GAMEDIR=%~1\%REL%"
goto :eof
