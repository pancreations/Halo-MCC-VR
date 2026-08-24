# Halo MCC VR Alpha 0.3.5

Alpha 0.3.5 is a pre-release of the accepted five-game cumulative build.

## Supported games

- Halo 3
- Halo 3: ODST
- Halo: Reach
- Halo 4
- Halo 2 Anniversary, in both Anniversary and Classic graphics

Halo: Combat Evolved Anniversary is not supported and remains flat-screen.

## Major features

- Per-eye stereo rendering and 6DOF head tracking.
- Motion-controller aiming, weapons, melee, grenades, and haptics.
- Tracked hands and weapons, including Halo 2's Chief and Arbiter rigs.
- Native HUDs and VR crosshairs where supported.
- First-person vehicles in Halo 3, ODST, and Reach.
- Room-fixed 3D cutscene theater.
- F1 configuration menu with comfort, rendering, control, weapon, HUD, and
  vehicle settings.
- Steam and Microsoft Store / Xbox app support from the same download.
- Supported campaigns can be played and switched within one MCC session.

## Install

Extract `HaloMCCVR.dll`, `HaloMCCVRLauncher.exe`, and `halomccvr.cfg` into a
folder named `Halo_MCC_VR` in MCC's main installation folder. Set SteamVR as
the active OpenXR runtime, start SteamVR, then launch the game through
`HaloMCCVRLauncher.exe`.

Replace all three files when upgrading. Full Steam and Microsoft Store paths,
required MCC settings, controls, and limitations are in `MANUAL-README.txt`
inside the ZIP and in the repository README.

Launch MCC without anti-cheat and do not use the mod in matchmaking.

## Known limitations

- Halo 2 does not yet have a VR HUD.
- Halo 4 uses floating hands rather than full arm IK.
- Reach passenger seats do not draw floating hands.
- Co-op, headset, and long-session coverage remains incomplete.

## SHA-256

```text
ZIP      BAD8D05852A22C8F00A378A0F1B02EB5A290E8FB1A4FCA7D90A4AEB012CA6B5D
DLL      D6332F6CE4F25B2277A071D12D80A93977913643BE89677855EA8C259F80A1D4
Launcher 87F1DF6202BE26065BAEB769BC19E06B526F25DDC5FCBF6FE75FAE779C54A902
Config   6CC28740564AAE1117A385F1B9FCCB7790212317335150E1E1ECBFE2D440A2A1
```

The DLL is the exact headset-accepted artifact from source commit
`1939eabc21c1607ef93ccaec97de004271d70091`, installed and hash-verified in
both the Steam and Microsoft Store editions. It was not rebuilt for this
release.
