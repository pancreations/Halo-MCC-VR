@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem ============================================================================
rem deploy-odst-private.bat
rem
rem Separate, opt-in deploy/restore path for the reviewed private ODST
rem camera/stereo/6DOF checkpoint. It deploys only the DLL, never launches MCC,
rem and never weakens or calls the public OFF-only deploy/export paths.
rem
rem Deploy:
rem   deploy-odst-private.bat I-APPROVE-ODST-TEST
rem Rebuild/test every gate without touching the game folder:
rem   deploy-odst-private.bat VERIFY-ODST-TEST
rem Restore the exact pre-test installed DLL:
rem   deploy-odst-private.bat RESTORE-ODST-BASELINE
rem
rem Add "auto" as argument 2 only for a reviewed non-interactive invocation.
rem ============================================================================

set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
set "SRC=N:\dev\halo3-openxr"
set "OFF_BUILD=%SRC%\build"
set "PRIVATE_BUILD=%SRC%\build-odst-private"
set "OFF_DLL=%OFF_BUILD%\Release\halo3xr.dll"
set "OFF_TEST=%OFF_BUILD%\Release\halomccvr_core_tests.exe"
set "PRIVATE_DLL=%PRIVATE_BUILD%\Release\halo3xr.dll"
set "PRIVATE_TEST=%PRIVATE_BUILD%\Release\halomccvr_core_tests.exe"
set "DEST_DIR=N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR"
set "DLL_DST=%DEST_DIR%\halo3xr.dll"
set "LAUNCHER_DST=%DEST_DIR%\halo3xr_launcher.exe"
set "ODST_DLL=N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\halo3odst\halo3odst.dll"
set "BACKUP_DIR=%DEST_DIR%\pre-odst-private-backup"
set "BASELINE_DLL=%BACKUP_DIR%\halo3xr.dll"
set "STAGED_PRIVATE_DLL=%BACKUP_DIR%\halo3xr.odst-private.dll"
set "STATE_PREPARED=%BACKUP_DIR%\PREPARED.txt"
set "STATE_DEPLOYING=%BACKUP_DIR%\DEPLOYING.txt"
set "STATE_ACTIVE=%BACKUP_DIR%\ACTIVE.txt"
set "STATE_RESTORING=%BACKUP_DIR%\RESTORING.txt"
set "STATUS_SNAPSHOT=%TEMP%\HaloMCCVR-ODST-status-%RANDOM%-%RANDOM%.txt"
set "EXPECTED_ODST_SHA=5BB20976EFDFD9E1CE59C589339804725FEC239021027C8D65B2733EAB94829A"
set "EXPECTED_BASELINE_DLL_SHA=0BD0233CD28975CADFCE7E03F9B9CA353CD533CD37D257FDCA362983D00B11BA"
set "EXPECTED_LAUNCHER_SHA=BDC0A20F56DF72CDDE68E5D0AB621321FBDE91DA427B6C24142B38336D33EA6D"
set "VERIFY_ONLY=0"
set "TEST_BRANCH="
set "TEST_COMMIT="

if /I "%~1"=="RESTORE-ODST-BASELINE" goto :restore
if /I "%~1"=="VERIFY-ODST-TEST" (
    set "VERIFY_ONLY=1"
) else (
    if /I not "%~1"=="I-APPROVE-ODST-TEST" goto :usage
)

echo.
echo [1/9] Verifying the reviewed source checkpoint...
if not exist "%CMAKE%" (
    echo *** FAILED: Visual Studio's bundled CMake was not found.
    goto :fail
)
if not exist "%CTEST%" (
    echo *** FAILED: Visual Studio's bundled CTest was not found.
    goto :fail
)
if not exist "%POWERSHELL%" (
    echo *** FAILED: Windows PowerShell was not found.
    goto :fail
)
git -C "%SRC%" rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
    echo *** FAILED: %SRC% is not a readable git worktree.
    goto :fail
)
git -C "%SRC%" merge-base --is-ancestor 7c25a1a HEAD >nul 2>&1
if errorlevel 1 (
    echo *** FAILED: HEAD does not contain reviewed ODST checkpoint 7c25a1a.
    goto :fail
)
git -C "%SRC%" branch --show-current >nul 2>&1
if errorlevel 1 (
    echo *** FAILED: the current branch could not be read.
    goto :fail
)
for /f "delims=" %%B in ('git -C "%SRC%" branch --show-current') do set "TEST_BRANCH=%%B"
if /I not "%TEST_BRANCH%"=="feature/odst-bringup" (
    echo *** FAILED: private testing is restricted to feature/odst-bringup.
    goto :fail
)
git -C "%SRC%" status --porcelain >"%STATUS_SNAPSHOT%" 2>nul
if errorlevel 1 (
    echo *** FAILED: git status could not verify the worktree.
    goto :fail
)
for /f "usebackq delims=" %%S in ("%STATUS_SNAPSHOT%") do (
    del /Q "%STATUS_SNAPSHOT%" >nul 2>&1
    echo *** FAILED: the worktree is not clean. Commit or remove unrelated
    echo     changes before producing a headset-test binary.
    goto :fail
)
del /Q "%STATUS_SNAPSHOT%" >nul 2>&1
git -C "%SRC%" rev-parse --verify HEAD >nul 2>&1
if errorlevel 1 (
    echo *** FAILED: HEAD could not be resolved.
    goto :fail
)
for /f "delims=" %%H in ('git -C "%SRC%" rev-parse HEAD') do set "TEST_COMMIT=%%H"
if not defined TEST_COMMIT (
    echo *** FAILED: the source commit identity is empty.
    goto :fail
)
echo   Test commit: %TEST_COMMIT%

echo [2/9] Verifying the retail ODST module and installed baseline...
call :verify_predeploy_identities
if errorlevel 1 goto :fail

echo [3/9] Confirming MCC and the launcher are closed...
call :require_game_closed
if errorlevel 1 goto :fail

echo [4/9] Configuring isolated OFF and ON build trees...
"%CMAKE%" -S "%SRC%" -B "%OFF_BUILD%" -A x64 -DBUILD_TESTING:BOOL=ON -DHALOMCCVR_EXPERIMENTAL_ODST_BRINGUP:BOOL=OFF
if errorlevel 1 (
    echo *** FAILED: normal OFF CMake configuration did not succeed.
    goto :fail
)
"%CMAKE%" -S "%SRC%" -B "%PRIVATE_BUILD%" -A x64 -DBUILD_TESTING:BOOL=ON -DHALOMCCVR_EXPERIMENTAL_ODST_BRINGUP:BOOL=ON
if errorlevel 1 (
    echo *** FAILED: private ON CMake configuration did not succeed.
    goto :fail
)
call :verify_cache "%OFF_BUILD%\CMakeCache.txt" OFF
if errorlevel 1 goto :fail
call :verify_cache "%PRIVATE_BUILD%\CMakeCache.txt" ON
if errorlevel 1 goto :fail

echo [5/9] Building fresh OFF and ON Release targets...
"%CMAKE%" --build "%OFF_BUILD%" --config Release --clean-first --target halo3xr halomccvr_core_tests
if errorlevel 1 (
    echo *** FAILED: the normal OFF Release build did not succeed.
    goto :fail
)
"%CMAKE%" --build "%PRIVATE_BUILD%" --config Release --clean-first --target halo3xr halomccvr_core_tests
if errorlevel 1 (
    echo *** FAILED: the private ON Release build did not succeed.
    goto :fail
)
if not exist "%OFF_DLL%" (
    echo *** FAILED: the expected normal OFF DLL output is missing.
    goto :fail
)
if not exist "%OFF_TEST%" (
    echo *** FAILED: the expected normal OFF test output is missing.
    goto :fail
)
if not exist "%PRIVATE_DLL%" (
    echo *** FAILED: the expected private DLL output is missing.
    goto :fail
)
if not exist "%PRIVATE_TEST%" (
    echo *** FAILED: the expected private test output is missing.
    goto :fail
)

echo [6/9] Running the named OFF and ON core tests...
"%CTEST%" --test-dir "%OFF_BUILD%" -C Release --output-on-failure --no-tests=error -R "^halomccvr_core_tests$"
if errorlevel 1 (
    echo *** FAILED: normal OFF CTest did not pass. Nothing was deployed.
    goto :fail
)
"%CTEST%" --test-dir "%PRIVATE_BUILD%" -C Release --output-on-failure --no-tests=error -R "^halomccvr_core_tests$"
if errorlevel 1 (
    echo *** FAILED: private ON CTest did not pass. Nothing was deployed.
    goto :fail
)

echo [7/9] Revalidating every gate and preserving the exact installed DLL...
call :require_game_closed
if errorlevel 1 goto :fail
call :verify_source_unchanged
if errorlevel 1 goto :fail
call :verify_cache "%OFF_BUILD%\CMakeCache.txt" OFF
if errorlevel 1 goto :fail
call :verify_cache "%PRIVATE_BUILD%\CMakeCache.txt" ON
if errorlevel 1 goto :fail
call :verify_predeploy_identities
if errorlevel 1 goto :fail
if "%VERIFY_ONLY%"=="1" goto :verified
if exist "%BACKUP_DIR%" (
    echo *** FAILED: %BACKUP_DIR% already exists.
    echo     Preserve and inspect that original baseline/ACTIVE marker.
    goto :fail
)
mkdir "%BACKUP_DIR%" >nul 2>&1
if errorlevel 1 (
    echo *** FAILED: could not create the recovery backup directory.
    goto :fail
)
copy /Y "%DLL_DST%" "%BASELINE_DLL%" >nul
if errorlevel 1 goto :backup_fail
"%SystemRoot%\System32\fc.exe" /b "%DLL_DST%" "%BASELINE_DLL%" >nul
if errorlevel 1 goto :backup_fail
"%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; if ((Get-FileHash -LiteralPath '%BASELINE_DLL%' -Algorithm SHA256).Hash -cne '%EXPECTED_BASELINE_DLL_SHA%') { throw 'backup hash mismatch' }"
if errorlevel 1 goto :backup_fail

echo [8/9] Staging, deploying, and byte-verifying only the private DLL...
copy /Y "%PRIVATE_DLL%" "%STAGED_PRIVATE_DLL%" >nul
if errorlevel 1 goto :stage_fail
"%SystemRoot%\System32\fc.exe" /b "%PRIVATE_DLL%" "%STAGED_PRIVATE_DLL%" >nul
if errorlevel 1 goto :stage_fail
"%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $candidate=(Get-FileHash -LiteralPath '%STAGED_PRIVATE_DLL%' -Algorithm SHA256).Hash; $item=Get-Item -LiteralPath '%STAGED_PRIVATE_DLL%'; $manifest=@('HaloMCCVR private ODST test','Source commit: %TEST_COMMIT%','Baseline DLL SHA-256: %EXPECTED_BASELINE_DLL_SHA%','Private DLL SHA-256: '+$candidate,'Private DLL size: '+$item.Length,'Private DLL UTC time: '+$item.LastWriteTimeUtc.ToString('O'),'Unchanged launcher SHA-256: %EXPECTED_LAUNCHER_SHA%','Retail ODST SHA-256: %EXPECTED_ODST_SHA%'); Set-Content -LiteralPath '%BACKUP_DIR%\MANIFEST.txt' -Value $manifest -Encoding ASCII; Set-Content -LiteralPath '%BACKUP_DIR%\BASELINE-SHA256.txt' -Value '%EXPECTED_BASELINE_DLL_SHA%' -Encoding ASCII; Set-Content -LiteralPath '%BACKUP_DIR%\PRIVATE-SHA256.txt' -Value $candidate -Encoding ASCII; Set-Content -LiteralPath '%BACKUP_DIR%\PRIVATE-SIZE.txt' -Value $item.Length -Encoding ASCII; Set-Content -LiteralPath '%BACKUP_DIR%\PRIVATE-UTC.txt' -Value $item.LastWriteTimeUtc.ToString('O') -Encoding ASCII; Set-Content -LiteralPath '%BACKUP_DIR%\PREPARED.tmp' -Value ('commit=%TEST_COMMIT%;private='+$candidate) -Encoding ASCII; Move-Item -LiteralPath '%BACKUP_DIR%\PREPARED.tmp' -Destination '%STATE_PREPARED%' -ErrorAction Stop; if ((Get-Content -LiteralPath '%BACKUP_DIR%\PRIVATE-SHA256.txt' -Raw).Trim() -cne $candidate) { throw 'candidate record verification failed' }"
if errorlevel 1 goto :stage_fail
call :verify_recovery_record PREPARED
if errorlevel 1 goto :stage_fail
call :require_game_closed
if errorlevel 1 goto :stage_fail
call :verify_source_unchanged
if errorlevel 1 goto :stage_fail
call :verify_cache "%OFF_BUILD%\CMakeCache.txt" OFF
if errorlevel 1 goto :stage_fail
call :verify_cache "%PRIVATE_BUILD%\CMakeCache.txt" ON
if errorlevel 1 goto :stage_fail
call :verify_predeploy_identities
if errorlevel 1 goto :stage_fail
call :verify_recovery_record PREPARED
if errorlevel 1 goto :stage_fail
call :require_game_closed
if errorlevel 1 goto :stage_fail
move /Y "%STATE_PREPARED%" "%STATE_DEPLOYING%" >nul
if errorlevel 1 goto :stage_fail
call :verify_recovery_record DEPLOYING
if errorlevel 1 goto :deploy_fail
copy /Y "%STAGED_PRIVATE_DLL%" "%DLL_DST%" >nul
if errorlevel 1 goto :deploy_fail
"%SystemRoot%\System32\fc.exe" /b "%STAGED_PRIVATE_DLL%" "%DLL_DST%" >nul
if errorlevel 1 goto :deploy_fail
"%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $expected=(Get-Content -LiteralPath '%BACKUP_DIR%\PRIVATE-SHA256.txt' -Raw).Trim(); if ((Get-FileHash -LiteralPath '%DLL_DST%' -Algorithm SHA256).Hash -cne $expected) { throw 'deployed hash mismatch' }"
if errorlevel 1 goto :deploy_fail
call :verify_recovery_record DEPLOYING
if errorlevel 1 goto :deploy_fail
call :require_game_closed
if errorlevel 1 goto :deploy_fail
move /Y "%STATE_DEPLOYING%" "%STATE_ACTIVE%" >nul
if errorlevel 1 goto :deploy_fail
call :verify_recovery_record ACTIVE
if errorlevel 1 goto :deploy_fail

echo [9/9] Recording the deployed identity...
echo.
echo ===== PRIVATE ODST DLL DEPLOYED - MCC NOT LAUNCHED =====
echo   Source commit: %TEST_COMMIT%
echo   Private DLL UTC timestamp:
type "%BACKUP_DIR%\PRIVATE-UTC.txt"
if errorlevel 1 goto :deploy_fail
echo   Private DLL size:
type "%BACKUP_DIR%\PRIVATE-SIZE.txt"
if errorlevel 1 goto :deploy_fail
echo   Private DLL SHA-256:
type "%BACKUP_DIR%\PRIVATE-SHA256.txt"
if errorlevel 1 goto :deploy_fail
echo   Launcher unchanged: %EXPECTED_LAUNCHER_SHA%
echo   Retail ODST verified: %EXPECTED_ODST_SHA%
echo   Exact baseline backup: %BACKUP_DIR%
echo.
echo   Launch manually with anti-cheat disabled and perform the staged checklist.
echo   On any failure, close MCC and run:
echo     deploy-odst-private.bat RESTORE-ODST-BASELINE
echo   After a successful ODST checklist, regression-test Halo 3 while this
echo   private DLL is still installed, then restore the exact baseline.
if /I not "%~2"=="auto" pause
exit /b 0

:restore
echo.
echo Restoring the exact pre-ODST-test installed DLL...
call :require_game_closed
if errorlevel 1 goto :restore_fail
call :prepare_restore_state
if errorlevel 1 goto :restore_fail
copy /Y "%BASELINE_DLL%" "%DLL_DST%" >nul
if errorlevel 1 goto :restore_fail
"%SystemRoot%\System32\fc.exe" /b "%BASELINE_DLL%" "%DLL_DST%" >nul
if errorlevel 1 goto :restore_fail
"%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; if ((Get-FileHash -LiteralPath '%DLL_DST%' -Algorithm SHA256).Hash -cne '%EXPECTED_BASELINE_DLL_SHA%') { throw 'restored DLL hash mismatch' }"
if errorlevel 1 goto :restore_fail
move /Y "%STATE_RESTORING%" "%BACKUP_DIR%\RESTORED.txt" >nul
if errorlevel 1 goto :restore_fail
if exist "%STATE_RESTORING%" goto :restore_fail
if not exist "%BACKUP_DIR%\RESTORED.txt" goto :restore_fail
echo.
echo ===== EXACT PRE-TEST DLL RESTORED AND BYTE-VERIFIED =====
echo   DLL SHA-256: %EXPECTED_BASELINE_DLL_SHA%
echo   The launcher was never changed.
echo   Preserve %BACKUP_DIR% as the test record.
if /I not "%~2"=="auto" pause
exit /b 0

:verified
echo.
echo ===== PRIVATE ODST DESK GATES VERIFIED - NOTHING DEPLOYED =====
echo   Source commit: %TEST_COMMIT%
echo   OFF and ON Release builds/tests passed.
echo   Retail ODST and installed baseline identities matched.
echo   Re-run with I-APPROVE-ODST-TEST to create the backup and deploy the DLL.
if /I not "%~2"=="auto" pause
exit /b 0

:verify_cache
"%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $lines=@(Get-Content -LiteralPath '%~1'); $required=@('HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP:BOOL=%~2','BUILD_TESTING:BOOL=ON','CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022','CMAKE_GENERATOR_PLATFORM:INTERNAL=x64','CMAKE_HOME_DIRECTORY:INTERNAL=N:/dev/halo3-openxr','CMAKE_COMMAND:INTERNAL=C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe','CMAKE_CTEST_COMMAND:INTERNAL=C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe'); foreach ($requiredLine in $required) { if (@($lines | Where-Object { $_ -ceq $requiredLine }).Count -ne 1) { throw ('cache identity mismatch: '+$requiredLine) } }"
if errorlevel 1 (
    echo *** FAILED: cache identity/uniqueness check failed for %~1.
    exit /b 1
)
exit /b 0

:verify_source_unchanged
git -C "%SRC%" branch --show-current >nul 2>&1
if errorlevel 1 (
    echo *** FAILED: the current branch can no longer be read.
    exit /b 1
)
git -C "%SRC%" branch --show-current | "%SystemRoot%\System32\findstr.exe" /X /C:"feature/odst-bringup" >nul
if errorlevel 1 (
    echo *** FAILED: the source branch changed during preparation.
    exit /b 1
)
git -C "%SRC%" rev-parse --verify HEAD >nul 2>&1
if errorlevel 1 (
    echo *** FAILED: HEAD can no longer be resolved.
    exit /b 1
)
git -C "%SRC%" rev-parse HEAD | "%SystemRoot%\System32\findstr.exe" /X /C:"%TEST_COMMIT%" >nul
if errorlevel 1 (
    echo *** FAILED: HEAD changed during preparation.
    exit /b 1
)
git -C "%SRC%" status --porcelain >"%STATUS_SNAPSHOT%" 2>nul
if errorlevel 1 (
    echo *** FAILED: git status can no longer verify the worktree.
    exit /b 1
)
for /f "usebackq delims=" %%S in ("%STATUS_SNAPSHOT%") do (
    del /Q "%STATUS_SNAPSHOT%" >nul 2>&1
    echo *** FAILED: the worktree changed during preparation.
    exit /b 1
)
del /Q "%STATUS_SNAPSHOT%" >nul 2>&1
exit /b 0

:verify_predeploy_identities
if not exist "%ODST_DLL%" (
    echo *** FAILED: the expected retail halo3odst.dll is missing.
    exit /b 1
)
if not exist "%DLL_DST%" (
    echo *** FAILED: the headset-confirmed installed DLL is missing.
    exit /b 1
)
if not exist "%LAUNCHER_DST%" (
    echo *** FAILED: the headset-confirmed installed launcher is missing.
    exit /b 1
)
"%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; if ((Get-FileHash -LiteralPath '%ODST_DLL%' -Algorithm SHA256).Hash -cne '%EXPECTED_ODST_SHA%') { throw 'halo3odst.dll hash mismatch' }; if ((Get-FileHash -LiteralPath '%DLL_DST%' -Algorithm SHA256).Hash -cne '%EXPECTED_BASELINE_DLL_SHA%') { throw 'installed baseline DLL hash mismatch' }; if ((Get-FileHash -LiteralPath '%LAUNCHER_DST%' -Algorithm SHA256).Hash -cne '%EXPECTED_LAUNCHER_SHA%') { throw 'installed launcher hash mismatch' }"
if errorlevel 1 (
    echo *** FAILED: retail or installed baseline identity mismatch.
    exit /b 1
)
exit /b 0

:verify_recovery_record
set "EXPECTED_STATE_FILE="
if /I "%~1"=="PREPARED" set "EXPECTED_STATE_FILE=%STATE_PREPARED%"
if /I "%~1"=="DEPLOYING" set "EXPECTED_STATE_FILE=%STATE_DEPLOYING%"
if /I "%~1"=="ACTIVE" set "EXPECTED_STATE_FILE=%STATE_ACTIVE%"
if /I "%~1"=="RESTORING" set "EXPECTED_STATE_FILE=%STATE_RESTORING%"
if not defined EXPECTED_STATE_FILE (
    echo *** FAILED: invalid recovery state request %~1.
    exit /b 1
)
if not exist "%EXPECTED_STATE_FILE%" (
    echo *** FAILED: expected recovery state %~1 is missing.
    exit /b 1
)
if not exist "%BACKUP_DIR%\MANIFEST.txt" (
    echo *** FAILED: the private-test manifest is missing.
    exit /b 1
)
if not exist "%BASELINE_DLL%" (
    echo *** FAILED: the preserved baseline DLL is missing.
    exit /b 1
)
if not exist "%STAGED_PRIVATE_DLL%" (
    echo *** FAILED: the staged private DLL is missing.
    exit /b 1
)
"%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $live=@('%STATE_PREPARED%','%STATE_DEPLOYING%','%STATE_ACTIVE%','%STATE_RESTORING%'); $terminal=@('%BACKUP_DIR%\RESTORED.txt','%BACKUP_DIR%\ROLLED_BACK.txt','%BACKUP_DIR%\PREPARE_FAILED.txt'); if (@($live | Where-Object { Test-Path -LiteralPath $_ }).Count -ne 1) { throw 'live-state count is not one' }; if (@($terminal | Where-Object { Test-Path -LiteralPath $_ }).Count -ne 0) { throw 'terminal marker coexists with live state' }; $baselineRecord=(Get-Content -LiteralPath '%BACKUP_DIR%\BASELINE-SHA256.txt' -Raw).Trim(); $privateRecord=(Get-Content -LiteralPath '%BACKUP_DIR%\PRIVATE-SHA256.txt' -Raw).Trim(); $sizeRecord=[int64](Get-Content -LiteralPath '%BACKUP_DIR%\PRIVATE-SIZE.txt' -Raw).Trim(); $timeRecord=(Get-Content -LiteralPath '%BACKUP_DIR%\PRIVATE-UTC.txt' -Raw).Trim(); $state=(Get-Content -LiteralPath '%EXPECTED_STATE_FILE%' -Raw).Trim(); if (-not ($state -cmatch '^commit=([0-9a-f]{40}|[0-9a-f]{64});private=([0-9A-F]{64})$')) { throw 'state marker malformed' }; $commit=$Matches[1]; $stateHash=$Matches[2]; if ($baselineRecord -cne '%EXPECTED_BASELINE_DLL_SHA%') { throw 'baseline record mismatch' }; if ($privateRecord -cnotmatch '^[0-9A-F]{64}$') { throw 'private hash record malformed' }; if ($stateHash -cne $privateRecord) { throw 'state/private hash mismatch' }; if ((Get-FileHash -LiteralPath '%BASELINE_DLL%' -Algorithm SHA256).Hash -cne $baselineRecord) { throw 'baseline backup hash mismatch' }; if ((Get-FileHash -LiteralPath '%STAGED_PRIVATE_DLL%' -Algorithm SHA256).Hash -cne $privateRecord) { throw 'staged private hash mismatch' }; $item=Get-Item -LiteralPath '%STAGED_PRIVATE_DLL%'; if ($item.Length -ne $sizeRecord) { throw 'staged private size mismatch' }; if ($item.LastWriteTimeUtc.ToString('O') -cne $timeRecord) { throw 'staged private time mismatch' }; $manifest=@(Get-Content -LiteralPath '%BACKUP_DIR%\MANIFEST.txt'); $expected=@('HaloMCCVR private ODST test','Source commit: '+$commit,'Baseline DLL SHA-256: '+$baselineRecord,'Private DLL SHA-256: '+$privateRecord,'Private DLL size: '+$sizeRecord,'Private DLL UTC time: '+$timeRecord,'Unchanged launcher SHA-256: %EXPECTED_LAUNCHER_SHA%','Retail ODST SHA-256: %EXPECTED_ODST_SHA%'); if ($manifest.Count -ne $expected.Count) { throw 'manifest line count mismatch' }; for ($i=0; $i -lt $expected.Count; $i++) { if ($manifest[$i] -cne $expected[$i]) { throw ('manifest mismatch at line '+($i+1)) } }"
if errorlevel 1 (
    echo *** FAILED: the recovery/candidate record is incomplete or inconsistent.
    exit /b 1
)
exit /b 0

:prepare_restore_state
set "CURRENT_STATE_FILE="
set "CURRENT_STATE_NAME="
set "RESTORE_POLICY="
if exist "%STATE_RESTORING%" (
    set "CURRENT_STATE_FILE=%STATE_RESTORING%"
    set "CURRENT_STATE_NAME=RESTORING"
    set "RESTORE_POLICY=UNKNOWN"
    goto :prs_validate
)
if exist "%STATE_DEPLOYING%" (
    set "CURRENT_STATE_FILE=%STATE_DEPLOYING%"
    set "CURRENT_STATE_NAME=DEPLOYING"
    set "RESTORE_POLICY=UNKNOWN"
    goto :prs_validate
)
if exist "%STATE_ACTIVE%" (
    set "CURRENT_STATE_FILE=%STATE_ACTIVE%"
    set "CURRENT_STATE_NAME=ACTIVE"
    set "RESTORE_POLICY=ACTIVE"
    goto :prs_validate
)
if exist "%STATE_PREPARED%" (
    set "CURRENT_STATE_FILE=%STATE_PREPARED%"
    set "CURRENT_STATE_NAME=PREPARED"
    set "RESTORE_POLICY=PREPARED"
    goto :prs_validate
)
echo *** FAILED: no recoverable PREPARED/DEPLOYING/ACTIVE/RESTORING state exists.
exit /b 1

:prs_validate
call :verify_recovery_record %CURRENT_STATE_NAME%
if errorlevel 1 exit /b 1
"%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; if ((Get-FileHash -LiteralPath '%LAUNCHER_DST%' -Algorithm SHA256).Hash -cne '%EXPECTED_LAUNCHER_SHA%') { throw 'installed launcher hash mismatch' }"
if errorlevel 1 exit /b 1
if /I "%RESTORE_POLICY%"=="ACTIVE" (
    "%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; $private=(Get-Content -LiteralPath '%BACKUP_DIR%\PRIVATE-SHA256.txt' -Raw).Trim(); $current=(Get-FileHash -LiteralPath '%DLL_DST%' -Algorithm SHA256).Hash; if ($current -cne $private -and $current -cne '%EXPECTED_BASELINE_DLL_SHA%') { throw ('refusing unrelated ACTIVE DLL '+$current) }"
    if errorlevel 1 exit /b 1
)
if /I "%RESTORE_POLICY%"=="PREPARED" (
    "%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; if ((Get-FileHash -LiteralPath '%DLL_DST%' -Algorithm SHA256).Hash -cne '%EXPECTED_BASELINE_DLL_SHA%') { throw 'PREPARED state requires untouched baseline' }"
    if errorlevel 1 exit /b 1
)
if /I "%CURRENT_STATE_NAME%"=="RESTORING" exit /b 0
move /Y "%CURRENT_STATE_FILE%" "%STATE_RESTORING%" >nul
if errorlevel 1 exit /b 1
call :verify_recovery_record RESTORING
if errorlevel 1 exit /b 1
exit /b 0

:require_game_closed
if not exist "%POWERSHELL%" (
    echo *** FAILED: Windows PowerShell is unavailable; process state is unknown.
    exit /b 1
)
"%POWERSHELL%" -NoProfile -NonInteractive -Command "try { $running=@(Get-Process -ErrorAction Stop | Where-Object { $_.ProcessName -in @('MCC-Win64-Shipping','halo3xr_launcher') }); if ($running.Count -ne 0) { $running | ForEach-Object { Write-Output ('running: ' + $_.ProcessName + ' pid=' + $_.Id) }; exit 1 } } catch { Write-Output ('process query failed: ' + $_.Exception.Message); exit 2 }"
if errorlevel 1 (
    echo *** FAILED: MCC/launcher absence could not be proved.
    exit /b 1
)
exit /b 0

:usage
echo.
echo PRIVATE ODST TEST DEPLOYMENT WAS NOT AUTHORIZED.
echo This script is inert unless a reviewed token is supplied:
echo.
echo   deploy-odst-private.bat I-APPROVE-ODST-TEST
echo   deploy-odst-private.bat VERIFY-ODST-TEST
echo   deploy-odst-private.bat RESTORE-ODST-BASELINE
echo.
echo It deploys/restores only the DLL and never launches MCC.
if /I not "%~2"=="auto" pause
exit /b 2

:backup_fail
echo *** FAILED: the installed baseline could not be backed up and verified.
echo     No private DLL was copied.
goto :fail

:stage_fail
echo *** FAILED: private staging or final pre-copy validation failed.
echo     No private DLL was copied.
if exist "%STATE_PREPARED%" (
    move /Y "%STATE_PREPARED%" "%BACKUP_DIR%\PREPARE_FAILED.txt" >nul
    if errorlevel 1 echo *** WARNING: PREPARED marker remains; inspect it manually.
)
goto :fail

:deploy_fail
echo *** FAILED: private deployment or verification failed.
echo     Attempting verified rollback to the preserved baseline...
call :require_game_closed
if errorlevel 1 goto :rollback_unproven
call :prepare_restore_state
if errorlevel 1 goto :rollback_unproven
copy /Y "%BASELINE_DLL%" "%DLL_DST%" >nul
if errorlevel 1 goto :rollback_unproven
"%SystemRoot%\System32\fc.exe" /b "%BASELINE_DLL%" "%DLL_DST%" >nul
if errorlevel 1 goto :rollback_unproven
"%POWERSHELL%" -NoProfile -NonInteractive -Command "$ErrorActionPreference='Stop'; if ((Get-FileHash -LiteralPath '%DLL_DST%' -Algorithm SHA256).Hash -cne '%EXPECTED_BASELINE_DLL_SHA%') { throw 'automatic rollback hash mismatch' }"
if errorlevel 1 goto :rollback_unproven
move /Y "%STATE_RESTORING%" "%BACKUP_DIR%\ROLLED_BACK.txt" >nul
if errorlevel 1 goto :rollback_unproven
if exist "%STATE_RESTORING%" goto :rollback_unproven
if not exist "%BACKUP_DIR%\ROLLED_BACK.txt" goto :rollback_unproven
echo *** Exact baseline rollback completed and was byte/hash verified.
goto :fail

:rollback_unproven
echo *** WARNING: automatic rollback could not be fully proved.
echo     The live state marker remains authoritative. Do not launch; restore.

:fail
if exist "%STATUS_SNAPSHOT%" del /Q "%STATUS_SNAPSHOT%" >nul 2>&1
echo.
echo ===== PRIVATE ODST DEPLOY FAILED - DO NOT LAUNCH OR TEST =====
if /I not "%~2"=="auto" pause
exit /b 1

:restore_fail
echo.
echo ===== BASELINE RESTORE FAILED - DO NOT LAUNCH =====
if /I not "%~2"=="auto" pause
exit /b 1
