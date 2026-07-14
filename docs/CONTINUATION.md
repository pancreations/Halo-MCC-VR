# Continuation / handoff guide

This document lets a different assistant (or a fresh session) pick up this project without
re-reading everything. Read `CLAUDE.md`, `docs/PLAN.md`, and `docs/RE-notes.md` too — this file
is the operational summary and the current frontier (M2).

## What this project is

A native **OpenXR VR mod for Halo 3 + Halo 3: ODST in Halo: The Master Chief Collection (Steam)**.
An injected C++ DLL hooks the game's D3D11 renderer and camera and submits frames to a PSVR2 via
SteamVR's OpenXR runtime. See `CLAUDE.md` for the hard rules (never modify game files on disk;
EAC must be off; find addresses by AOB signature, never hardcoded offsets; OpenXR only).

**The user is not a programmer.** Claude writes all code; the user runs the game, tests in the
headset, and reports back. Give exact, click-by-click instructions. Explain jargon.

## Current status: M0 and M1 DONE. M2 (stereo) is next.

- **M0** — inject, launch EAC-off, D3D11 Present hook, OpenXR session, game shown on a world-locked
  quad "virtual screen", in-headset ImGui menu (F1). Working.
- **M1** — head rotation drives the in-game world camera (1:1, correct, level); leaning (positional)
  offsets the camera; the flat screen head-locks so it stays in front; camera located by AOB
  signature. Working and tuned on PSVR2.

Git history (all committed): `git log --oneline` →
`cc38771` docs M1 complete · `2f70826` AOB signature · `cb75d2e` leaning · `f9fd50b` screen-follow ·
`38145f4` head rotation · `58c6b12` RE toolkit · `08c9b61` M0.

## Key paths / environment

- Game: `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection`
- Host process: `MCC\Binaries\Win64\MCC-Win64-Shipping.exe` (build **1.3528.0.0**)
- Engine DLL (RE target): `halo3\halo3.dll` (loads only once you enter a Halo 3 level)
- Installed mod: `...\Halo The Master Chief Collection\halo3xr\` (halo3xr.dll, halo3xr_launcher.exe, logs)
- Runtime: SteamVR/OpenXR 2.17.3, PSVR2 via Sony adapter. Dev GPU RTX 5070 Ti.
- CMake (bundled with VS 2022): `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- Python 3.13 with `capstone` installed (for `tools/disasm.py`).

## Build / deploy / test loop

```
# configure once
cmake -S n:\dev\halo3-openxr -B n:\dev\halo3-openxr\build -G "Visual Studio 17 2022" -A x64
# build the DLL (or --target camscan for the RE tool)
cmake --build n:\dev\halo3-openxr\build --config Release --target halo3xr
# deploy: copy build\Release\halo3xr.dll into the game's halo3xr\ folder
#   (the game must be CLOSED — the DLL file is locked while the game runs)
```
Then the user launches via the **"Halo 3 VR"** desktop shortcut (the launcher: starts the game
EAC-off by setting `SteamAppId=976730`, injects the DLL via CreateRemoteThread+LoadLibrary).
Logs: `halo3xr.log` (DLL) and `halo3xr_launcher.log` in the mod folder; `.prev` keeps the last run.

## Architecture (src/)

- `dll/dllmain.cpp` — DllMain spawns an init thread: log, config, MinHook init, install D3D11 hooks,
  start OpenXR instance (background), start `Game_Init` (waits for halo3.dll).
- `dll/d3d11_hook.cpp` — hooks IDXGISwapChain Present(8)/Present1(22)/ResizeBuffers(13) via a dummy
  swapchain vtable. `VR_OnPresent` runs each game frame on the render thread.
- `dll/vr.cpp` — OpenXR session on the game's D3D11 device. Instance created on a bg thread (avoids a
  ~20 s render-thread stall). Blits the game backbuffer to an XR swapchain, submits a **quad layer**
  (world-locked, or head-locked/VIEW-space when head tracking is on). Captures head pose each frame
  (`VR_GetHeadPose`). Menu on a second quad. **This is where M2 stereo work happens.**
- `dll/menu.cpp` — hooks the game window proc: F-key hotkeys + focus spoofing (WM_ACTIVATE etc. so the
  game keeps rendering/taking input when SteamVR has focus — essential). ImGui rendered to a texture.
- `dll/game.cpp` — waits for halo3.dll, finds the camera-copy function by AOB signature, MinHooks it,
  and overwrites the camera forward/up (and position for leaning) from the head pose each frame.
- `dll/sigscan.cpp` — AOB pattern scan (`sig::Find`, `sig::FindInModule`, `sig::ModuleRange`).
- `common/log.cpp`, `common/config.cpp` — logging and the plain-text `halo3xr.cfg`.

## RE toolkit (tools/) — how we reverse-engineer without Cheat Engine

The user's AV blocks Cheat Engine, so we built our own **read-only** tools. Build with
`--target camscan`; run from `build\Release`. The game must be in a level (halo3.dll loaded).
- `camscan attach` — pid, halo3.dll base/size.
- `camscan first halo3` → `camscan narrow inc|dec|changed|unchanged` — differential float scan
  (user looks up/down/holds still on cue; narrows to the values that track the camera).
- `camscan clusters` — group surviving floats that are adjacent in memory (finds vectors/matrices).
- `camscan watch <rva> <n> <secs>` — live-print floats while the user moves (confirm it's the camera).
- `camscan poke <rva> x y z <secs>` / `spin <rva> <secs>` — WRITE test (the only writing command).
- `camscan findwrite <rva> [hits] [secs]` — hardware watchpoint: catches the instruction + registers
  that WRITE an address (this is how we traced the camera). `xref` similar for reads.
- `camscan hex <rva> <n>` — dump bytes. `tools/disasm.py <rva_hex> <len>` — capstone disassembly of
  the live process. `tools/verify_sig.py` — confirm an AOB pattern is unique in the on-disk DLL.

## M1 camera facts (build 1.3528.0.0 — RVAs move on update; we scan for them)

- Camera-copy function hooked at RVA `0x2A628C`, `__fastcall(dst, src)`. Found by signature
  `kCamCopySig` in `game.cpp` (verified unique). In `src`: **position** at +0x00 (x,y,z),
  **forward** unit vec at +0x28, **up** unit vec at +0x34. We overwrite forward/up (and position for
  leaning) before the copy so the whole game uses the head direction.
- Coordinate mapping: Halo is **Z-up**; forward `(i,j,k)` has k = vertical. Head from OpenXR
  (-Z fwd, +Y up). Yaw is relative + recenter; pitch is absolute (head-level = game-level) + trim.
- Hotkeys: F1 menu · F2 head-tracking · F3 recenter · F4/F5 flip yaw/pitch · F6 leaning · F7 up-vec ·
  F8/F9 pitch trim · F10 screen-follow vs world-locked · PgUp/PgDn lean strength.

## M2 — true stereo rendering (the current frontier)

**Goal:** render the scene twice per frame with correct per-eye view + projection so the world has
real depth; submit as OpenXR **projection layers** (replacing the flat quad). Plan flags this as the
#1 risk — re-rendering a mid-2000s engine per-eye by injection is hard.

**Why the flat frame can't just be reused:** the game renders one image with its own single camera
and FOV. VR needs two images with the headset's asymmetric per-eye projection and an eye-separation
(IPD) offset. So the game has to actually render per-eye.

**Approaches, roughly easiest→best:**
1. *Alternate-eye*: offset the camera by ±IPD/2 (along the camera right vector — we already control
   camera position via the game.cpp hook) on alternating frames, capture each as one eye, submit
   both. Half per-eye framerate + temporal mismatch, but reuses the existing mono capture pipeline.
   Good first proof that stereo separation reads as depth in the headset.
2. *Render twice per frame*: find and hook the game's scene-render pass and invoke it twice with
   per-eye camera + projection, capturing color (and ideally depth) each time. True simultaneous
   stereo. Needs RE of the render path + the projection matrix.
3. Fix per-eye correctness: projection must be the headset's per-eye frustum (from `xrLocateViews`),
   not the game's; screen-space effects/HUD may need per-eye handling (see HaloCEVR for the categories
   of fixes). MCC already supports arbitrary FOV/resolution, which helps.

**Data captured so far (2026-07-14):**
- PSVR2 via SteamVR: per-eye recommended render **2720x2772**; per-eye FOV is **asymmetric/canted**
  (left eye L61.5° R43.4° U53° D53°, right eye mirrored) → total ~105° H; **IPD 67.5 mm**.
  Logged by `vr.cpp` (`xrLocateViews`) each session as `M2: eye ...`.
- Projection block shape confirmed in the gun/overlay camera at `0x2D2F680`: `+0x30/+0x34` are
  the H/V FOV tangents (~0.857/0.875 ≈ 81° FOV). The **world-render projection is a separate
  struct still to be located** (trace it like the camera: `findwrite`/`xref` from the world
  render camera, or capture the projection matrix from the VS constant buffers).

**Suggested first M2 steps:**
- In `vr.cpp`: call `xrLocateViews` each frame to get per-eye pose + FOV; create two per-eye
  projection swapchains; build the `XrCompositionLayerProjection` submission path (currently we only
  submit quad layers). This scaffolding is needed for any approach.
- Find the game's **projection matrix** (RE, like we found the camera): so we can replace the FOV per
  eye. Likely reachable near the camera struct or via the render setup. Use `findwrite`/`xref` on the
  camera struct to find the render-setup code, then locate the projection.
- Get the eye-separation working via the existing camera-position hook (offset along camera right).

**Watch out for:** writing camera position feeds the sim (leaning is clamped for this reason) — for
per-eye offset we want a *render-only* offset, so prefer offsetting at the render/projection level
rather than the sim camera if possible. Comfort: keep the mono screen as a fallback (F-key) until
stereo is solid.
