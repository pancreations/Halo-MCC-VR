@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Halo MCC VR alpha - installer

echo ==============================================
echo   Halo MCC VR - ALPHA TEST BUILD
echo   Installer for Halo 3 in MCC on Steam
echo ==============================================
echo.
echo This copies the mod into a "halo3xr" folder inside the game's install
echo folder and puts a "Halo MCC VR" shortcut on your desktop.
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

rem --- is an older copy of the mod already installed here? ---
set "UPGRADE="
if exist "!MODDIR!\halo3xr.dll" set "UPGRADE=1"
if exist "!MODDIR!\halo3xr_launcher.exe" set "UPGRADE=1"
if defined UPGRADE (
    echo An existing install of the mod was found in that folder.
    echo It will be updated in place: the mod files are replaced and your
    echo saved settings are kept. Game files are still untouched.
    echo.
)

rem --- the game must be closed, or the files are locked and the copy fails ---
tasklist /fi "imagename eq MCC-Win64-Shipping.exe" /nh 2>nul | findstr /i "MCC-Win64-Shipping.exe" >nul
if not errorlevel 1 (
    echo [ERROR] Halo: The Master Chief Collection is running right now.
    echo         Close the game completely, then run install.bat again.
    echo.
    pause
    exit /b 1
)

rem --- which settings file do we write the picture quality into? ---
rem The mod reads halomccvr.cfg, and falls back to the older halo3xr.cfg when
rem that is the only file present. Editing whichever one is already there keeps
rem every other setting the user has tuned.
set "CFGFILE=!MODDIR!\halomccvr.cfg"
if not exist "!MODDIR!\halomccvr.cfg" if exist "!MODDIR!\halo3xr.cfg" set "CFGFILE=!MODDIR!\halo3xr.cfg"

set "CURSCALE="
if exist "!CFGFILE!" for /f "usebackq tokens=2 delims==" %%v in (`findstr /r /i /c:"^ *resolution_scale *=" "!CFGFILE!" 2^>nul`) do set "CURSCALE=%%v"
if defined CURSCALE set "CURSCALE=!CURSCALE: =!"

rem =====================================================================
rem  picture quality
rem =====================================================================
echo ==============================================
echo   Choose your picture quality
echo ==============================================
echo.
echo How sharp the game renders inside your headset. Higher looks better but
echo needs a stronger graphics card. You can change this any time in-game
echo with F1 - it takes effect the next time you start the game.
echo.
echo   1 - Potato        50%%  - 1456 x 1050 - very weak PCs and laptops
echo   2 - Low           67%%  - 1952 x 1408 - safe starting point, most tested
echo   3 - Medium        80%%  - 2330 x 1680 - mid-range gaming PC
echo   4 - High         100%%  - 2912 x 2100 - strong graphics card
echo   5 - Ultra        110%%  - 3204 x 2310 - high-end graphics card
echo   6 - Keith David  150%%  - 4368 x 3150 - top-end only. Absurdly sharp.
echo.
if defined CURSCALE echo Press Enter on its own to keep your current setting: !CURSCALE!
if not defined CURSCALE echo Press Enter on its own for 2 - Low, the safest first try.
echo.

:askquality
set "SCALE="
set "SCALENAME="
set "PICK="
set /p "PICK=Pick 1-6: "
if not defined PICK goto :quality_default
if "!PICK!"=="1" (set "SCALE=0.50" & set "SCALENAME=Potato 50%%")
if "!PICK!"=="2" (set "SCALE=0.67" & set "SCALENAME=Low 67%%")
if "!PICK!"=="3" (set "SCALE=0.80" & set "SCALENAME=Medium 80%%")
if "!PICK!"=="4" (set "SCALE=1.00" & set "SCALENAME=High 100%%")
if "!PICK!"=="5" (set "SCALE=1.10" & set "SCALENAME=Ultra 110%%")
if "!PICK!"=="6" (set "SCALE=1.50" & set "SCALENAME=Keith David 150%%")
if not defined SCALE (
    echo Please type a single number from 1 to 6, then press Enter.
    goto :askquality
)
goto :quality_done

:quality_default
if defined CURSCALE (
    echo Keeping your current picture quality: !CURSCALE!
    goto :quality_done
)
set "SCALE=0.67"
set "SCALENAME=Low 67%%"

:quality_done
if defined SCALENAME echo Picture quality: !SCALENAME!
echo.

rem =====================================================================
rem  copy the mod in
rem =====================================================================
if not exist "!MODDIR!" mkdir "!MODDIR!"
copy /y "%~dp0halo3xr.dll" "!MODDIR!\" >nul || goto :copyfail
copy /y "%~dp0halo3xr_launcher.exe" "!MODDIR!\" >nul || goto :copyfail
if exist "%~dp0uninstall.bat" copy /y "%~dp0uninstall.bat" "!MODDIR!\" >nul
if exist "%~dp0ALPHA-README.txt" copy /y "%~dp0ALPHA-README.txt" "!MODDIR!\" >nul
if exist "%~dp0BUILD-INFO.txt" copy /y "%~dp0BUILD-INFO.txt" "!MODDIR!\" >nul
> "!MODDIR!\.halomccvr-installed" echo Halo MCC VR installer-owned directory

rem --- write only the resolution line, leaving every other setting alone ---
if defined SCALE (
    set "H3XR_CFG=!CFGFILE!"
    set "H3XR_SCALE=!SCALE!"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$p = $env:H3XR_CFG; $line = 'resolution_scale = ' + $env:H3XR_SCALE; if (Test-Path -LiteralPath $p) { $t = @(Get-Content -LiteralPath $p); if ($t -match '^\s*resolution_scale\s*=') { $t = $t -replace '^\s*resolution_scale\s*=.*$', $line } else { $t += $line }; Set-Content -LiteralPath $p -Value $t -Encoding ASCII } else { Set-Content -LiteralPath $p -Value @('# Halo MCC VR settings. Press F1 in game to change these.', '# A full game restart is needed after changing resolution_scale.', $line) -Encoding ASCII }"
    if errorlevel 1 (
        echo [WARNING] Could not save the picture quality setting.
        echo           The mod will start at its default. Press F1 in game to set it.
    )
)

rem --- desktop shortcut (works with OneDrive-redirected desktops too) ---
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ws = New-Object -ComObject WScript.Shell; $lnk = $ws.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Halo MCC VR.lnk')); $lnk.TargetPath = '!MODDIR!\halo3xr_launcher.exe'; $lnk.WorkingDirectory = '!MODDIR!'; $lnk.IconLocation = '!GAMEDIR!\!EXEREL!,0'; $lnk.Description = 'Halo MCC with the VR mod (anti-cheat off)'; $lnk.Save()" >nul
if errorlevel 1 (
    echo [WARNING] Could not create the desktop shortcut. You can still start the
    echo           mod by double-clicking: !MODDIR!\halo3xr_launcher.exe
)

echo.
echo ==============================================
if defined UPGRADE echo   Updated successfully.
if not defined UPGRADE echo   Installed successfully.
echo ==============================================
echo.
echo   REQUIRED MCC settings - set these in the game first:
echo    1. Settings ^> Video ^> Max Frame Rate ....... 120
echo    2. Settings ^> Video ^> V-Sync .............. Off
echo    3. Halo 3 ^> Settings ^> Field of View ...... 120
echo.
echo   Do NOT turn on FSR in the MCC video menu - it breaks the VR
echo   image scale. Use the picture quality setting instead.
echo.
echo   FOV 120 is the important one. Lower FOV makes the game stop
echo   drawing things at the edges, so scenery pops in and out in
echo   the headset. You can change all of this with the headset on,
echo   from inside the VR game.
echo.
echo   How to play in VR:
echo    1. Start Steam.
echo    2. Connect your headset and start SteamVR.
echo    3. Double-click the "Halo MCC VR" shortcut on your desktop.
echo.
echo   Notes:
echo    - Only that shortcut loads the mod (with anti-cheat off).
echo      Launching from Steam gives you the normal, unmodded game.
echo    - Press F1 in game for all settings, including picture quality.
echo      Picture quality only changes after you close and relaunch the game.
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
