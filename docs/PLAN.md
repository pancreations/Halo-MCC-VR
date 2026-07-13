# Technical plan

Last updated: 2026-07-13 (M0 built, entering headset testing)

## Progress log

- **2026-07-13 — M0 WORKING IN HEADSET.** User loaded a Halo 3 level and played it on the
  virtual theatre screen via PSVR2. Debugging findings from the three test iterations, in order:
  1. **Steam relaunch bounce:** launching `MCC-Win64-Shipping.exe` directly makes Steamworks
     kill the process and relaunch unmodded. Fixed by setting `SteamAppId=976730` (+`SteamGameId`,
     `SteamOverlayGameId`) in the child environment.
  2. **Render-thread stall:** `xrCreateInstance` against SteamVR takes ~21 s; doing it inside
     the Present hook froze the game (audio only, no window). Fixed by two-phase init: instance
     created on the DLL's background thread, session created on the render thread once ready.
  3. **Frozen screen / dead input:** MCC stops rendering and ignores input when its window
     loses desktop focus — which happens as soon as SteamVR takes over. Fixed by spoofing
     `WM_ACTIVATE`/`WM_ACTIVATEAPP`/`WM_NCACTIVATE`/`WM_KILLFOCUS` in our WndProc hook so the
     game always believes it is the focused foreground window.
  - Confirmed at runtime: game swapchain is 3840x2160 `DXGI_FORMAT_R8G8B8A8_UNORM` (fmt 28),
    XR swapchain `R8G8B8A8_UNORM_SRGB` (29) → same-family `CopyResource` path, gamma correct.
  - SteamVR/OpenXR 2.17.3; session reaches FOCUSED and stays there; logs have per-state
    heartbeat. Launcher keeps `halo3xr_launcher.log`; DLL keeps `halo3xr.log` (+`.prev`).
  - Remaining M0 rough edges: startup needs one focus nudge around the intro-video phase on
    some runs (game may need a taskbar click to start presenting); F1 settings menu + slider
    + uninstall.bat not yet formally verified in-headset; OpenXR Toolkit is installed on the
    dev machine (harmless overlay seen in testing).

- **2026-07-13 — M0 implemented and building.** Full skeleton written and compiles clean with
  MSVC (Release, x64). Decisions made during implementation:
  - **Injection mechanism:** external launcher exe using `CreateProcessW(SUSPENDED)` +
    `CreateRemoteThread(LoadLibraryW)`, then resume. Chosen over a proxy DLL because it needs
    no game file in place and uninstall is a clean folder delete. The launcher starts
    `MCC-Win64-Shipping.exe` directly, which is what Steam's "Play without anti-cheat" runs,
    so EAC is never launched.
  - **D3D11 capture:** hook `IDXGISwapChain::Present` (vtbl[8]), `Present1` (vtbl1[22]), and
    `ResizeBuffers` (vtbl[13]) via a throwaway dummy device to read the vtable. MinHook.
  - **M0 "virtual cinema":** copy the game backbuffer into an OpenXR swapchain each frame and
    submit it as a world-locked **quad layer**; the ImGui menu is a second quad. No stereo yet
    (that's M2). Compositor handles reprojection.
  - **Libraries pinned:** OpenXR-SDK `release-1.1.61`, MinHook `v1.3.4`, Dear ImGui `v1.92.8`,
    all via CMake FetchContent. Forced OpenXR loader to the static CRT to match our build.
  - **Config:** `halo3xr.cfg` plain-text next to the DLL; menu (F1) edits screen size/distance
    live and saves. Log at `halo3xr.log`. VR init failures show a MessageBox and let the game
    keep running flat instead of crashing.
  - **Open items surfaced for testing:** MCC backbuffer pixel format (affects the color/gamma
    path in the blit — logged at runtime so we can confirm); whether raw-input mouselook fights
    the menu cursor (menu swallows window messages but not raw input in M0); PSVR2 controller
    profile exposure (deferred to M1/M3, no actions bound yet).

## Decisions log

| Decision | Choice | Why |
|---|---|---|
| VR API | OpenXR only | User requirement; runs on SteamVR's OpenXR runtime, which is how PSVR2 connects on PC |
| Games in scope | Halo 3 + ODST | Same engine DLL family; ODST comes nearly free after Halo 3 works |
| Delivery | Staged milestones M0–M4 | User is fine playing intermediate stages; each stage is independently useful |
| Perf target | RTX 2070 Super playable | Friend's GPU class; dev machine is a 5070 Ti. Resolution scale slider is the main lever |
| Distribution | Free, likely public | Installer must be robust on machines we've never seen; sig scanning mandatory |
| Store version | Steam only | That's what the user (and friend) own; Game Pass/UWP is out of scope |
| Name | TBD | "H3VR" is unusable — it's the well-known VR game *Hot Dogs, Horseshoes & Hand Grenades* |

## Architecture

One injected DLL (`halo3xr.dll`, name TBD) loaded into `MCC-Win64-Shipping.exe`:

1. **Injection/loading** — a tiny launcher (part of the install) starts MCC with EAC off and
   loads our DLL into the process. Exact mechanism decided in M0 (external injector exe vs.
   proxy DLL); criteria: reliability on other people's machines and clean uninstall.
2. **D3D11 hook** (MinHook) — hook the swapchain Present and the device to capture frames,
   inject rendering work, and own the moment-per-frame where we talk to OpenXR.
3. **OpenXR session** — created with the D3D11 graphics binding using the *game's* device.
   Projection layers for the stereo eyes; **quad layers** for the ImGui settings menu and
   (later) the HUD. Resolution scale = recreating eye swapchains at a new size at runtime.
4. **Game hooks in `halo3.dll`** — found by AOB signature scan at startup:
   - camera/view matrix write (M1: head tracking; M2: per-eye view+projection)
   - render loop (M2: render the scene twice per frame)
   - first-person weapon transform (M3: controller drives the gun)
   - aim/projectile direction (M3: bullets follow the gun, not the camera)
   - input layer (M3: OpenXR actions → game input)
5. **Config + menu** — plain-text config file; ImGui menu on a quad layer edits it live.

## Milestones and acceptance criteria

### M0 — skeleton (everything but VR gameplay)
- .bat installer copies files into the game dir, .bat uninstaller removes them completely
- Game launches EAC-off via our shortcut; our DLL loads; game runs normally otherwise
- OpenXR session starts; the flat game is visible on a big virtual screen in the headset
- ImGui menu opens in-headset with at least one working setting (screen size/distance)
- **Proves:** injection, EAC-off flow, D3D11 hook, OpenXR frame submission, menu pipeline —
  every risky integration, before any reverse engineering

### M1 — headlook
- AOB-scan finds the camera; failure prints a clear error instead of crashing
- Head rotation drives the in-game camera 1:1 with no added latency feel; positional (leaning)
  after rotation works
- Mono rendering still (flat frame reprojected) — comfort is *not* expected to be good yet

### M2 — true stereo (the big one)
- Scene renders twice per frame with correct per-eye view/projection
- Culling, shadows, and transparents correct in both eyes; known screen-space artifacts documented
- Resolution scale slider works live; 90 fps at reasonable scale on the 5070 Ti,
  playable scale exists for 2070 Super
- Sim tick vs. render decoupling verified (engine ticks at 60 Hz; rendering must not be locked to it)

### M3 — 6DOF motion controls
- Weapon aims from right controller, decoupled from head; projectiles/hitscan follow the gun
- OpenXR action set with bindings for PSVR2 Sense controllers (via SteamVR) + generic profile
- Snap turn and smooth turn options; grenade + melee usable (head-aimed fallback is acceptable v1)
- Vehicles/turrets playable (head-aim mode is acceptable v1)

### M4 — ship quality
- HUD/menus on a comfortable quad layer instead of stamped into the eyes
- ODST verified end to end; differences documented and fixed
- Cutscene handling (theater screen mode), world scale calibration
- Installer handles: non-default Steam library paths, MCC updates changing the build
  (sig scan failure → friendly message), reinstall-over-existing
- Friend installs successfully using only the README

## Biggest risks (ranked)

1. **Stereo rendering (M2).** Re-rendering a mid-2000s renderer per-eye by injection is the
   hardest problem; screen-space effects, shadow caching, and HDR passes may fight us.
   Mitigation: HaloCEVR's source shows the categories of fixes needed; MCC's Halo 3 already
   supports arbitrary resolutions/FOV which removes some classic obstacles.
2. **Aim decoupling (M3).** Weapon fire direction may be derived deep in player logic.
   Mitigation: H3EK (official Halo 3 Mod Tools) documents tag structures; the MCC modding
   community has mapped large parts of halo3.dll.
3. **MCC updates.** Any patch can break signatures. Mitigation: AOB scanning + version check
   with a clear "mod needs an update" message, never a crash.
4. **PSVR2-specific quirks** (SteamVR OpenXR runtime differences). Mitigation: test early in
   M0, not late.

## Prerequisites before M0 (user's machine)

- ✅ Visual Studio 2022 Community 17.14 with C++ workload — verified installed 2026-07-10
  (`cl.exe` at `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\...`)
- ✅ CMake 3.31.6 (bundled with VS) — verified installed
- ✅ Git 2.49.0 — verified installed
- ⬜ PSVR2 connected and working in SteamVR before we test M0
- Later, for RE work (M1+): Ghidra, Cheat Engine, RenderDoc

## Open questions (to resolve during development)

- Injection mechanism: external injector vs. proxy DLL (decide in M0 by testing what
  survives real-world machines best)
- Does MCC's EAC-off mode object to a proxy DLL in the game dir? (test in M0)
- Exact PSVR2 Sense controller interaction profile exposure through SteamVR's OpenXR runtime
  (test in M0 with a minimal action set)
- Co-op: mod is client-side; verify a modded client can join an unmodded EAC-off host (M4)
