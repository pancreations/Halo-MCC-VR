# Halo MCC VR — Alpha 0.1.2

A native OpenXR VR mod for Halo 3 in Halo: The Master Chief Collection (Steam).

Alpha 0.1.2 promotes the latest headset-tested build. It focuses on reliable
campaign transitions, correct stereo across headsets, usable weapon scopes, and
more flexible HUD and two-handed controls.

**This is an early alpha.** Halo 3 campaign is the tested path. ODST, online play,
custom games, Forge, every weapon, vehicle, headset/runtime combination, and long
session have not all been validated.

**This code was written by AI.** Every line was written by Anthropic's Claude and
OpenAI's Codex, directed by a human modder who made the decisions, ran the builds,
and confirmed results in a headset, but did not review every line. Treat it as
unaudited source. It is published under the MIT license so anyone can inspect,
learn from, or fork it.

## What changed since 0.1.1

- **Campaign transitions recover automatically.** Leaving Halo 3 for the MCC shell
  now detaches stale stereo state, and returning to Halo 3 recreates title-specific
  hooks. The tested mission-exit/re-entry sequence restored 3D and tracking without
  a fatal error or manual F1 toggle.
- **Cutscenes behave more naturally in VR.** The mod preserves the VR frustum and
  follows authored camera-shot facing through cuts and the return to gameplay.
- **Runtime-provided per-eye poses.** Stereo cameras now use each OpenXR runtime's
  reported eye positions and orientations instead of one fixed headset baseline.
- **Experimental universal scope.** R3 opens a magnified world-only view aligned to
  controller aim. Right-stick Y adjusts zoom while scoped. The final scope layout
  and tighter lens were accepted in a Quest 3 headset test, but broader weapon
  coverage is still needed.
- **Two-handed aim stays acquired.** Once the support hand grabs the barrel zone,
  two-handed aiming remains engaged while grip is held and releases with grip. This
  exact DLL was headset-confirmed before packaging.
- **More HUD controls.** Display & HUD now separates overall size, width/aspect,
  curvature, and vertical height. The existing automatic headset correction stays
  in place; the new controls are live trims. Width and height still need broader
  headset calibration.

## Install or update

1. Download and unzip `HaloMCCVR-alpha-0.1.2.zip`. Do not run files from inside
   the ZIP.
2. In Steam, open MCC's **Manage > Browse local files** folder and create a folder
   named exactly `Halo_MCC_VR` inside it.
3. Copy `halo3xr.dll` and `halo3xr_launcher.exe` into that folder.
4. Start Steam and your OpenXR runtime, then run `halo3xr_launcher.exe`.

To update, close MCC and overwrite those two files. Your `halomccvr.cfg` remains.
Delete any old `install.bat` or `uninstall.bat`; current releases use manual file
copying only.

## Required MCC settings

| Setting | Value |
| --- | --- |
| Settings > Video > Max Frame Rate | 120 |
| Settings > Video > V-Sync | Off |
| Halo 3 > Settings > Field of View | 120 |

Keep FSR off in MCC's video menu. Launch only through MCC's official
anti-cheat-disabled mode, and do not use the mod in anti-cheat-enabled matchmaking.

## Tested artifact

- `halo3xr.dll` SHA-256:
  `860C5A88F70DE943AE29E9A1C95B61C5DBAAA4A513973C8EA6B08B17475B907F`
- `halo3xr_launcher.exe` SHA-256:
  `6F44B75CA1669DE224C192F13F71C27671E3ADBD993934E677210BB10AD28D70`

