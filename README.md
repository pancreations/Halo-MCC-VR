Expect me to quit at any moment 
https://www.youtube.com/watch?v=GFl3_wPFvdA


# Halo MCC VR
⚠️ Heads up: 

Windows or your antivirus might flag the mod or move a file to quarantine. The Files are not verified so please use at your own risk. More knowledgeable people are yelling at me so I gotta disclose that to you all.  Hopefully we can take care of this issue soon with some human review of the code itself. it's all there take a gander.

Please report issues, I'm gonna take a quick break before starting Halo Reach

A native OpenXR VR mod for Halo 3 and Halo 3: ODST in Halo: The Master Chief
Collection (Steam).

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

The current unified ODST test line is `feature/odst-bringup`. Its runtime at
commit `034c4a6` was built with `HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP=ON` and
headset-tested as DLL SHA-256
`B7363F79650E42A04D4CED6A3F51F57A6B4C2F376FF00298A6173A8287752CEF`.
The protected pre-ODST Halo 3 release remains tagged `v0.1.3-alpha`.

Working in Halo 3 on PSVR2, HTC, and Quest headsets through SteamVR OpenXR:

- true per-eye stereo and 6DOF head tracking;
- Sense-controller input, snap/smooth turning, melee, grenades, and menu control;
- controller-driven weapon aim and floating VR reticle;
- articulated VRIK arms;
- free left support hand on the shotgun, with assault rifle and pistol as the known-good comparison;
- native HUD with the centered game reticle hidden and the authored weapon
  reticle used as the floating VR crosshair;
- motion blur disabled by default to prevent stereo echo artifacts;
- verified build/deploy workflow.

Headset-confirmed on the ODST-enabled build:

- true stereo, 6DOF head tracking, head-relative movement, and snap/smooth turn;
- controller-driven gun and hands, arm IK, two-hand options, and floating hands;
- the native ODST HUD, authored floating crosshair, HUD size/aspect/curvature/
  height controls, and controller vibration;
- stereo cutscenes with head look and authored-shot facing;
- death/respawn recovery, a tested drivable car, and ODST -> Halo 3 -> ODST
  re-entry.

This remains an alpha. ODST brightness intentionally stays at the game default
because the attempted hook hid the HUD. Broader weapon, turret, passenger-gun,
vehicle, co-op, and headset coverage is still needed. MCC can also retain more
than one title module after switching games; if a level returns to the menu,
fully restart MCC before loading it. The default CMake option remains OFF so a
normal build stays on the frozen Halo-3-only path; ODST test builds must be
configured explicitly with `-DHALOMCCVR_EXPERIMENTAL_ODST_BRINGUP=ON`. See
[docs/CURRENT-STATE.md](docs/CURRENT-STATE.md) for the exact evidence and
remaining regression gates.

## Install

Installing is copying two files; there is no installer script.

1. In Steam, open MCC's **Manage > Browse local files** folder.
2. Create a folder named exactly `Halo_MCC_VR` inside the main **Halo The Master Chief Collection** folder.
3. Copy only `halo3xr.dll` and `halo3xr_launcher.exe` from the unzipped package into that new `Halo_MCC_VR` folder.
4. Start Steam and your OpenXR headset runtime, then run `halo3xr_launcher.exe`.
5. Optionally right-click the launcher and use **Send to > Desktop (create shortcut)**.

The final layout must be `Halo The Master Chief Collection\Halo_MCC_VR\halo3xr_launcher.exe`; do not place the files loose in the main MCC folder. To update, close MCC and copy the two new files over the old ones — `halomccvr.cfg` is left alone. To uninstall, close MCC and delete only the dedicated `Halo_MCC_VR` folder you created. The mod does not patch game files.

Settings live in `Halo_MCC_VR\halomccvr.cfg`, written with defaults on first run. Every setting carries its description, default, and range, so it can be edited by hand in Notepad with MCC closed; the F1 menu edits the same file live. Display & HUD has separate live controls for overall size, width/aspect, curvature, and vertical height. Deleting the file regenerates it with defaults.

## Required MCC settings

Set these in MCC before playing in VR. The mod does not change them for you.

| Setting | Value |
| --- | --- |
| Settings > Video > Max Frame Rate | 120 |
| Settings > Video > V-Sync | Off |
| Halo 3 > Settings > Field of View | 120 |
| Halo 3: ODST > Settings > Controls > Look Sensitivity | Maximum |
| Halo 3: ODST > Settings > Controls > Look Acceleration | Off |

### ODST motion-aim check

ODST's look settings control how quickly the game's bullet direction catches up
to the motion-controller crosshair. Set **Look Sensitivity to maximum** and turn
**Look Acceleration off**. Then point left and fire: the bullets should snap to
the crosshair instead of trailing behind it.

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

## If Windows warns about the files, it's a false alarm

Windows or your antivirus may show a warning about `halo3xr_launcher.exe` or
`halo3xr.dll`, or move one to quarantine. **The files are safe.** This happens to
almost every VR mod, because the mod has to load itself into the game to turn on
VR, and Windows plays it safe with anything it hasn't seen before. Because each
release is a new file, the warning can come back after an update — that's normal
and doesn't mean anything is wrong.

If it gets blocked, just choose **Allow** (or **Run anyway**, or restore the file
from Windows Security → Protection history) and you're good. **You don't need to
turn your antivirus off** — allow only these two files, or your `Halo_MCC_VR`
folder.

Just get your download from the official page —
[github.com/pancreations/Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR) —
where the full source is public too. (For the extra-cautious: each release's
`BUILD-INFO.txt` lists a SHA-256 fingerprint you can check the files against.)

## Credits

Inspired by HaloCEVR by LivingFray and the proof of concept ReclaimerVR by Nibre. Halo is a Microsoft trademark; this project is not affiliated with Microsoft or Halo Studios.
