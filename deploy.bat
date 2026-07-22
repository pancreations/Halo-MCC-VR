@echo off
setlocal
rem ============================================================================
rem deploy.bat - build the DLL + launcher and install both into the game folder, with
rem every step VERIFIED. Created after the 2026-07-19 silent-deploy incident:
rem a bare "cmake" command failed (cmake is not on PATH), the copy then
rem re-deployed the STALE dll, and three headset sessions were burned testing
rem the same broken build. This script makes that impossible:
rem   1. refuses to run while the game is open
rem   2. uses the full cmake path and STOPS if the build fails
rem   3. copies, then byte-compares build output vs deployed file
rem   4. prints the build time, which must match the log's "M0 loaded" stamp
rem Run it by double-clicking, or "deploy.bat auto" to skip the final pause.
rem ============================================================================

set CMAKE="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set SRC=n:\dev\halo3-openxr
set DLL_OUT=%SRC%\build\Release\halo3xr.dll
set LAUNCHER_OUT=%SRC%\build\Release\halo3xr_launcher.exe
set DLL_DST=N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR\halo3xr.dll
set LAUNCHER_DST=N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR\halo3xr_launcher.exe

rem Public/normal deployment must never inherit a private ON value from the
rem persistent CMake cache. A reviewed headset experiment gets a separate,
rem explicit deployment mode later; this established path is stock-ODST only.
"%SystemRoot%\System32\findstr.exe" /X /C:"HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP:BOOL=OFF" "%SRC%\build\CMakeCache.txt" >nul
if errorlevel 1 (
    echo *** FAILED: the normal build cache is not explicitly configured with
    echo     HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP=OFF. Nothing was deployed.
    goto :fail
)

echo.
echo [1/4] Checking the game is closed...
"%SystemRoot%\System32\tasklist.exe" /FI "IMAGENAME eq MCC-Win64-Shipping.exe" | "%SystemRoot%\System32\find.exe" /I "MCC-Win64-Shipping.exe" >nul
if not errorlevel 1 (
    echo *** FAILED: the game is still running. Close it first
    echo     ^(Ctrl+Shift+Esc, find MCC-Win64-Shipping.exe, End task^).
    echo     Nothing was changed.
    goto :fail
)

echo [2/4] Building ^(dllmain touched so the build stamp is always fresh^)...
copy /b "%SRC%\src\dll\dllmain.cpp"+,, "%SRC%\src\dll\dllmain.cpp" >nul
%CMAKE% --build "%SRC%\build" --config Release --target halo3xr halo3xr_launcher
if errorlevel 1 (
    echo *** FAILED: the build did not succeed. The game folder was NOT touched.
    goto :fail
)

echo [3/4] Copying the new DLL and launcher into the game folder...
copy /Y "%LAUNCHER_OUT%" "%LAUNCHER_DST%" >nul
if errorlevel 1 (
    echo *** FAILED: could not copy the launcher. Is the game really closed?
    goto :fail
)
copy /Y "%DLL_OUT%" "%DLL_DST%" >nul
if errorlevel 1 (
    echo *** FAILED: could not copy the DLL. Is the game really closed?
    goto :fail
)

echo [4/4] Verifying both installed files byte-for-byte...
"%SystemRoot%\System32\fc.exe" /b "%DLL_OUT%" "%DLL_DST%" >nul
if errorlevel 1 (
    echo *** FAILED: the installed DLL does not match the build output!
    goto :fail
)
"%SystemRoot%\System32\fc.exe" /b "%LAUNCHER_OUT%" "%LAUNCHER_DST%" >nul
if errorlevel 1 (
    echo *** FAILED: the installed launcher does not match the build output!
    goto :fail
)

echo.
echo ===== DEPLOYED OK =====
for %%F in ("%DLL_OUT%") do echo   DLL build time: %%~tF
for %%F in ("%LAUNCHER_OUT%") do echo   launcher build time: %%~tF
echo   The game log's first line ^("HaloMCCVR loaded ... build ..."^) must
echo   show this same date/time - that is the proof the new code is running.
if /I not "%~1"=="auto" pause
exit /b 0

:fail
echo.
echo ===== DEPLOY FAILED - DO NOT TEST. An old file may still be installed. =====
if /I not "%~1"=="auto" pause
exit /b 1
