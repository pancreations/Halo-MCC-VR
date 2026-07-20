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

Working in Halo 3 on PSVR2 through SteamVR OpenXR:

- true per-eye stereo and 6DOF head tracking;
- Sense-controller input, snap/smooth turning, melee, grenades, and menu control;
- controller-driven weapon aim and floating VR reticle;
- articulated VRIK arms;
- free left support hand on the shotgun, with assault rifle and pistol as the known-good comparison;
- native HUD with the centered game reticle hidden by the selected HUD element;
- motion blur disabled by default to prevent stereo echo artifacts;
- verified build/deploy workflow.

This is a development build, not yet a public release. ODST, every weapon, scopes, vehicles, cutscenes, performance targets, and friend-machine installation still need systematic validation. See docs/CURRENT-STATE.md.

## Build

Requirements: Visual Studio 2022 C++ workload, CMake, Git, and x64.

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

For the development machine, close MCC and run:

    .\deploy.bat auto

The deploy script stops on build failure and byte-compares the built and installed DLL.

## Export an alpha test build

To make a clean package for another PC, run:

    .\export-alpha.bat

It rebuilds Release, verifies the packaged DLL and launcher byte-for-byte, and creates both
`dist\Halo3VR-alpha\` and `dist\Halo3VR-alpha.zip`. Copy the ZIP to the test PC, unzip the
whole folder, read `ALPHA-README.txt`, and run `install.bat`. The test PC does not need the
source tree, CMake, Visual Studio, or a Visual C++ redistributable.

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

## Credits

Inspired by HaloCEVR by LivingFray and the proof of concept ReclaimerVR by Nibre. Halo is a Microsoft trademark; this project is not affiliated with Microsoft or Halo Studios.
