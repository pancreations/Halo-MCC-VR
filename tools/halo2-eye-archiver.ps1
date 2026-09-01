# Halo 2 classic/anniversary parity evidence archiver.
#
# The installed mod already saves a picture of what each eye actually rendered
# in Halo 2 every 10 seconds (and 3 seconds after every graphics switch) as
# HaloMCCVR-halo2-eye0.bmp / eye1.bmp next to HaloMCCVR.log - but each new
# pair overwrites the previous one. This script runs OUTSIDE the game, watches
# both MCC editions' Halo_MCC_VR folders, and copies every new pair (plus the
# log) into a timestamped folder under out/test-runs before it can be
# overwritten. The renderer each pair came from is read afterwards from the
# "Halo 2 live renderer" lines in the archived log.
#
# It never touches the game, the mod, or any config. Close the window (or
# press Ctrl+C) when the play session is over.

$ErrorActionPreference = 'Continue'

$roots = @(
    @{ Name = 'steam'; Path = 'N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR' },
    @{ Name = 'steam-c'; Path = 'C:\Program Files (x86)\Steam\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR' },
    @{ Name = 'xbox'; Path = 'N:\XBOX\Halo- The Master Chief Collection\Content\Halo_MCC_VR' }
) | Where-Object { Test-Path $_.Path }

if (-not $roots) {
    Write-Host 'No Halo_MCC_VR install folder found. Nothing to watch.' -ForegroundColor Red
    exit 1
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$repoRoot = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $repoRoot "out\test-runs\h2-parity-$stamp"
New-Item -ItemType Directory -Force $dest | Out-Null

Write-Host ''
Write-Host 'Halo 2 eye-picture archiver running.' -ForegroundColor Green
Write-Host "Saving every new pair into: $dest"
Write-Host 'Start Halo 2 now. Leave this window open while you play.'
Write-Host 'When you are done playing, just close this window.'
Write-Host ''

$last = @{}
foreach ($root in $roots) { $last[$root.Name] = [datetime]::MinValue }
$copied = 0

while ($true) {
    foreach ($root in $roots) {
        $eye0 = Join-Path $root.Path 'HaloMCCVR-halo2-eye0.bmp'
        if (-not (Test-Path $eye0)) { continue }
        $mtime = (Get-Item $eye0).LastWriteTime
        if ($mtime -le $last[$root.Name]) { continue }
        # A new pair appeared; give the writer a moment to finish both files.
        Start-Sleep -Milliseconds 500
        $last[$root.Name] = (Get-Item $eye0).LastWriteTime
        $tag = '{0}-{1}' -f $root.Name, $mtime.ToString('HHmmss')
        $ok = $true
        foreach ($eye in 0, 1) {
            $src = Join-Path $root.Path ('HaloMCCVR-halo2-eye{0}.bmp' -f $eye)
            if (Test-Path $src) {
                try { Copy-Item $src (Join-Path $dest ('{0}-eye{1}.bmp' -f $tag, $eye)) -Force }
                catch { $ok = $false }
            }
        }
        # Keep the newest log alongside, so each pair can be matched to the
        # renderer that was live when it was taken.
        $log = Join-Path $root.Path 'HaloMCCVR.log'
        if (Test-Path $log) {
            try { Copy-Item $log (Join-Path $dest ('{0}-HaloMCCVR.log' -f $root.Name)) -Force } catch {}
        }
        if ($ok) {
            $copied += 2
            Write-Host ('  {0}  saved pair {1}' -f (Get-Date -Format 'HH:mm:ss'), $tag)
        }
    }
    Start-Sleep -Milliseconds 900
}
