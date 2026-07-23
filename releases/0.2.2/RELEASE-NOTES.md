# MCC VR Alpha 0.2.2

A hotfix on top of 0.2.1 that clears the reported Halo 3: ODST regressions
(GitHub issue #18) and a menu-controller dropout after Save & Quit.

## Fixes

- **ODST movement is head-relative again.** You walk where you look, not where
  the gun points. (#18)
- **ODST cutscenes match Halo 3.** No more focal-length pull-in during
  cinematics. (#18)
- **ODST rumble is steady.** Controller vibration no longer cuts in and out. (#18)
- **The in-game menu no longer goes dead after Save & Quit.** The mod now keeps
  its virtual controller reported as connected through the brief title-teardown
  window, so MCC no longer registers a false controller-unplug and stops reading
  the pad. This is title-agnostic, so it also protects switching between Halo
  games in one session.

Halo 3 gameplay is unchanged; the input fix touches the shared controller path
and was regression-checked.

## Install

1. Download `MCC_VR_ALPHA_0.2.2.zip` below and unzip it.
2. Copy `halo3xr.dll` and `halo3xr_launcher.exe` into a folder named exactly
   `Halo_MCC_VR` inside your MCC install
   (Steam > Manage > Browse local files).
3. Make SteamVR the default OpenXR runtime, start Steam + SteamVR, then run
   `halo3xr_launcher.exe`.

To update from 0.2.1, close MCC and replace only those two files; keep your
`halomccvr.cfg`. Full steps and MCC settings are in `MANUAL-README.txt` inside
the zip and in the README.

## Verify your download (optional)

```text
ZIP      43E52AEF5A2D1647A8F3AE6AEFDB6C22F0C67C7AA06FD70D327FB3E00ACF5DCC
DLL      1E3F0F7E1D67DB7F322FF0B2C0236CA8708E4C9EC204EDE83484DBD6BBAF3BD6
Launcher FA95B264630D42594581E4D2F8E1103FE4DB2D0711714DA4F62AA6175155C534
```

Windows or antivirus may warn about the unsigned files; that is expected for VR
mods. Allow the two files (or the `Halo_MCC_VR` folder) rather than disabling
security. Launch only through the included launcher, with anti-cheat disabled.

Source commit: `3a2a11b`. Built Release x64 with ODST bring-up enabled.
