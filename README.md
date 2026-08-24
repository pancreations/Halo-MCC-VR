# Halo MCC VR

<img width="1280" height="640" alt="heromxall" src="https://github.com/user-attachments/assets/6682b4bc-15d3-476f-b6f5-f6de82d1c0cd" />

Halo MCC VR is a native OpenXR mod for *Halo: The Master Chief Collection*.
Alpha 0.3.5 supports the Steam and Microsoft Store / Xbox app editions of MCC.

This is an alpha pre-release. Launch MCC without anti-cheat and do not use the
mod in matchmaking.

## Supported games

| Game | VR support |
| --- | --- |
| Halo 3 | Stereo rendering, 6DOF head tracking, motion controls, hands and weapons, HUD, cutscenes, and first-person vehicles |
| Halo 3: ODST | Stereo rendering, 6DOF head tracking, motion controls, hands and weapons, HUD, cutscenes, and first-person vehicles |
| Halo: Reach | Stereo rendering, 6DOF head tracking, motion controls, hands and weapons, HUD, cutscenes, and first-person vehicles |
| Halo 4 | Stereo rendering, 6DOF head tracking, motion controls, floating hands and weapons, HUD, cutscenes, and haptics |
| Halo 2 Anniversary | Campaign support in Anniversary and Classic graphics: stereo rendering, 6DOF head tracking, motion controls, hands and weapons, and haptics. Multiplayer is planned for a later release |
| Halo: Combat Evolved Anniversary | Not included in Alpha 0.3.5; planned for a later release |

## Major features

- Per-eye stereo rendering and full 6DOF headset tracking.
- Motion-controller aiming, shooting, melee, grenades, weapon handling, and
  menu control.
- Tracked first-person hands and weapons.
- Native in-game HUDs and VR crosshairs where supported.
- First-person vehicle views in Halo 3, ODST, and Reach, including drivers,
  passengers, gunners, and mounted turrets.
- A room-fixed 3D theater for cutscenes.
- Snap or smooth turning, recentering, resolution scaling, comfort options,
  weapon alignment, and per-game settings in the F1 menu.
- One download for both Steam and Microsoft Store / Xbox app (Game Pass).
- Automatic title switching between supported campaigns without restarting the
  launcher.

## Requirements

- Windows 10 or 11.
- *Halo: The Master Chief Collection* from Steam or the Microsoft Store / Xbox
  app.
- SteamVR configured as the active OpenXR runtime.
- OpenXR-compatible VR headset and motion controllers.
- MCC installed with the campaign content for the games you want to play.

The mod is commonly used with SteamVR headsets, Meta Link, Virtual Desktop,
and ALVR, but headset and streaming compatibility can vary.

## Installation

1. Download `MCC_VR_ALPHA_0.3.5.zip` from the
   [Alpha 0.3.5 release](https://github.com/pancreations/Halo-MCC-VR/releases/tag/MCC_VR_ALPHA_0.3.5).
2. Open MCC's main installation folder—the folder containing the `MCC` folder.

   - **Steam:** right-click MCC, then choose **Manage > Browse local files**.
   - **Microsoft Store / Xbox app:** open **... > Manage > Files > Browse**, then
     open the `Content` folder next to `MicrosoftGame.config`.

3. Create a folder named exactly `Halo_MCC_VR` inside that main folder.
4. Copy these files from the ZIP into `Halo_MCC_VR`:

   ```text
   HaloMCCVR.dll
   HaloMCCVRLauncher.exe
   halomccvr.cfg
   ```

5. In SteamVR, set SteamVR as the current OpenXR runtime.
6. Start SteamVR, then run `HaloMCCVRLauncher.exe` from the `Halo_MCC_VR`
   folder.

On Steam, Steam must also be running. For the Microsoft Store edition, be
signed in to the Xbox app and let the included launcher start MCC; do not start
MCC first.

The finished launcher path should look like one of these:

```text
Halo The Master Chief Collection\Halo_MCC_VR\HaloMCCVRLauncher.exe
Halo- The Master Chief Collection\Content\Halo_MCC_VR\HaloMCCVRLauncher.exe
```

Do not place the mod files loose in MCC's main folder. If you are upgrading
from an older version, remove the obsolete `halo3xr.dll` and
`halo3xr_launcher.exe` files and replace all three current files, including
`halomccvr.cfg`. Back up the old config first if you want to copy your personal
settings into the new version.

## Required MCC settings

| Setting | Value |
| --- | --- |
| Video > Max Frame Rate | 120 |
| Video > V-Sync | Off |
| Halo 3 > Field of View | 120 |
| ODST > Look Sensitivity | Maximum |
| ODST > Look Acceleration | Off |
| MCC FSR | Off |

If performance falls to half the headset refresh rate, disable motion
smoothing / ASW or lower `resolution_scale` in the F1 menu.

## Controls and configuration

- Press **F1** to open the VR settings menu.
- Press **L3 + R3** to recenter VR space and toggle the settings menu.
- Press **Y + B** to pause in ODST and Reach.
- In Reach, the left trigger and X bindings are swapped compared with Halo 3
  and ODST, so grenades are on X.
- Settings are saved in `Halo_MCC_VR\halomccvr.cfg`.

Vehicle camera positions are adjusted per seat. While seated, open
**F1 > Vehicles** and adjust **Seat forward**, **Seat height**, and
**Seat left / right**. Each driver, passenger, gunner, and turret position is
saved separately.

## Known limitations

- Halo CE is not included yet and will be added in a later release.
- Halo 2 Anniversary multiplayer is not currently compatible and will be added
  in a later release. Current Halo 2 support is for campaign.
- Halo 2 Anniversary does not yet have a VR HUD.
- Halo 4 uses floating hands rather than full arm IK.
- Reach passenger seats do not draw floating hands, although aiming and firing
  work.
- Reach character tags and navpoints can be misplaced in 3D.
- ODST's first captioned opening cutscene can appear black; skip that scene
  once to continue to the working drop sequence.
- The Microsoft Store edition can pause on its first loading screen for about
  nine seconds. Wait for it to continue.
- Co-op, headset, and long-session coverage is still incomplete.

If you report a problem, open a
[GitHub issue](https://github.com/pancreations/Halo-MCC-VR/issues) and include
your MCC edition, headset, OpenXR runtime, connection method, and
`Halo_MCC_VR\HaloMCCVR.log`.

## Uninstall

Close MCC, then delete only the dedicated `Halo_MCC_VR` folder. No original
game files are modified.

## Build from source

See [BUILDING.md](BUILDING.md). The accepted runtime source and artifact hashes
are recorded in [docs/CURRENT-STATE.md](docs/CURRENT-STATE.md).

The project is licensed under the [MIT License](LICENSE). Inspired by HaloCEVR
by LivingFray and ReclaimerVR by Nibre. Halo is a Microsoft trademark; this
project is not affiliated with Microsoft or Halo Studios.
