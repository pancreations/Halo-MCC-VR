$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$uninstaller = Join-Path $repo 'installer\uninstall.bat'
$source = Get-Content -LiteralPath $uninstaller -Raw

if ($source -match '(?im)^\s*(?!rem\b)(rd|rmdir)\b[^\r\n]*/s\b' -or
    $source -match '(?im)^\s*(?!rem\b)[^\r\n]*Remove-Item\b[^\r\n]*-Recurse') {
    throw 'uninstall.bat contains recursive deletion'
}
$leafGuard = 'GetFileName(' + [char]36 + "m) -ine 'halo3xr'"
if ($source -notmatch [regex]::Escape($leafGuard)) {
    throw 'uninstall.bat is missing the exact halo3xr leaf-name guard'
}

$testRoot = Join-Path $env:TEMP ('HaloMCCVR-uninstall-test-' + [guid]::NewGuid())
$game = Join-Path $testRoot 'Halo The Master Chief Collection'
$mod = Join-Path $game 'halo3xr'
$desktop = Join-Path $testRoot 'Desktop'
$exe = Join-Path $game 'MCC\Binaries\Win64\MCC-Win64-Shipping.exe'

try {
    New-Item -ItemType Directory -Path $mod,$desktop,(Split-Path -Parent $exe) | Out-Null
    Set-Content -LiteralPath $exe -Value 'fake MCC executable'
    Set-Content -LiteralPath (Join-Path $game 'KEEP-GAME-DATA.bin') -Value 'must survive'
    Set-Content -LiteralPath (Join-Path $mod 'halo3xr.dll') -Value 'fake mod dll'
    Set-Content -LiteralPath (Join-Path $mod 'halo3xr_launcher.exe') -Value 'fake launcher'
    Set-Content -LiteralPath (Join-Path $mod 'halomccvr.cfg') -Value 'fake config'
    Set-Content -LiteralPath (Join-Path $mod 'KEEP-UNKNOWN-FILE.bin') -Value 'must survive'
    Set-Content -LiteralPath (Join-Path $desktop 'Halo MCC VR.lnk') -Value 'fake shortcut'
    Copy-Item -LiteralPath $uninstaller -Destination (Join-Path $mod 'uninstall.bat')

    $previousTestDesktop = $env:H3XR_TEST_DESKTOP
    $env:H3XR_TEST_DESKTOP = $desktop
    try {
        $output = 'y' | & cmd.exe /d /c ('call "' + (Join-Path $mod 'uninstall.bat') + '"') 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw ('uninstaller returned ' + $LASTEXITCODE +
                [Environment]::NewLine + ($output -join [Environment]::NewLine))
        }
    }
    finally {
        $env:H3XR_TEST_DESKTOP = $previousTestDesktop
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ((Test-Path -LiteralPath (Join-Path $mod 'uninstall.bat')) -and
           [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }

    if (-not (Test-Path -LiteralPath $exe)) { throw 'game executable was removed' }
    if (-not (Test-Path -LiteralPath (Join-Path $game 'KEEP-GAME-DATA.bin'))) {
        throw 'unrelated game data was removed'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $mod 'KEEP-UNKNOWN-FILE.bin'))) {
        throw 'unknown mod-folder file was removed'
    }
    if (Test-Path -LiteralPath (Join-Path $mod 'halo3xr.dll')) {
        throw 'known mod DLL was not removed'
    }
    if (Test-Path -LiteralPath (Join-Path $mod 'halo3xr_launcher.exe')) {
        throw 'known launcher was not removed'
    }
    if (Test-Path -LiteralPath (Join-Path $desktop 'Halo MCC VR.lnk')) {
        throw 'known shortcut was not removed'
    }
    if (Test-Path -LiteralPath (Join-Path $mod 'uninstall.bat')) {
        throw 'uninstaller did not remove itself'
    }

    # Exact incident regression: package files were placed directly in the MCC
    # root. An adjacent halo3xr.dll must never make that root a deletion target.
    $rootUninstaller = Join-Path $game 'uninstall-from-package.bat'
    $rootDll = Join-Path $game 'halo3xr.dll'
    Copy-Item -LiteralPath $uninstaller -Destination $rootUninstaller
    Set-Content -LiteralPath $rootDll -Value 'misplaced package DLL'
    $previousDisableDiscovery = $env:H3XR_TEST_DISABLE_DISCOVERY
    $previousTestDesktop = $env:H3XR_TEST_DESKTOP
    $env:H3XR_TEST_DISABLE_DISCOVERY = '1'
    $env:H3XR_TEST_DESKTOP = $desktop
    try {
        $rootOutput = 'x' | & cmd.exe /d /c ('call "' + $rootUninstaller + '"') 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw ('root guard returned ' + $LASTEXITCODE +
                [Environment]::NewLine + ($rootOutput -join [Environment]::NewLine))
        }
    }
    finally {
        $env:H3XR_TEST_DISABLE_DISCOVERY = $previousDisableDiscovery
        $env:H3XR_TEST_DESKTOP = $previousTestDesktop
    }
    if (-not (Test-Path -LiteralPath $rootDll)) {
        throw 'misplaced package DLL in game root was removed'
    }
    if (-not (Test-Path -LiteralPath $exe)) {
        throw 'game executable was removed by the root-path regression case'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $game 'KEEP-GAME-DATA.bin'))) {
        throw 'game data was removed by the root-path regression case'
    }

    'uninstall safety test passed'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
