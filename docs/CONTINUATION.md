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

## Current status: M0 and M1 DONE. M2 per-eye stereo WORKS; validation/polish is current.

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
- Hotkeys: F1 menu · F2 head-tracking · F3 recenter · F6 leaning · F8/F9 pitch trim ·
  F10 screen-follow vs world-locked · F11 stereo · PgUp/PgDn lean strength ·
  Home/End weapon+HUD bigger/smaller (stereo only). The yaw/pitch/up calibration flips
  (formerly F4/F5/F7) are **F1-menu-only** — see the phantom-F4 root cause below.

## M2 — true stereo rendering (the current frontier)

> **CURRENT STATE: distinct per-eye raster targets work (updated 2026-07-14).**
> F11 invokes Halo's inner world renderer once per eye and redirects Halo's final full-resolution
> scene output to a dedicated left/right texture. The user confirmed a clear, convincing stereo image
> in both eyes. This is no longer the old shared-backbuffer/fake-copy path. Do not remove the RTV
> redirect or restore post-render backbuffer copies. An objective pixel-disparity capture is still
> complete: a one-time GPU readback measured mean RGB delta **6.262** with **48.8%** of sampled
> pixels changed between eyes (11,631/23,842), while both independent RTV redirects were logged.

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
- `vr.cpp` creates a two-slice headset-recommended-size color swapchain and valid
  `XrCompositionLayerProjectionView` descriptors from each frame's predicted pose/FOV. F11 now
  submits the independently rasterized eye textures through this projection layer.
- Projection block shape confirmed in the gun/overlay camera at `0x2D2F680`: `+0x30/+0x34` are
  the H/V FOV tangents (~0.857/0.875 ≈ 81° FOV). The **world-render projection is a separate
  struct. It is now confirmed to pass through the existing camera-copy hook: source
  `+0x68/+0x6C` contained stable values `1.091595/1.114286` in a live test and is copied to compact
  render-camera `dst+0x28/+0x2C`. Two source buffers alternate into one destination at render
  cadence, matching the engine's double buffering. See `RE-notes.md`.

**Suggested next M2 steps:**
- Get the eye-separation working via the existing camera-position hook (offset along camera right).
- Verify how source `+0x68/+0x6C` maps to the final projection matrix, then override it with the
  headset frustum. The two scalar fields can represent symmetric scale but asymmetric/canted PSVR2
  projection may require a deeper matrix or viewport-center offset.

**2026-07-14 built, AWAITING HEADSET TEST — border + gun-size fixes:**
- *Borders*: user reported edge borders; brightness confirmed fine; HUD too wide and gun ~2x too
  large/close. Cause of borders: eyes were rastered straight-ahead (shared midpoint orientation)
  while PSVR2's displays are canted outward, so the outer lens edge was uncovered.
  Fix: `VR_GetEyeCantQuat` (vr.cpp) returns each eye's rotation relative to the eye midpoint;
  `RenderViewHook` rotates each eye's raster camera by it (OpenXR view axes mapped onto the camera
  basis, Rodrigues rotation of camera fwd/up), and vr.cpp now submits the **true per-eye
  orientations** instead of the midpoint. The raster projection override now also derives from live
  `VR_GetEyeFov` (max of each axis, symmetric) instead of hardcoded PSVR2 constants.
- *Gun/HUD size*: the gun/overlay camera (`0x2D2F680`, now found by `kGunCamRefSig` — see
  RE-notes) renders ~81° stretched over the ~123° stereo frame → ~2.15x magnification. In stereo,
  `RenderViewHook` rewrites its `+0x30/+0x34` tangents to the world-raster tangents × a user scale
  just before each eye's render call. **Home/End** = bigger/smaller weapon+HUD (scale ×/÷1.05,
  clamp 0.3–3.0). The game recomputes the fields each frame, so disabling stereo needs no restore.
  Log line `M2 gun overlay tangents: game (...) -> world match (...)` confirms the write path ran.
- If the gun write shows no visual effect, the overlay pass likely re-derives its frustum after our
  write point; next step would be hooking the overlay camera's per-frame update instead.

**Historical stereo prototype (superseded; do not treat as correct):** F11 toggles an alternate-eye proof using one conventional two-slice
OpenXR array swapchain. Persistent left/right frame caches retain alternating game renders. The
projection layer is visible, but correct world depth is **not verified**. The VR launcher requests a VR-shaped
windowed game surface to match the near-square eye frusta without changing normal MCC settings.
Headset roll is applied to the camera up-vector and MCC motion blur is disabled (original settings
backed up as `GameUserSettings.ini.halo3xr-backup`). The known-good symmetric projection has small
visible borders; guessing wider values at source `+0x68/+0x6C` caused warp/zoom, confirming that a
later projection scale/center still needs tracing.

**Confirmed alternate-eye limit:** stereo depth and alignment work, but head movement shows
persistence blur/judder. `xrWaitFrame` paces total Presents to the headset refresh rate, so alternating
eyes gives each eye only every other frame (roughly 45 Hz on a 90 Hz headset), and one cached eye is
always temporally stale. This cannot be tuned away; the next implementation must locate/invoke the
scene render twice inside one OpenXR frame so both eyes update at the full headset rate.

**2026-07-14 inner-render breakthrough and per-eye raster path:** hooked the engine's signature-located inner per-view
renderer (`0x286A14` on build 1.3528) rather than re-entering the non-reentrant outer frame. It
calls/captures left and right sequentially inside one Halo frame; fpsVR confirms 120 application
FPS after F11. The original implementation replayed into shared targets and was not stereo. The
current implementation redirects the final scene output independently for each call; the user
confirmed the resulting stereo image works beautifully. Positional tracking is forced on during
stereo and physical OpenXR eye poses are retained.

**Hard requirements for completing M2 (in order):**
1. Bind or redirect two distinct color render targets around the world draw, one per eye. Do not
   render twice into the same backbuffer and copy afterward unless GPU ordering is proven.
2. Apply physical ±IPD/2 translation before each world raster pass. Keep OpenXR eye poses physical.
3. Read back or GPU-compare one frame and prove nearby world pixels have horizontal disparity while
   distant pixels converge. Baseline changes must scale that disparity. **Distinct-image portion
   complete:** one-time readback measured mean RGB delta 6.262 and 48.8% changed samples. A future
   correspondence/depth-aware test can quantify near-vs-far disparity.
4. Route the proven eye textures into the two-slice OpenXR swapchain every frame.
5. Separate weapon/HUD from world projection.
6. Match raster projection, culling, reconstruction, and OpenXR FOV together.

**Confirmed failed approaches — do not repeat:**
- Scaling OpenXR eye positions: caused double vision; it does not create geometric depth.
- Scaling shader constant `0x270000`: it is reconstruction data and caused warped fog/lighting,
  curved clipping, white gaps, and apparent missing meshes.
- Writing shared projection `view+0x98+0x78`: contaminated weapon/HUD and did not prove raster stereo.
- Fixed compositor FOV guesses: caused zoom/stretch and head-turn warp when raster FOV differed.
- Resolution/aspect changes alone: cannot create stereo disparity.
- Do not restore the pre-headlook vectors around `CamCopyHook` to decouple the weapon. Despite the
  earlier static-gun-camera hypothesis, that destination feeds the active world view; doing so
  disables head tracking. Weapon/HUD separation must occur at a later overlay draw/pass or a more
  specific weapon transform hook.

**Captured evidence:** matrix census ID `0x00560000` changed with eye offset and doubled when the
baseline doubled, but the user still saw no depth. A changed matrix is therefore insufficient proof.
Current configuration uses physical game-space IPD (`0.33` world units per meter), physical OpenXR
eye poses, forced positional tracking during stereo, the no-bars projection override, and distinct
per-eye final-scene render targets.

**Per-eye raster implementation working (2026-07-14):** following the useful D3D11 architecture in
the user-provided `praydog/UEVR` reference, `d3d11_hook.cpp` now hooks
`ID3D11DeviceContext::OMSetRenderTargets` (vtable slot 33). `VR_BeginRasterEye()` creates two
render-target-capable eye textures and marks the active eye around each inner render call. An RTV
census proved the world pass never binds the swapchain backbuffer. Its final output is instead the
unique 2912x2100 `DXGI_FORMAT_R8G8B8A8_TYPELESS` resource with bind flags `0xA8`
(RTV+SRV+UAV), immediately after a typed RGBA intermediate. The hook redirects only that resource
to the active eye RTV; redirecting the preceding format-28 intermediate produced black.
`VR_CaptureRenderedEye()` no longer copies the shared backbuffer: it marks an eye valid only after a
real RTV redirect. Expected success logs are
`M2 RASTER: redirected internal scene-color RTV to eye 0/1`. Expected failure is
`M2 RASTER: no internal scene-color RTV redirect occurred; refusing fake eye copy`.

The first successful image was slightly dark because the eye cache inherited the swapchain format.
The current source creates eye caches lazily from Halo's exact typed final RTV view format, preserving
its sRGB conversion. This color-space fix was built but still awaited headset testing when this note
was written. Shared depth is temporary and should be split after color/gamma is confirmed stable.

**M3 kickoff (2026-07-14): aim is decoupled from the camera.** User test: with head tracking on,
bullets follow the **gamepad right stick**, not the head. So the game keeps its own aim state
(stick-integrated yaw/pitch) upstream of the camera buffers we overwrite.

**M3 aim-state hunt (2026-07-14, dead end — do not repeat):** watchpoint tracing of `src+0x28`
only ever caught giant ring-buffer memcpys (0xA1594 bytes, buffers 0xA00000 apart; registers at
the trap are unreliable for chasing). A full-memory differential float scan (1.85M → 17
candidates via wiggle/still/nudge cues) found only per-frame-recycled scratch: the promising
radian pair at heap `+0x20/+0x24` ignored a direct 4-second write test (view unaffected). The aim
state evidently lives in frame-recycled allocations with no stable address. camscan gained
`aimwrite`, `aimchain`, `findwriteabs`, `watchabs`, `pokeabs` during this hunt (the DLL logs every
new `src=` buffer so tools can self-locate it).

**M3 SHIPPED APPROACH — closed-loop stick injection (built + deployed, awaiting headset test):**
`input.cpp` MinHooks `XInputGetState` (xinput1_4/1_3/9_1_0, hooked when halo3.dll loads).
When VR aim is ON (**Insert**), `Game_ComputeAimStick` (game.cpp) compares the game's true aim
forward — captured in `CamCopyHook` pre-head-look into `g_aimFwd*` — with the right Sense
controller ray (shared F3 yaw reference with the head), and substitutes right-stick values
(full deflection at ~10° error, gain k=5.7, deadzone floor 9000). The game integrates it through
its normal turn path, so bullets/reticle/vehicles/turrets stay consistent; view stays head-driven.
Known v1 limits: aim speed capped by game turn rate (raise in-game look sensitivity if laggy);
no visible hand-ray crosshair yet (next: draw a small aim-dot quad or trace the reticle draw);
expect a possible sign flip in yaw/pitch steering on first test (one line in
`Game_ComputeAimStick`).

**M3 full controller support (2026-07-14, built + deployed, awaiting test):** first stereo session
was a success (user: "3D is great", depth confirmed) — but VR aim was never engaged because Insert
was opt-in; it now defaults ON. New in this build:
- `vr.cpp` action set expanded: move/turn sticks, triggers, grips, A/B/X/Y, stick clicks, menu —
  suggested for touch/index/wmr/vive/simple profiles (SteamVR remaps onto PSVR2 Sense; users can
  rebind in SteamVR controller settings). Read each frame into `VrPadState` (`VR_GetPadState`).
- `input.cpp` is now a full virtual gamepad: merges VrPadState into XInputGetState (or fabricates
  a connected pad if none plugged in — menus work with Sense only). Grips→bumpers,
  menu→Start, stick clicks→L3/R3, triggers analog. Hook installs at frontend (WaitThread retries
  until an xinput DLL loads), so menus navigate before halo3.dll exists.
- Snap/smooth turn (`ApplyVrTurn` in game.cpp): right Sense stick X rotates `g_gameYawRef`, which
  turns the view AND drags the hand-aim target. Menu (F1) has snap/smooth radio + snap increment
  (5-90°) + smooth speed (30-360°/s) sliders; persisted as `turn_smooth`, `turn_snap_deg`,
  `turn_smooth_deg_s` in halo3xr.cfg.
- Movement is head-relative (`Game_MapMoveStick` rotates move vector by head-aim yaw delta) so
  pushing forward walks toward the gaze, not the gun.

**M3 CONFIRMED WORKING (2026-07-14 evening):** bullets follow the hand; Sense sticks/buttons all
respond (menus + gameplay) after hooking ALL XInput entry points — MCC loads xinput DLLs lazily
and reads through xinput1_4 even when 1_3 loads first, so `WaitThread` now retries
`Input_InstallXInputHook` forever and hooks both `XInputGetState` and ordinal-100
`XInputGetStateEx` in 1_4/1_3/9_1_0. Snap turn confirmed working. Remaining M3: attach the gun
MODEL to the hand (it is still drawn camera-glued; only the aim follows the hand), draw a hand-ray
crosshair, refine control mapping per user feedback.

**2026-07-14 late session notes:** a "head tracking completely broken" report was a stray **F4**
press (yaw sign flip — visible as `yaw sign +1` in the log); F4 again fixes it, resets on
relaunch. TODO: move the F4/F5/F7 calibration flips out of casual reach (menu-only). Menu
navigation via Sense: per user request (UEVR-style), the left stick becomes the **D-pad while the
right controller is held within 0.3 m of the head** (walking suppressed during the gesture);
lower it to walk. No frontend/gameplay special-casing. `per_eye_history=0` config switch disables
only the ghosting fix, never bloom itself.

**F1-menu-in-stereo fix + left-hand gesture (2026-07-14, deployed):** the menu quad was only
submitted in the mono-screen branch, so F1 was invisible in stereo — it now submits in both modes
(head-locked in stereo). Left controller pose is now tracked (`left_aim_pose` action +
`VR_GetLeftControllerPose`); the D-pad gesture uses the controller picked by `dpad_hand` in the
config (0 = left, DEFAULT, per user request; 1 = right), selectable via radio buttons in the F1
menu.

**2026-07-14 screenshot findings (user-provided stereo capture):** (1) left-eye ghosting NOT fixed
by fp16 history isolation — sparkle-trail ghosts in canopy, left lens only; tracked-set capped at
16 (many 1x1s) and the real history buffer is likely compute/UAV-written (invisible to the
OMSetRenderTargets hook). New diagnostic shipped: `right_eye_first` config + F1 checkbox swaps eye
render order — if ghosting moves to the right lens, shared-history-order is confirmed. (2) **HUD
is split between the eyes**: grenades+radar render only in the left eye, ammo counters only in the
right eye — the HUD pass appears to alternate elements between the two inner renders. Open M2/M3
item. (3) Controls regressed again (only snap turn worked) despite all hooks installed and actions
active in the log; added `M3 DIAG` heartbeat (xinput reads / padValid / merged counts every 10 s)
plus one-shot reasons when aim steering is blocked — next log will pinpoint the break.

**2026-07-14 root causes found:** (1) Controls dying between sessions = **physical gamepad power
state**: MCC gates ALL XInputGetState polling on XInputGetCapabilities reporting a connected pad
(the DIAG heartbeat showed literally zero reads with the pad asleep). Fixed by hooking
XInputGetCapabilities in all xinput DLLs and fabricating a standard connected gamepad on slot 0
whenever the VR controllers are live. (2) Eye-order test CONFIRMED the ghost follows whichever eye
renders first (user flipped `right_eye_first` and the ghost moved to the right lens) — shared
temporal history, not in the half-res fp16 set; isolation filter widened to fp16 + R11G11B10F +
R10G10B10A2 at ANY size (cap 32). If ghosting persists after the widened net, the history is in a
UAV/compute-written or non-HDR-format resource: next step is a CSSetUnorderedAccessViews census.

**2026-07-14 final session state:** XInputGetCapabilities fabrication CONFIRMED WORKING — all
controls (move/shoot/aim/D-pad gesture) work with no physical gamepad. Widened full-res RTV
isolation was HARMFUL (massive radial smears: full-res targets are fed by passes outside the
wrapped eye renders) — reverted to the ≤half-res fp16 set. Ghost hunt continues via
`M2 UAV CENSUS` (new hooks: CSSetUnorderedAccessViews vtable 68 +
OMSetRenderTargetsAndUnorderedAccessViews vtable 34, census-only): next session, read the census
from the log and pick persistent full-res UAV textures as isolation targets (swap their contents
per eye like the RTV set — but only those, not every full-res target).

**2026-07-14 controls ROOT CAUSE (definitive):** dumpbin shows MCC-Win64-Shipping.exe statically
imports ONLY `XINPUT1_3.dll` for gamepads — yet DIAG showed exactly ONE read reaching our DLL-level
detour per session. Conclusion: **Steam Input patches MCC's import table**, routing the game's
XInput calls to Steam's handler and around our MinHook detours; whether our hook saw traffic
depended on patch-timing races each session. Fix: `Input_ClaimXInputIat()` writes our shims
directly into the exe's IAT slots for XInputGetState/XInputGetCapabilities, chains to whatever
handler was there (Steam's or the real export), and WaitThread re-asserts every 2 s so a later
Steam patch cannot take the slot back. Caps/state fabrication is unconditional (idle pad before VR
data is ready) because MCC enumerates controllers once at startup. Ghost hunt: UAV census came back
EMPTY during eye passes (4 raster redirects, 0 UAV binds) → history is RTV-based after all; now
isolating the full-res `R10G10B10A2_UNORM` accumulation target per eye (the only remaining
full-res candidate; full-res fp16 remains excluded as harmful).

**GHOST IDENTIFIED (2026-07-14, hypothesis with strong evidence): it is the SUN-SHAFT radial blur,
not temporal history.** The streaks radiate from the sun's screen position; the engine computes
that position once per frame from the LAST prepared camera, so the second-rendered eye is clean
and the first smears (matches the eye-order flip test exactly, and explains why every buffer
isolation failed — nothing temporal is involved). IAT claim CONFIRMED WORKING (controls stable
with Steam Input active). Current build: `M2 PARAM` census logs every unique (id,eye) shader
upload ≥8 bytes with size, caller RVA (`_ReturnAddress` − halo3 base) and first 4 floats — find
the small param whose floats look like a screen position and are IDENTICAL across eyes, then
disassemble its caller and re-run that computation per eye in `RenderViewHook` (like
`g_prepareView`). R10G10B10A2 isolation reverted; fp16 ≤half-res isolation retained.

**Left-eye bloom ghosting — fix built (awaiting test):** cause fits render order: eye 0 blends
bloom against eye 1's PREVIOUS frame (wrong viewpoint), eye 1 against eye 0 of the same frame
(near-identical, looks clean). Fix in vr.cpp: track every half-res-or-smaller
`R16G16B16A16_FLOAT` RTV bound during eye passes (bloom/exposure pyramid; pure intermediates are
harmless to include), keep per-eye shadow copies, `CopyResource` them in at `VR_BeginRasterEye`
and out at `VR_EndRasterEye`. Tracked set resets on ResizeBuffers. Expect log lines
`M2: isolating post-process history target WxH per eye (N tracked)`. If ghosting persists, the
history lives in a full-res or non-fp16 target — widen the filter next.

**2026-07-14 (Claude session) — "fixing ghosting breaks controls" ROOT CAUSE + sun-shaft fix
rebuilt:**

1. **Controls were never broken by the ghost fixes. The breaker is a phantom F4.** Both retained
   logs (19:47 and 20:16 sessions) show `yaw sign +1` firing **30–85 ms after** the XR session
   drops to `synchronized` (SteamVR dashboard/exit takes focus). That is SteamVR's exit path
   sending **Alt+F4** — it arrives as `WM_SYSKEYDOWN` with `VK_F4`, which the hotkey handler
   (deliberately listening to SYSKEYDOWN for F10) treated as the F4 yaw-flip. A flipped yaw sign
   inverts head-turn AND makes the closed-loop hand-aim steer away from the controller — reads
   exactly as "controls completely broken". Meanwhile the same logs show the input path healthy
   (M3 DIAG merged counts in the tens of thousands). **Fix (menu.cpp):** hotkeys act on plain
   `WM_KEYDOWN` only (F10 keeps its SYSKEYDOWN exception); F4/F5/F7 flips moved into the F1 menu
   ("Tracking calibration" row, current signs displayed); Alt+F4 passes through to the game (and
   is exempt from the menu-open key swallow) so closing the game works again. A one-shot log line
   `Alt+F4 received; passing it to the game` marks the event.
2. **The 19:47 build's sun-shaft disable was reverted for the wrong reason** (blamed for the
   "broken controls" above; its log actually shows controls fine). That source was never
   committed and is lost. Corroborating evidence it was the right fix: with it active, the
   800x800 R16G16_FLOAT + R8G8B8A8 pair did NOT bind at stereo-on; in the reverted 20:16 build
   they bind immediately as the FIRST eye-pass RTVs.
3. **Sun-shaft neutralization re-implemented (vr.cpp, D3D level):** during eye passes, any small
   (≤2048) R16G16-family RTV+SRV render target — the shaft chain's occlusion buffer, the only
   R16G16 target in the eye-pass census — is cleared to black on the binding context and its pass
   writes diverted to a private dummy, so the shaft composite adds nothing in either eye. Gated by
   `stereo_sun_shafts` (default 0 = shafts off in stereo). **F1 menu checkbox "Sun shafts in
   stereo" toggles it live for A/B.** Mono rendering is untouched. Expected log:
   `M2: sun-shaft occlusion target 800x800 fmt=34 neutralized (eye 0, stereo_sun_shafts=0)`.
   If the sun position param is ever found (PARAM census plan), a per-eye recompute can restore
   the effect properly; until then disabling it is the shippable fix.
4. **Read-before-write history discovery provably finds nothing** (its promotion log line never
   fired): ping-pong history reads buffer A and writes buffer B within one pass, so a
   same-pass read-then-write pattern never occurs. It now disables itself (and the SRV hook)
   after 480 empty eye passes: `M2: no read-before-write history found...`. Machinery kept for
   reference; the active ghost fix is item 3.
5. Dead diagnostics still present but harmless: `M3 DRAW RUNS` dumps `total=0` (its Draw hooks
   were removed); `VR_RecordUavCensus` has no caller; `per_eye_history` config is unconsumed.

**Live headset test falsified the sun-shaft theory (same evening).** With the R16G16
neutralization confirmed engaged in the log, the user reported: ghost unchanged in the left eye
(and visible on the desktop mirror); it is a **trailing after-image of bright spots**, present
**even with the head perfectly still**, and the "Sun shafts in stereo" A/B checkbox made **no
difference**. The Alt+F4 fix, however, was PROVEN this session: log shows
`Alt+F4 received; passing it to the game (close request)` 1.6 s after the session lost focus,
with NO yaw flip — the controls-break mystery is closed.

**Current ghost theory (fits every observation): bloom persistence through a frame-flipped
ping-pong buffer pair.** Halo accumulates last frame's glow by reading buffer A while writing
buffer B, swapping roles each frame. With one shared pair and two eye renders, the last writer
each frame is the second eye, so the FIRST eye blends against the OTHER eye's glow (offset by
IPD+cant) and the feedback repeats that offset into a decaying trail of copies. Explains: trails
only on bright sparkle content (bloom); present when still (the eye offset never goes away);
follows whichever eye renders first (eye-order flip test); v1 same-pass read-before-write
discovery finding NOTHING (the pair's read side is only written on the NEXT frame — invisible by
construction); and the fp16-set isolation failing earlier (its 16-slot tracked set was flooded by
the game's endlessly re-allocated 1x1 exposure scratch textures, plus copy-in/out of a pair only
works if BOTH pair members are tracked).

**v2 cross-pass discovery (built + deployed 20:52, awaiting test):** `RecordHistoryUse` now keeps
one persistent candidate per texture across passes (cap 96; textures ≤8px excluded — kills the
1x1 flood; UAV-capable excluded — final scene is already redirected). Promote when a texture was
(a) read in a pass before being written in that pass (`readCarriedIn` — it carried data INTO a
pass) and (b) also RTV-written inside some eye pass. Rule (b) keeps the outside-fed full-res
targets that made the old format-based isolation harmful OUT of the set. Promotion feeds the
existing per-eye shadow-copy machinery (copy-in at `VR_BeginRasterEye`, copy-out at
`VR_EndRasterEye`) — for a frame-flipped pair with both members tracked, each eye then always
reads its own previous glow (verified by simulation). Discovery stays open 480 eye passes (~2 s
of stereo) since the pair needs ≥2 frames to show both sides, then the SRV hook shuts off.
Everything is gated on `per_eye_history` (cfg already 1) with a new F1 checkbox **"Per-eye glow
history"** for live A/B. Expected log: `M2: isolating cross-pass history target WxH format=F per
eye (N tracked)` then `M2: history discovery window closed; N target(s) isolated per eye`.
The sun-shaft neutralization stays in (default off in stereo, harmless, its own checkbox).

**v2 cross-pass result (same evening): 0 targets isolated in two sessions** — the log line
`history discovery window closed; 0 target(s)` proves NOTHING with RTV+SRV binds is both read and
written inside the eye passes (PS binds + RTV binds + the earlier empty UAV census all agree).
Ghost still on the first-rendered eye, still moves with `right_eye_first`. Conclusion: the shared
history is **written once per frame OUTSIDE the two wrapped eye renders** (frame-level post,
where all previous instrumentation was blind) and only **sampled** inside the eye passes. Per-eye
shadow copies cannot fix a single-write-stream buffer.

**v3 RESULT: also zero.** Log shows no `stereo-blanking` lines. That makes **four independent
probes empty** (in-pass RTV writes, in-pass PS SRV reads, UAV binds, frame-level RTV writes)
while the ghost still tracks render order. Conclusion: **stop trying to name the buffer.** It is
reached by a path none of these hooks observe — most likely `CopyResource`/`CopySubresourceRegion`
(never hooked) or an SRV bound before the eye passes and left bound (our SRV hook only fires at
bind time, so a bind-once-read-forever texture is invisible). Also from this log: **stereo perf is
fine — `fps 117`/`fps 118` with stereo on**; the user's "lower fps" is the MONO screen path
(`fps 62`/`67`, stereo off), a separate issue.

**v4 SHIPPED (21:14) — warm-up pass: kill the ghost WITHOUT identifying the buffer.**
`RenderViewHook` now renders `firstEye` an extra time and discards that image
(`stereo_warmup_pass`, default ON, F1 checkbox "Warm-up render pass"). Pass order becomes
[firstEye discard, firstEye keep, otherEye keep], so both KEPT renders inherit leftovers from
their own viewpoint no matter which resource carries them. This tests the entire cross-eye
contamination class in one shot: if the ghost dies, contamination is confirmed and the user has a
working milestone; if it survives, the cross-eye theory is dead despite the eye-order evidence and
the next suspects are the eye caches themselves or the compositor. Cost: 3 world renders/frame
(~120 → ~80 fps expected). Disproven machinery neutered in the same build: `per_eye_history`
default 0 (its SRV hook was pure overhead) and `stereo_sun_shafts` default 1 (stop disabling a
real effect for zero benefit); both remain as diagnostic switches. Deployed cfg updated to match.

**v4 RESULT: CONFIRMED by headset test — "the ghosting is gone", controls "working amazing".**
Cross-eye state contamination is therefore proven, and the ghost is fixable without ever naming
the resource. Cost as predicted, but worse than the arithmetic: the user reports frame rate
**halved**, not reduced by a third — because dropping under the headset's 120 Hz budget snaps
SteamVR to 60. There is no 80 fps on a 120 Hz headset; it is 120 or 60.

**v5 SHIPPED (21:20) — alternating eye order: the same fix for free (2 renders/frame).**
Exploits the asymmetry the warm-up test proved: a render inheriting the other eye from the SAME
frame is clean (the second eye, always fine); only inheriting it from the PREVIOUS frame ghosts.
Swapping the order every frame makes each frame's first render inherit the previous frame's last
render — the same eye — so every render is in one of the two known-clean cases:
`frame N [A B]: A<-A(prev last), B<-A(same frame); frame N+1 [B A]: B<-B(prev last), A<-B(same)`.
`stereo_alternate_order` (default ON, F1 checkbox); `stereo_warmup_pass` now defaults OFF but is
retained as the proven fallback. Deployed cfg updated to match. **If v5 holds, M2 is complete at
full frame rate.**

**v5 RESULT: FAILED — "ghosting is now flickering on both eyes"** (perf good, 120 fps). Frame
parity moved the ghost every frame instead of removing it. Three data points now fit exactly one
rule: **the first render of each frame ghosts, whichever eye it is**
(`[A B]`→ghost on A steady; `[A_d A B]`→clean; alternating→ghost alternates). The rule implies
alternation SHOULD have worked (each frame's first render would follow the same eye's last
render), so the assumption underneath it is wrong: **RenderViewHook is probably NOT called exactly
once per frame** (extra engine views — skybox/reflection/scope — would flip a frame-parity counter
the wrong way).

**v6 diagnostic ANSWERED: `M2: view renders 60/sec` vs `fps 60` — exactly ONE hook call per
frame.** The frame-parity alternation really was alternating, and it still flickered. That kills
the "each render inherits the previous render" model for good.

**THE MODEL THAT FITS ALL FIVE RESULTS — the poisoning writer is Halo's frame-level post pass,
anchored by OUR OWN centre-camera restore.** The frame is:
`CamCopy(centre) → RenderViewHook[eye A, eye B] → (we restore the CENTRE camera) → Halo's
once-per-frame post → Present`. That post pass writes the shared glow history from the **centre**
viewpoint — belonging to neither eye. So the next frame's FIRST render always inherits a
centre-anchored history and ghosts by half an IPD, whichever eye it is; the SECOND render inherits
the first eye's fresh state and is clean. This explains: fixed order → ghost on first eye
(steady); parity alternation → ghost follows the first render → flickers on both; warm-up → clean
because the discarded render absorbs the poisoned inheritance; and why no per-eye buffer isolation
ever helped (nothing is shared *between the eye passes* — the contamination enters between
frames).

**v7 SHIPPED (21:37) — post anchor: the free fix implied by the model.** Instead of restoring the
centre camera at the bottom of `RenderViewHook`, restore the FIRST EYE's camera + derived matrices
and re-run `g_prepareView` (the post pass reads the engine globals PrepareView uploads, not the
view struct). Halo's post then writes the history from the first eye's viewpoint — the same eye
that renders first next frame, which inherits its own history. Two renders per frame, full speed.
Requires fixed eye order (the anchor must match whoever goes first), so alternation is gone.

**v8 (21:40) — the warm-up fallback is DELETED at the user's explicit request** ("no fall backs",
"why would i want a feature that halves the frame rate"). The post anchor is now the single
unconditional code path: always two renders, always anchored, no `ghost_fix` setting, no radio
buttons, no 3-render path anywhere. `ghost_fix`/`stereo_alternate_order`/`stereo_warmup_pass` are
parsed-and-ignored so older cfg files load clean. The disproven `per_eye_history` and
`stereo_sun_shafts` checkboxes are out of the menu too (config keys remain, inert, as
diagnostics); the deployed cfg had `stereo_sun_shafts = 0`, which was still neutralizing a real
effect for zero benefit — set back to 1.

**Process note for future sessions: the user does not want fallbacks or tradeoff options — fix
the thing.** Offering an fps-halving safety net as a default cost a play session and real trust.
Ship one correct path.

**v9 RESULT (same evening): post anchor FAILED** — ghost back on the left eye; fps wobbled 60-78
then reached 120. Five ordering/anchoring attempts, five failures. The unified empirical rule that
fits every result with no contradictions: **whichever render crosses the frame boundary absorbs
the poisoned state, regardless of eye identity or camera anchoring** (fixed order → first eye
steady ghost; alternation → flicker; post anchor → unchanged; warm-up → clean because the
boundary-crossing render is discarded). No ordering trick can dodge it; only a discarded render
absorbs it.

**v10 SHIPPED (21:49) — FINAL shape: warm-up unconditional + resolution rebudgeted.** The
warm-up ([firstEye discard, firstEye, otherEye]) is the single unconditional code path in
`RenderViewHook` (no config, no menu). The fps cost is paid at the launcher: two renders at
2912x2100 measured 117 fps (no headroom), so the launcher now requests **2240x1616**
(~sqrt(2/3) linear scale; 3 x 3.62 MP = 10.9 MP/frame vs 12.2 before) so three renders fit the
120 Hz budget. Compositor upscales to 3040x3100 per eye. Post anchor, alternation, ghost_fix
config, and the g_lastRenderedEye tracker are all removed.

**v10 RESULT: ghost gone, but fps still exactly 60 at 2240x1616 — resolution is NOT the lever.**
2 renders @ 12.2 MP = 117 fps, but 3 renders @ 10.9 MP = 60 fps: Halo's world render cost is
dominated by per-render engine/CPU work (~4 ms each), nearly independent of pixels. Three renders
can never fit the 8.3 ms budget at any resolution; SteamVR then paces to 60. Only deleting the
warm-up render gets 120 back.

**v11 SHIPPED (21:57) — resolution restored + the two decisive probes (log-only).** Launcher back
to 2912x2100 (sharpness costs nothing at 60; the measured comment is in launcher.cpp). New
probes, both no-behavior-change: (1) `CopyResource`+`CopySubresourceRegion` hooks (vtable 46/47)
→ `M2 COPY eye=N src=... -> dst=...` for each unique texture pair while stereo runs — GPU copies
were invisible to every earlier census; eye=-1 tags copies BETWEEN passes, where the poison is
suspected to move. (2) `PSGetShaderResources` snapshot at `VR_BeginRasterEye` (first 600 passes)
→ `M2 PASS-SRV eye=N slot=S ...` for render-target-capable textures ALREADY bound when a pass
starts — bind-call censuses can never see bound-once-read-forever inputs. Reading the next log:
the poison carrier should appear either as an `M2 COPY` with eye=-1 into an RTV|SRV texture, or
as a recurring `M2 PASS-SRV` entry that is not a known intermediate. Once named: isolate or
per-eye-swap that ONE resource, delete the warm-up pass in `RenderViewHook`, and 120 fps returns.

**Current shipped state: ghost-free, full sharpness, 60 fps, controls good.** The user finds 60
"abysmal" — eliminating the warm-up via the probe data is the top priority of the next session.

**LATE-NIGHT ARC (22:00-22:25), current frontier:** the copy probe caught frame-level full-res
snapshot copies; per-eye substitution of the learned pairs was built and ARMED (log:
`per-eye history snapshot armed` x2, at 117-120 fps with the warm-up deleted) — commit `5485596`,
which the user declared the baseline: **120 fps + controls great + stereo; only left-eye ghosting
remains. Never regress fps/controls again — warm-up (3 renders) is BANNED (halves fps).** See
memory `best-working-build`. dst-only substitution: ghost persisted. dst+src substitution
(`55ca9f0`): ghost STILL persisted; capture pipeline verified healthy (no `refusing` lines).
Conclusion: every texture-level channel is exhausted; the poison must travel as a **shader
constant** (per-frame single-viewpoint param consumed by both eyes — the old PARAM-census plan).
Shipped `3cd1644` (22:24): census-only hooks on UpdateSubresource + Map/Unmap (vtable 48/14/15).
**Census result:** all constant uploads funnel through ONE call site (`halo3+0x2AF8C1`,
~100k/s), so caller-RVA cannot discriminate effects; the census itself sagged fps 117→92 over a
minute. Follow-up `da5a168` (22:31): exact-match matcher — swap any 64-byte block equal to the
OTHER eye's derived matrices (view+0x98, built by g_buildMatrices, current+prev frame) for this
eye's. **Result: ZERO hits in a full session** — the engine does NOT reuse our built matrices
verbatim in any constant upload; its per-frame effect params are computed from its own camera
state (game-thread side, the CamCopy source buffers), not from the render-thread matrix block.
`47a35aa` (22:38) removed all three hot-path hooks entirely; baseline perf restored.

**NIGHT-END STATE (2026-07-14 ~22:40): baseline intact** — 120 fps, controls great, stereo,
full sharpness; left-eye ghosting remains (trailing after-images of bright pixels, follows the
first-rendered eye, also on the desktop mirror). Warm-up (3 renders) remains the only proven
remover and remains BANNED (halves fps).

**NEXT SESSION PLAN — no more headset-guessing; understand first (user-directed):**
1. **Study `reference/UEVR` (praydog)** — the user explicitly asked to learn from it. Focus:
   how UEVR handles per-eye temporal state at the D3D11 level (its stereo submission,
   AFR-vs-sequential handling, ghosting mitigations), and its general pattern of giving each eye
   a complete private view state rather than chasing individual buffers.
2. **Offline RE, no headset needed:** the ghost's effect params are computed game-thread-side
   from the authoritative camera. Anchors already known: camera-copy fn `halo3+0x2A628C` and its
   src buffers (logged每 session as `src=`), inner renderer `0x286A14`, prepare `kPrepareViewSig`,
   central constant-upload funnel `0x2AF8C1`. Use `tools/disasm.py` (live) / capstone on-disk to
   trace what reads the camera src buffers besides CamCopy (camscan `xref`/`findwrite` on
   `src+0x28` READS was never done — the M3 hunt watched WRITES). Identify the per-frame effect
   param computation (sun screen pos / prev-frame reprojection), then re-run or patch it per eye
   inside RenderViewHook exactly like g_prepareView.
3. Only then ship a fix build. All five ordering fixes, six buffer fixes, and two constant-level
   fixes are catalogued above — none of them may be repeated.

**Answer to "would disabling the desktop view help?" — no, effectively nothing.** The desktop
image is Halo's own frame: the engine runs its frame-level post + HUD + Present regardless of
what we do, and our only added cost there is one blit. The frame cost is the world render, which
is why the third render halved it. Real perf levers, in order: (1) alternating order instead of
warm-up (v5, free); (2) MCC resolution — eye caches inherit the game's 2912x2100 backbuffer, so
lowering the game's resolution directly cuts every world render (this is also the RTX 2070 Super
story); (3) only if warm-up is ever required again, find the buffer via the two probes never
tried — hook `CopyResource`/`CopySubresourceRegion`, and read `PSGetShaderResources` at draw time
(catches bind-once-read-forever) — then isolate that one resource at 2 renders.

**Separate known issue: mono screen mode runs at ~62-67 fps while stereo runs at 117-118**
(`fps` log lines). Not investigated; the mono path blits a 2912x2100 backbuffer through the
shader path to a quad every frame. Only matters as the fallback view.

**Superseded v3 detail (frame-level discovery + stereo blanking):**
`VR_RecordFrameRtv` (called from OMSetRenderTargets when no redirect happened, plus a new
record-only hook on OMSetRenderTargetsAndUnorderedAccessViews, vtable 34) records RTV binds that
occur outside the eye passes while discovery runs. Promotion rule: sampled inside an eye pass
(`readSeen`) AND written only at frame level (`writtenOutOfPass && !writeSeen`) → the target's
RTV is AddRef'd into a blank list, and `VR_BeginRasterEye` clears those targets to black each
pass, so neither eye can sample the other's leftovers (the game refills them after the eye passes
each frame; mono untouched; gated on `per_eye_history` + its F1 checkbox). Expected log:
`M2: stereo-blanking frame-level history target WxH format=F ...` then the window-close line with
a nonzero blank count. Also new: `fps N (stereo on/off)` logged every 10 s (user reports lower
fps; steady-state hook overhead should be nil after the discovery window closes — correlate with
these lines).

**Next-session test script (user in headset):** enter a level → F2 → F11 → find sparkly canopy →
hold still: trails should be GONE in both eyes within ~2 s of enabling stereo (discovery window).
F1 → untick "Per-eye glow history": trails return (confirms); re-tick. Check the fps lines in the
log. If the blank count is 0, the accumulator read may be a stale pre-pass SRV binding or a
non-PS bind path: next steps are recording currently-bound SRVs at VR_BeginRasterEye via
PSGetShaderResources (catches bind-once-read-forever) and hooking CSSetShaderResources. If
blanking kills something legit (water reflections going black), narrow the promotion filter by
size/format from the logged lines. If controls ever feel inverted again: F1 menu shows the
current yaw/pitch signs and has the flip buttons.

**Watch out for:** writing camera position feeds the sim (leaning is clamped for this reason) — for
per-eye offset we want a *render-only* offset, so prefer offsetting at the render/projection level
rather than the sim camera if possible. Comfort: keep the mono screen as a fallback (F-key) until
stereo is solid.
