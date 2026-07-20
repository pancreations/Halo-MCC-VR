# Halo MCC VR

A native OpenXR VR mod for Halo 3 in Halo: The Master Chief Collection (Steam).

## This code was written by AI

Every line here was written by AI coding assistants — Anthropic's **Claude** and
OpenAI's **Codex** — working under the direction of a human modder who is not a
programmer. The human did the reverse-engineering decisions, ran every build, and
verified every result in a VR headset; the AI wrote the C++.

That is stated up front for two reasons. It is honest, and it should change how you
read the code: no human reviewed every line, so treat it as you would any unaudited
source. Read it before you run it. It is also why the license is as permissive as it
is — take it, learn from it, fork it, do whatever is useful to you.

Licensed [MIT](LICENSE).

Port work must follow the [per-title editing-kit evidence policy](docs/EDITING-KIT-EVIDENCE.md).

## Current status

The headset-proven recovery baseline is Git commit 330a568 on branch recovery/best-working-20260719-1300.
The safe cleanup branch retains that exact runtime source and changes documentation/repository hygiene only. A broader runtime cleanup was rejected after a headset fatal error during level transition.

Working in Halo 3 on PSVR2 and Quest 3 through SteamVR OpenXR:

- true per-eye stereo and 6DOF head tracking;
- Sense-controller input, snap/smooth turning, melee, grenades, and menu control;
- controller-driven weapon aim and floating VR reticle;
- articulated VRIK arms;
- free left support hand on the shotgun, with assault rifle and pistol as the known-good comparison;
- native HUD with the centered game reticle hidden by the selected HUD element;
- motion blur disabled by default to prevent stereo echo artifacts;
- verified build/deploy workflow.

This is a development build, not yet a public release. ODST, every weapon, scopes, vehicles, cutscenes, performance targets, and friend-machine installation still need systematic validation. See docs/CURRENT-STATE.md.

## Install

Installing is copying two files; there is no installer script.

1. In Steam, open MCC's **Manage > Browse local files** folder.
2. Create a folder named exactly `Halo_MCC_VR` inside the main **Halo The Master Chief Collection** folder.
3. Copy only `halo3xr.dll` and `halo3xr_launcher.exe` from the unzipped package into that new `Halo_MCC_VR` folder.
4. Start Steam and your OpenXR headset runtime, then run `halo3xr_launcher.exe`.
5. Optionally right-click the launcher and use **Send to > Desktop (create shortcut)**.

The final layout must be `Halo The Master Chief Collection\Halo_MCC_VR\halo3xr_launcher.exe`; do not place the files loose in the main MCC folder. To update, close MCC and copy the two new files over the old ones — `halomccvr.cfg` is left alone. To uninstall, close MCC and delete only the dedicated `Halo_MCC_VR` folder you created. The mod does not patch game files.

Settings live in `Halo_MCC_VR\halomccvr.cfg`, written with defaults on first run. Every setting carries its description, default, and range, so it can be edited by hand in Notepad with MCC closed; the F1 menu edits the same file live. Deleting the file regenerates it with defaults.

## Required MCC settings

Set these in MCC before playing in VR. The mod does not change them for you.

| Setting | Value |
| --- | --- |
| Settings > Video > Max Frame Rate | 120 |
| Settings > Video > V-Sync | Off |
| Halo 3 > Settings > Field of View | 120 |

Do **not** enable FSR in MCC's video menu; it breaks the VR image scale. Use the mod's
picture quality presets instead.

Field of View is the one that visibly breaks the game if it is wrong: at the default
FOV the engine culls geometry outside the flat-screen frustum, so scenery pops in and
out at the edges of the headset view. FOV 120 pushes culling past the headset's field
of view. V-Sync on or a 60 FPS limit caps the headset at that rate on any hardware.

These can be changed with the headset on from inside the VR session; no flat-screen
launch is needed and the mod does not crash when MCC video settings change.

## Requirements and safety

- Steam copy of MCC and an OpenXR headset/runtime.
- Launch through MCC's official "Play without anti-cheat" mode.
- No game files are patched or redistributed.
- Do not use the mod in anti-cheat-enabled matchmaking.
- The install.bat/uninstall.bat scripts were removed on 2026-07-20; install and
  uninstall are manual. Delete any `uninstall.bat` left over from an older
  package rather than running it: a version from before 2026-07-20 could
  recursively delete the folder containing it when package files had been
  extracted directly into the MCC root.

## Credits

Inspired by HaloCEVR by LivingFray and the proof of concept ReclaimerVR by Nibre. Halo is a Microsoft trademark; this project is not affiliated with Microsoft or Halo Studios.
