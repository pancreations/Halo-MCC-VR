# Halo MCC VR

A native OpenXR VR mod for Halo 3 and Halo 3: ODST in the Steam edition of
Halo: The Master Chief Collection.

The current known-good release is
[MCC VR Alpha 0.2.1](https://github.com/pancreations/Halo-MCC-VR/releases/tag/MCC_VR_ALPHA_0.2.1).
It is an alpha: use it at your own risk, launch only without anti-cheat, and
expect incomplete hardware and gameplay coverage.

## What works

- Per-eye stereo and 6DOF head tracking.
- Motion-controller input, weapon aim, arm IK, snap/smooth turning, melee,
  grenades, and menu control.
- Native HUD, authored floating weapon crosshair, scopes, resolution scaling,
  comfort controls, and a shared F1 configuration menu.
- Halo 3 campaign behavior, including cutscenes, pause/resume, death/respawn,
  and mission transitions.
- ODST stereo, controls, weapons/hands, native HUD, cutscenes, vibration,
  death/respawn recovery, and one tested drivable car.

Known limitations:

- ODST's first captioned opening cutscene can be black. Skip that first scene
  once; do not repeatedly skip or you will miss the working drop sequence.
- MCC can retain multiple title modules after switching games. If a level
  returns to the menu, fully close and restart MCC.
- ODST brightness stays at the game default.
- Broader ODST weapon, turret, passenger-gun, vehicle, co-op, headset, and
  long-session coverage is still needed.

The exact accepted source and artifact hashes are in
[docs/CURRENT-STATE.md](docs/CURRENT-STATE.md).

## Install

There is no installer script.

1. Download the binary asset
   `MCC_VR_ALPHA_0.2.1.zip` from the official `0.2.1` release page.
2. In Steam, open MCC's **Manage > Browse local files** folder.
3. Create a folder named exactly `Halo_MCC_VR` in the main MCC folder.
4. Copy `halo3xr.dll` and `halo3xr_launcher.exe` into that folder.
5. Make SteamVR the default OpenXR runtime, start Steam and SteamVR, then run
   `halo3xr_launcher.exe`.

The final path must end in:

```text
Halo The Master Chief Collection\Halo_MCC_VR\halo3xr_launcher.exe
```

Do not place the files loose in the MCC root. To update, close MCC and replace
only the DLL and launcher; keep `halomccvr.cfg`. To uninstall, close MCC and
delete only the dedicated `Halo_MCC_VR` folder.

If one PC behaves differently, first confirm SteamVR is still the default
OpenXR runtime, fully close every MCC process before relaunching, compare the
installed hashes below, and compare `halomccvr.cfg`. Do not use a repository
build folder as an installation source.

Release `0.2.1` hashes:

```text
ZIP      C5AE012BC379CBC7A909652D297DC0E8059CDBF41D26260771B385F8F729B124
DLL      B7363F79650E42A04D4CED6A3F51F57A6B4C2F376FF00298A6173A8287752CEF
Launcher BDC0A20F56DF72CDDE68E5D0AB621321FBDE91DA427B6C24142B38336D33EA6D
```

Windows security software may flag or quarantine unsigned injection-based VR
mods. Download only from the official release, verify the hashes, inspect the
source if desired, and allow only the two release files rather than disabling
security software globally.

## Required MCC settings

| Setting | Value |
| --- | --- |
| Video > Max Frame Rate | 120 |
| Video > V-Sync | Off |
| Halo 3 > Field of View | 120 |
| ODST > Look Sensitivity | Maximum |
| ODST > Look Acceleration | Off |
| MCC FSR | Off |

ODST's look settings control how quickly bullet direction catches the
motion-controller crosshair. If shots trail, confirm sensitivity is at maximum
and acceleration is off.

Settings live in `Halo_MCC_VR\halomccvr.cfg`. The game creates this documented
file on first launch; the F1 menu edits the same values. Deleting only that file
with MCC closed regenerates the release defaults.

## Build from source

See [BUILDING.md](BUILDING.md). A clean build uses the accepted source and
configuration, but produces a new, unaccepted candidate and file hash. Only the
published hashes remain accepted until that rebuilt candidate passes a headset
test. Use the released ZIP when you need the exact accepted binaries.

## Development evidence

- [Current accepted baseline](docs/CURRENT-STATE.md)
- [Halo 3 reverse-engineering facts](docs/RE-notes.md)
- [ODST signatures](docs/ODST-SIGNATURE-EVIDENCE.md)
- [ODST camera layout](docs/ODST-CAMERA-LAYOUT.md)
- [ODST weapon/IK evidence](docs/ODST-WEAPON-IK-EVIDENCE.md)
- [Per-title evidence policy](docs/EDITING-KIT-EVIDENCE.md)

The code was written by Claude and Codex under the direction of a human modder
who made the product and reverse-engineering decisions and performed the headset
tests. No human reviewed every line. The project is licensed under the
[MIT License](LICENSE).

Inspired by HaloCEVR by LivingFray and ReclaimerVR by Nibre. Halo is a Microsoft
trademark; this project is not affiliated with Microsoft or Halo Studios.
