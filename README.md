# Halo 3 MCC OpenXR

A native OpenXR VR mod for Halo 3 in Halo: The Master Chief Collection (Steam).

## Current status

The headset-proven recovery baseline is Git commit 330a568 on branch recovery/best-working-20260719-1300.

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

## Requirements and safety

- Steam copy of MCC and an OpenXR headset/runtime.
- Launch through MCC's official "Play without anti-cheat" mode.
- No game files are patched or redistributed.
- Do not use the mod in anti-cheat-enabled matchmaking.

## Credits

Inspired by HaloCEVR by LivingFray and the proof of concept ReclaimerVR by Nibre. Halo is a Microsoft trademark; this project is not affiliated with Microsoft or Halo Studios.