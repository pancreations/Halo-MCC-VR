@echo off
setlocal EnableExtensions
title Halo MCC VR - export alpha test build

rem Builds a fresh Release and creates the exact folder/ZIP that can be copied
rem to another PC. Nothing is installed into MCC by this script.

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD_DIR=%ROOT%\build"
rem Release identity. This is a packaging label only: no compiled code reads it,
rem so bumping it never changes the DLL that testers run.
set "PKG_VER=0.1.3"

set "PACKAGE_LEAF=HaloMCCVR-alpha-%PKG_VER%"
set "PACKAGE_DIR=%ROOT%\dist\%PACKAGE_LEAF%"
set "ZIP_PATH=%ROOT%\dist\%PACKAGE_LEAF%.zip"
set "H3XR_PACKAGE_DIR=%PACKAGE_DIR%"
set "H3XR_ZIP_PATH=%ZIP_PATH%"
set "H3XR_VERSION_STRING=alpha %PKG_VER%"
set "H3XR_GIT_COMMIT=unknown"
for /f "usebackq delims=" %%H in (`git -C "%ROOT%" rev-parse --short^=12 HEAD 2^>nul`) do set "H3XR_GIT_COMMIT=%%H"
set "H3XR_GIT_DIRTY="
for /f "usebackq delims=" %%S in (`git -C "%ROOT%" status --porcelain 2^>nul`) do set "H3XR_GIT_DIRTY=-dirty"
if defined H3XR_GIT_DIRTY set "H3XR_GIT_COMMIT=%H3XR_GIT_COMMIT%%H3XR_GIT_DIRTY%"

echo ==============================================
echo   Halo MCC VR - %H3XR_VERSION_STRING% package exporter
echo ==============================================
echo.

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [ERROR] The CMake build folder has not been configured yet.
    echo         Follow the Build section in README.md once, then retry.
    goto :fail
)

rem A configured build tree must be driven by the same CMake that created it.
rem Mixing a newer PATH CMake with Visual Studio's bundled CMake can fail while
rem regenerating and can even leave old output files behind.
set "CMAKE_EXE="
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"CMAKE_COMMAND:INTERNAL=" "%BUILD_DIR%\CMakeCache.txt"') do set "CMAKE_EXE=%%B"
if not defined CMAKE_EXE for %%I in (cmake.exe) do set "CMAKE_EXE=%%~$PATH:I"
if not defined CMAKE_EXE if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKE_EXE if exist "C:\Program Files\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
if not defined CMAKE_EXE (
    echo [ERROR] CMake was not found.
    echo         Install CMake or Visual Studio 2022 with the C++ workload.
    goto :fail
)
if not exist "%CMAKE_EXE%" (
    echo [ERROR] The CMake recorded by the build folder no longer exists:
    echo         %CMAKE_EXE%
    goto :fail
)

echo [1/5] Building a fresh Release DLL and launcher...
rem Recompile the file containing the human-readable build stamp every time.
copy /b "%ROOT%\src\dll\dllmain.cpp"+,, "%ROOT%\src\dll\dllmain.cpp" >nul
rem Remove only the two exact package outputs. Their required reappearance is a
rem second guard against a build tool returning success after regeneration fails.
if /I not "%BUILD_DIR%"=="%ROOT%\build" (
    echo [ERROR] Internal build path safety check failed.
    goto :fail
)
if exist "%BUILD_DIR%\Release\halo3xr.dll" del /q "%BUILD_DIR%\Release\halo3xr.dll"
if exist "%BUILD_DIR%\Release\halo3xr_launcher.exe" del /q "%BUILD_DIR%\Release\halo3xr_launcher.exe"
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target halo3xr halo3xr_launcher
if errorlevel 1 (
    echo [ERROR] Build failed. No alpha package was produced.
    goto :fail
)
if not exist "%BUILD_DIR%\Release\halo3xr.dll" (
    echo [ERROR] Build returned without producing a fresh halo3xr.dll.
    goto :fail
)
if not exist "%BUILD_DIR%\Release\halo3xr_launcher.exe" (
    echo [ERROR] Build returned without producing a fresh halo3xr_launcher.exe.
    goto :fail
)

echo [2/5] Creating a clean portable folder...
if /I not "%PACKAGE_DIR%"=="%ROOT%\dist\%PACKAGE_LEAF%" (
    echo [ERROR] Internal package path safety check failed.
    goto :fail
)
if exist "%PACKAGE_DIR%" rd /s /q "%PACKAGE_DIR%"
if exist "%ZIP_PATH%" del /q "%ZIP_PATH%"
"%CMAKE_EXE%" --install "%BUILD_DIR%" --config Release --prefix "%PACKAGE_DIR%" --component dist
if errorlevel 1 (
    echo [ERROR] CMake could not assemble the portable folder.
    goto :fail
)

echo [3/5] Checking every required user file...
rem Install is manual: the package is the two binaries plus the two READMEs.
rem There is no installer or uninstaller script to verify (removed 2026-07-20).
for %%F in (halo3xr.dll halo3xr_launcher.exe ALPHA-README.txt MANUAL-README.txt) do (
    if not exist "%PACKAGE_DIR%\%%F" (
        echo [ERROR] Missing from package: %%F
        goto :fail
    )
)
for %%F in (ALPHA-README.txt MANUAL-README.txt) do (
    "%SystemRoot%\System32\fc.exe" /b "%ROOT%\installer\%%F" "%PACKAGE_DIR%\%%F" >nul
    if errorlevel 1 (
        echo [ERROR] Packaged %%F does not match its verified source file.
        goto :fail
    )
)
"%SystemRoot%\System32\fc.exe" /b "%BUILD_DIR%\Release\halo3xr.dll" "%PACKAGE_DIR%\halo3xr.dll" >nul
if errorlevel 1 (
    echo [ERROR] Packaged DLL does not match the fresh build.
    goto :fail
)
"%SystemRoot%\System32\fc.exe" /b "%BUILD_DIR%\Release\halo3xr_launcher.exe" "%PACKAGE_DIR%\halo3xr_launcher.exe" >nul
if errorlevel 1 (
    echo [ERROR] Packaged launcher does not match the fresh build.
    goto :fail
)

echo [4/5] Writing build identity and SHA-256 checksums...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop';" ^
  "$dll = Get-FileHash -LiteralPath (Join-Path $env:H3XR_PACKAGE_DIR 'halo3xr.dll') -Algorithm SHA256;" ^
  "$launcher = Get-FileHash -LiteralPath (Join-Path $env:H3XR_PACKAGE_DIR 'halo3xr_launcher.exe') -Algorithm SHA256;" ^
  "@(('Halo MCC VR ' + $env:H3XR_VERSION_STRING), ('Exported: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')), ('Git commit: ' + $env:H3XR_GIT_COMMIT), '', ('halo3xr.dll SHA-256: ' + $dll.Hash), ('halo3xr_launcher.exe SHA-256: ' + $launcher.Hash)) | Set-Content -LiteralPath (Join-Path $env:H3XR_PACKAGE_DIR 'BUILD-INFO.txt') -Encoding ASCII"
if errorlevel 1 (
    echo [ERROR] Could not write BUILD-INFO.txt.
    goto :fail
)

echo [5/5] Creating the take-to-another-PC ZIP...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop'; Compress-Archive -LiteralPath $env:H3XR_PACKAGE_DIR -DestinationPath $env:H3XR_ZIP_PATH -CompressionLevel Optimal -Force"
if errorlevel 1 (
    echo [ERROR] Could not create the ZIP file.
    goto :fail
)

echo.
echo ===== ALPHA EXPORT READY =====
echo.
echo Folder:
echo   %PACKAGE_DIR%
echo.
echo ZIP to copy to the laptop:
echo   %ZIP_PATH%
echo.
echo On the laptop, unzip the whole ZIP and follow MANUAL-README.txt: copy the
echo two files into a "Halo_MCC_VR" folder inside the game folder.
echo.
if /I not "%~1"=="auto" pause
exit /b 0

:fail
echo.
echo ===== EXPORT FAILED =====
echo No package should be tested unless all five steps say they passed.
if /I not "%~1"=="auto" pause
exit /b 1
