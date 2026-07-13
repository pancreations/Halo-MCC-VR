# Halo 3 MCC OpenXR (working title)

A native OpenXR VR mod for **Halo 3** and **Halo 3: ODST** in *Halo: The Master Chief Collection* (Steam, PC).

> **Status: M0 works — the game plays on a big virtual screen in the headset (verified on PSVR2 via SteamVR). Head tracking (M1) is next.**

## Planned features

- **Full 6DOF head tracking and stereo rendering** — real VR, not a flat screen in a headset
- **6DOF motion controls** — weapon aim decoupled from your view, driven by your VR controller
- **In-headset settings menu** (UEVR-style) — resolution scale, world scale, snap turn, and comfort options adjustable live without leaving the game
- **Clean install/uninstall via .bat** — no game files are ever modified; uninstalling deletes the mod files and nothing else
- Pure **OpenXR** — works with any OpenXR runtime (SteamVR, including PSVR2 via Sony's PC adapter)

## What it will require

- Halo: The Master Chief Collection on **Steam** (you must own it — this mod contains zero game files)
- A PC VR headset with an OpenXR runtime
- The game must be launched **without Easy Anti-Cheat** (Steam's built-in option; the installer sets this up). This means no online matchmaking while modded — campaign, co-op custom games, and Forge still work.
- Performance target: playable on an RTX 2070 Super-class GPU using resolution scaling

## Milestones

| Stage | What you get | Status |
|---|---|---|
| M0 | Mod injects into the game, .bat install/uninstall works, game shows on a giant virtual screen in the headset, in-headset menu opens | **working** |
| M1 | Your head controls the in-game camera (look around for real) | not started |
| M2 | True stereoscopic 3D rendering | not started |
| M3 | 6DOF motion controls — aim your weapon with your hands | not started |
| M4 | HUD projected into 3D space, comfort options, ODST verification, polish | not started |

## Building from source (developers)

Requires Visual Studio 2022 with the C++ workload (CMake and Git are bundled). From the repo root:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix dist --component dist
```

`dist/` then contains everything a user needs: `halo3xr.dll`, `halo3xr_launcher.exe`,
`install.bat`, `uninstall.bat`. The three dependencies (OpenXR loader, MinHook, Dear ImGui)
are fetched and built automatically by CMake — no manual setup. Binaries link the static C
runtime, so target machines need no Visual C++ redistributable.

## Credits & prior art

- [HaloCEVR](https://github.com/LivingFray/HaloCEVR) by LivingFray — the open-source VR mod for the original 2003 Halo CE that proves every one of these features is possible in a Blam engine, and whose design this project learns from
- [ReclaimerVR](https://github.com/Nibre/ReclaimerVR) by Nibre — the unreleased MCC VR prototype that proved it can be done in MCC specifically

## Legal

This mod injects code into a running game the user already owns. It contains and distributes **no** Microsoft/343 Industries game assets or binaries. Halo is a trademark of Microsoft Corporation. This project is not affiliated with or endorsed by Microsoft or 343 Industries / Halo Studios.
