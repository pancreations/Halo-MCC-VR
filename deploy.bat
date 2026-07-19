@echo off
setlocal
rem ============================================================================
rem deploy.bat - build halo3xr.dll and install it into the game folder, with
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
set OUT=%SRC%\build\Release\halo3xr.dll
set DST=N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\halo3xr\halo3xr.dll

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
%CMAKE% --build "%SRC%\build" --config Release --target halo3xr
if errorlevel 1 (
    echo *** FAILED: the build did not succeed. The game folder was NOT touched.
    goto :fail
)

echo [3/4] Copying the new DLL into the game folder...
copy /Y "%OUT%" "%DST%" >nul
if errorlevel 1 (
    echo *** FAILED: could not copy the DLL. Is the game really closed?
    goto :fail
)

echo [4/4] Verifying the installed file byte-for-byte...
"%SystemRoot%\System32\fc.exe" /b "%OUT%" "%DST%" >nul
if errorlevel 1 (
    echo *** FAILED: the installed DLL does not match the build output!
    goto :fail
)

echo.
echo ===== DEPLOYED OK =====
for %%F in ("%OUT%") do echo   build time: %%~tF
echo   The game log's first line ^("halo3xr M0 loaded ... build ..."^) must
echo   show this same date/time - that is the proof the new code is running.
if /I not "%~1"=="auto" pause
exit /b 0

:fail
echo.
echo ===== DEPLOY FAILED - DO NOT TEST. The old DLL may still be installed. =====
if /I not "%~1"=="auto" pause
exit /b 1
