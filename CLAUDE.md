# CLAUDE.md — project context for Claude sessions

## What this project is
0000000000000000000000000000000
A native OpenXR VR mod for Halo 3 + Halo 3: ODST in Halo: The Master Chief Collection (Steam).
Injected C++ DLL that hooks the game's D3D11 renderer and camera, submits stereo frames via
OpenXR, drives weapon aim from motion controllers, and shows a Dear ImGui settings menu on an
OpenXR quad layer. Ships with a .bat installer/uninstaller. See `docs/PLAN.md` for the full
technical plan, decisions log, and milestone acceptance criteria — read it before starting work.

## The user

- **No coding experience.** Background is game modding via guides and Blender.
  - Explain what code does and why in plain language as you go. Define jargon on first use.
  - Claude writes all the code. The user runs the game, tests in the headset, and reports back.
  - When a step needs the user to do something (run the game, put on the headset, read a value
    from Cheat Engine), give exact click-by-click instructions.
- Hardware: PSVR2 (via Sony PC adapter → SteamVR → SteamVR's OpenXR runtime), RTX 5070 Ti.
- Goal: public free release eventually; near-term must be installable by a friend via the .bat.
- Performance target: playable on an RTX 2070 Super (friend's class of GPU) via resolution scale.

## Key paths and facts

- Game install: `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection`
- Host process: `MCC\Binaries\Win64\MCC-Win64-Shipping.exe` (v1.3528.0.0 as of 2026-07-10)
- Engine DLL / RE target: `halo3\halo3.dll` (~10.6 MB, loaded at runtime by the host process;
  same engine family covers ODST)
- MCC's frontend menus are UE4; the games are native Blam engine. UEVR is not applicable.
- Dev machine is Windows 11, PowerShell 5.1 default shell.

## Hard rules

- **Never modify, patch on disk, or redistribute any game file.** All modification is in-memory
  at runtime. Install = copy our files + create an EAC-off launch shortcut; uninstall = delete them.
- The game must be launched with Easy Anti-Cheat disabled (Steam's official
  "Play without anti-cheat" option). Never attempt to bypass or tamper with EAC itself.
- Find game addresses via **AOB/signature scanning**, never hardcoded offsets — MCC updates
  shift addresses and the mod must survive patches (or fail gracefully with a clear message).
- OpenXR only. No OpenVR/SteamVR API calls in mod code.
- Source lives here; built artifacts get copied into the game folder by the install script.

## Tech stack (agreed)

- C++20, x64, MSVC (Visual Studio 2022), CMake
- OpenXR SDK (Khronos loader), D3D11 graphics binding
- MinHook for function hooking; Dear ImGui for the in-headset menu
- Config as a plain-text file next to the DLL so users can hand-edit
