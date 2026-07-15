# Continuation / handoff guide

Operational summary + current frontier. Read `CLAUDE.md` first (hard rules, who the user is).
`docs/PLAN.md` = milestones. `docs/RE-notes.md` = reverse-engineering findings.

**The user is not a programmer.** Claude writes all code; the user runs the game, tests in the
headset, reports back. Give exact click-by-click instructions. Explain jargon.

**Process rules learned the hard way — violating these costs real play sessions:**
- **No fallbacks, no tradeoff options, no degraded-mode defaults.** Ship one correct path.
  (An fps-halving "safety net" default cost a session and real trust.)
- **Understand offline first. One decisive build per headset session.** Do not ship theories.
- **Never regress fps or controls.** 120 fps + working controls is the floor, not a goal.

---

## KNOWN MAJOR BUG — left-eye ghosting (open, deprioritized 2026-07-14)

**Symptom:** trailing after-images of bright pixels (sparkles/canopy) in the **first-rendered
eye** only. Present with the head perfectly still. Also visible on the desktop mirror.
Flipping `right_eye_first` moves it to the other lens — that is the reliable repro.

**Status: ACCEPTED AND MOVED ON FROM at the user's direction.** Playable. Do not spend another
session on it without new evidence. ~14 fixes across 5 categories have failed; the catalogue
below exists so nobody repeats them.

**The only proven remover is BANNED:** a discarded warm-up render
(`[firstEye discard, firstEye, otherEye]`) removes it completely — and halves fps to 60,
because 3 world renders cannot fit a 120 Hz budget at any resolution (Halo's per-render cost is
~4 ms of CPU/engine work, nearly independent of pixels — measured: 2 renders @ 12.2 MP = 117 fps,
3 renders @ 10.9 MP = 60 fps). SteamVR then paces to 60. There is no 80 fps on a 120 Hz headset.

**The empirical rule that fits every result:** *whichever render crosses the frame boundary
absorbs the poisoned state, regardless of eye identity or camera anchoring.* No ordering trick
dodges it; only a discarded render absorbs it.

**DO NOT REPEAT — all falsified in headset tests:**
| Category | Attempts | Result |
|---|---|---|
| Ordering | fixed order; alternating parity (v5); post-anchor to first eye (v7/v9) | fixed → steady first-eye ghost; alternating → ghost flickers between eyes; anchoring → unchanged |
| Buffer isolation | fp16 ≤half-res set; widened full-res; R10G10B10A2; cross-pass discovery (v2); frame-level blanking (v3) | 0 targets promoted in every discovery variant; widened full-res was actively harmful (radial smears) |
| Copy substitution | learned scene-snapshot pairs, dst-only then dst+src | ghost persisted both times |
| Shader constants | UpdateSubresource/Map/Unmap census; exact-match matrix matcher | all uploads funnel through ONE call site (`halo3+0x2AF8C1`, ~100k/s) so caller-RVA cannot discriminate; matcher scored **zero hits** — the engine does not reuse our built matrices verbatim |
| Effects | sun-shaft (R16G16 occlusion target) neutralization | A/B checkbox made no difference |

Four independent probes came back **empty** (in-pass RTV writes, in-pass PS SRV reads, UAV
binds, frame-level RTV writes). Conclusion: the poison is not reachable by any bind-time hook.

**The one lead never tested** (offline, no headset needed — see RE-notes for addresses):
Halo's view struct holds **two** camera+matrix slots: `{camera@+0x08, derived@+0x98}` and an
identical pair at `{+0x158, +0x1E8}`. That is the classic *current view / previous view* layout
used for temporal reprojection — the shape of this bug. `RenderViewHook` (game.cpp) currently
sets **prev := current for both eyes every pass**, which would zero any reprojection delta;
those two memcpys arrived unexplained in `bd2254e` and were never justified. **Open question:**
is `{+0x158,+0x1E8}` previous-frame state or a secondary view? Settle it by finding the
engine's own writer of `view+0x158` (a per-frame current→prev copy proves it). If it is
prev-frame state, the fix is giving each eye its own memory of its own last frame — memcpys,
two renders, no fps cost. Related unfollowed note: RE-notes records the world render camera as
*"a sliding array/ring (render history for temporal effects)"* around `~0x468xxxx`.

---

## BREAKTHROUGH 2026-07-15 ~04:20 — the gun mesh finally tracks the controller

The working lever, after every other one was falsified in-headset: **write the controller pose
(quat i,j,k,w + translation + scale, 0x20 bytes) into the orientation-bank record of the wrist's
ancestor node directly under the skeleton root** (index found by walking the tag node table's
parent words), inside the compose hooks before the original runs. The renderer rebuilds the mesh
from the bank's child records under its own AIM-camera root — it replaces record 0 (why bank-root
writes only nudged the camera) and keeps children. Composed-output/defaults-root writes reach
ONLY markers/effects + a camera_control readback that nudges the camera with the wrist (the
"wrist moves the world/body" reports, and the illusion behind 01:55's "gun follows both").

Confirmed in-headset: gun AND bullets track the right controller. Same-session tuning shipped
(04:29 build): aim-frame anchoring (kills the inverse-head drift — the render root is the AIM
camera, not the head), mounting-angle sliders (barrel authored ~90 deg off; default pitch -90),
reticle 2.25 deg, gun_scale 0.75 + Home/End, experimental HUD-size slider (scales overlay
cameras 1-3 only; weapon camera 0 stays world-matched for registration). Awaiting headset pass.

## Current state: M0, M1, M2, M3 all working

- **M0** — inject, launch EAC-off, D3D11 Present hook, OpenXR session, in-headset ImGui menu (F1).
- **M1** — head rotation + leaning drive the game camera; camera found by AOB signature.
- **M2** — true per-eye stereo at **120 fps**, full sharpness. Confirmed "3D is great".
- **M3** — full VR controls: hand aim, sticks, buttons, snap/smooth turn, D-pad gesture.

**Baseline to protect (commit `5485596` lineage): 120 fps + controls + stereo.**

**Known open issues:** the left-eye ghost above; mono screen mode runs ~62-67 fps while stereo
runs 117-118 (not investigated; only matters as the fallback view); the gun MODEL is still
camera-glued (only aim follows the hand); HUD elements split between the eyes (grenades+radar
left, ammo right).

## 2026-07-14 hands/guns + crosshair handoff

- Implemented an OpenXR quad-layer aim crosshair using the game's actual aim direction mapped
  back into LOCAL space. It is a 64x64 static alpha reticle, submitted after the stereo projection
  and before the F1 menu, so it adds no third game render.
- Added F1 settings and config persistence: `crosshair`, `crosshair_distance_m`, and
  `crosshair_size_deg`. Defaults: on, 10 m, 1.2 degrees.
- Added AOB-located hooks at the proven first-person global-bone composition boundary. The full
  animated hands + gun assembly is moved from its selected weapon anchor to the tracked controller:
  primary/right slot to the right controller, dual/left slot to the left controller. This happens
  before Halo copies matrices into the render packet, so the visible model receives the pose.
- Headset acceptance check: right hand and gun stay at the right controller while the head moves;
  reload/recoil remain animated; dual-wield left gun follows the left controller; the crosshair
  follows the actual bullet direction. Confirm baseline remains 117-120 fps and controls unchanged.
- The prior user test used the older installed DLL (its timestamp/size preceded the crosshair
  build), so it did not test the OpenXR crosshair or these first-person matrix hooks.

### First hands/guns headset result and correction

- Report: the weapon moved, but pieces separated; bullet/gun/crosshair tracking all worked but
  were unsynchronized, and the crosshair had substantial input lag.
- Root cause 1 is proven and fixed: Halo's 0x34 matrix starts with scale, not rotation. The bad
  declaration shifted every basis vector.
- The attempted direct-angle resolver found zero candidates and never activated. It is removed;
  do not describe that path as a working synchronization fix.
- First direct-aim resolver build froze the game thread after F2 while XInput continued. Runtime
  timing proved the initial scan had not reached its first completion log. Cause: `VirtualQuery`
  was called for every 4-byte candidate (hundreds of thousands of kernel calls). Fixed by checking
  each allocation once, then comparing its already-validated float range directly.
- Next headset report: cursor and muzzle flash followed the hand, while visible gun and bullets
  followed the head. This proves composed matrices feed markers/effects but mesh skinning consumes
  the local orientation bank. The hook now transforms only the bank root and recomposes, instead
  of editing every completed matrix.

### 2026-07-15 headset result and the three-part correction

Report: gun followed **both** head and hand; bullets shot **from the head** (called a
regression); gun barely visible, size unverifiable. All three were diagnosed offline from the
logs + statics and fixed in one build:

1. **Head/hand mixing was caused by the camera-copy "scoping" experiment** (save/restore of the
   authoritative camera around the copy so gameplay would keep the aim pose). The first-person
   bone rewrite expresses the controller **relative to the head camera**, and every proven-good
   result (muzzle flash at the hand) was measured with the head pose living permanently in that
   camera. Splitting the frames made the FP assembly consume the aim pose while our math
   subtracted the head. The scoping is reverted — the M3 regime (head pose stays in `src`) is
   load-bearing for the FP frame; a comment in `CamCopyHook` now says so.
2. **"Bullets from the head" is origin parallax made visible, not a steering failure.** Halo
   spawns first-person projectiles at the camera (the head); no stick steering can move that
   origin. It became conspicuous because (a) the gun now sits in your hand and (b) the crosshair
   was switched to the raw controller ray *from the controller*, so head-origin shots visibly
   missed the reticle. Fix shipped: `Game_ComputeAimStick` now steers the head-origin bullet ray
   **through the point the hand ray reaches at `crosshair_distance_m`** — every shot passes
   exactly through the floating reticle (exact at that distance, negligible error beyond it).
   The complete fix remains a weapon-fire hook that swaps origin+direction around the call
   (HaloCEVR's pattern); the offline hunt for H3's fire function is still open (see RE-notes).
3. **The gun was shrunk twice**: overlay frustum scale 2.0 (draws at half size) x mesh scale
   0.33 = ~1/6 apparent size. Now the overlay tangents are **pinned to the exact world match**
   (required anyway so the weapon projects at the controller's true position) and weapon size is
   a single mesh scale `gun_scale` (config + F1 slider), default 1.0 = authored size,
   **Home/End** adjust it live around the wrist anchor.

### Result of that build: (1) FAILED, (3) untested, and a process failure

Headset report: **gun and bullets follow the head; reticle and muzzle flash follow the hand.**
That is verbatim the *earlier* report — so reverting the scoping moved us backwards, and item (1)
above was wrong. Worse, the RE-notes "correction" that justified it (blaming the r_hand string
index) was **a theory written into the docs as a finding**. It has been retracted there. This is
the "understand offline first / do not ship theories" rule being broken twice in a row, at the
cost of two play sessions. **Do not ship another weapon-frame theory.**

What the follow-up offline pass DID establish (all in RE-notes, all real):
- The composed FP bones are **camera-space, proven** from the composer's root transform
  (scale 1, identity rotation, ~zero translation; the offset divisor is literally 1000.0).
  So the *space* our hook writes in is correct.
- There are only **two composers and four callers** binary-wide; we hook the only two
  first-person ones; argument orders verified register-by-register; the nesting guard is right.
- Our edits provably reach BOTH the effects anchor (muzzle flash — observed) AND the mesh's own
  render packet (`memcpy` at `0x2C490F`, sharing its gate with the effects publish).

### SOLVED — the head-stuck gun (2026-07-15, proven in the disassembly)

The contradiction above resolved: **the engine undoes our pose one instruction after we write
it.** At `0x2C4843-0x2C486A` the first-person evaluator rotates every composed bone by Halo's
camera-driven weapon-lag matrix, **skipping only `camera_control`** — which is cached earlier at
`0x2C4695` and is exactly what feeds the muzzle flash. Hence, verbatim, both headset reports:
flash follows the hand, mesh rides the head. In VR the head drives that camera constantly, so the
lag rotation *is* the head-stick. Full instruction-level trace in RE-notes.

**Fix:** hook the shared applier `0x120DF8` (unique AOB) and skip it for exactly the bone range
we re-anchored, armed by a thread-local set only on a successful transform. The engine already
skips that rotation for one bone, so this is a state the pipeline supports.

**Guard rail:** `0x120DF8` has ~121 call sites and is hot. The hook must remain a range compare +
tail call. Do not add logging, atomics, or math there — a comparable-frequency hook (the constant-
upload census) cost ~25% fps.

The `weapon_probe` flag (F1 checkbox / config, default OFF) stays: it pushes every composed bone
0.3 wu left with no controller input, and is the fastest way to re-confirm the mesh consumes
`weapon+0x4A4` if this area ever regresses.

## Key paths / environment

- Game: `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection`
- Host: `MCC\Binaries\Win64\MCC-Win64-Shipping.exe` (build **1.3528.0.0**)
- Engine DLL (RE target): `halo3\halo3.dll` (loads only once you enter a Halo 3 level)
- Installed mod: `...\Halo The Master Chief Collection\halo3xr\` (dll, launcher, logs)
- Runtime: SteamVR/OpenXR, PSVR2 via Sony adapter. Dev GPU RTX 5070 Ti.
- CMake: `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- Python 3.13 + `capstone`. Reference: `reference/UEVR` (praydog) — study its pattern of giving
  each eye a complete private view state rather than chasing individual buffers.

## Build / deploy / test loop

```
cmake --build n:\dev\halo3-openxr\build --config Release --target halo3xr
# deploy: copy build\Release\halo3xr.dll into the game's halo3xr\ folder
#   (the game must be CLOSED — the DLL is locked while it runs)
```
Launch via the **"Halo 3 VR"** desktop shortcut. Logs: `halo3xr.log`, `halo3xr_launcher.log`
(`.prev` keeps the last run).

## Architecture (src/)

- `dll/dllmain.cpp` — init thread: log, config, MinHook, D3D11 hooks, OpenXR, `Game_Init`.
- `dll/d3d11_hook.cpp` — hooks Present(8)/Present1(22)/ResizeBuffers(13) and
  `OMSetRenderTargets` (context vtable 33). **The RTV redirect is what makes stereo work.**
  A comment block lists the hooks deliberately NOT installed and why — do not re-add them.
- `dll/vr.cpp` — OpenXR session on the game's device; per-eye raster (`VR_BeginRasterEye` /
  `VR_CaptureRenderedEye` / `VR_EndRasterEye`); projection layer submission; menu quad.
- `dll/game.cpp` — camera-copy hook (head look, aim capture), `RenderViewHook` (renders the
  world once per eye with per-eye camera/cant/projection), turn + move mapping.
- `dll/input.cpp` — XInput virtual gamepad; **claims the exe's IAT slots** (Steam Input patches
  MCC's import table and routes around MinHook detours) and re-asserts every 2 s.
- `dll/menu.cpp` — window proc hotkeys + focus spoofing; ImGui.
- `common/` — logging, `halo3xr.cfg`.

## RE toolkit (tools/)

The user's AV blocks Cheat Engine, so we built our own **read-only** tools.
- **`pedis.py` — OFFLINE static disassembler** of halo3.dll on disk (no game running):
  `fn <rva> [len]` disassemble · `sig "<AOB>"` → RVAs · `scan <disp>...` find `[reg+disp]`.
  **Gotcha:** capstone's `disasm()` stops at the first undecodable byte; a naive linear sweep
  silently skips almost everything (50 hits vs 405 for the same scan once resync was added).
- `disasm.py <rva_hex> <len>` — capstone disassembly of the **live** process only.
- `verify_sig.py` — confirm an AOB is unique on disk.
- `camscan` (build `--target camscan`, run from `build\Release`, game must be in a level):
  `attach` · `first`/`narrow` differential float scan · `clusters` · `watch` · `poke`/`spin`
  (write test) · `findwrite`/`xref` (hardware watchpoints) · `hex`.

## Hotkeys

F1 menu · F2 head-tracking · F3 recenter · F6 leaning · F8/F9 pitch trim · F10 screen-follow ·
F11 stereo · PgUp/PgDn lean strength · Home/End hand-held weapon size.
Yaw/pitch/up calibration flips are **F1-menu-only** (a stray Alt+F4 from SteamVR's exit path
used to hit the old F4 binding and invert head-turn + hand-aim — it read as "controls completely
broken". Fixed: hotkeys act on `WM_KEYDOWN` only and Alt+F4 passes through to the game).

## Root causes worth remembering

- **Controls dying between sessions** = **Steam Input patches MCC's IAT**, routing XInput calls
  around our detours; whether our hook saw traffic was a per-session race. Fixed by claiming the
  IAT slots directly and re-asserting. MCC also gates ALL XInput polling on
  `XInputGetCapabilities` reporting a connected pad — we fabricate one unconditionally.
- **"Head tracking completely broken"** was the phantom Alt+F4 above, not a ghost fix.
- Writing camera position feeds the sim (leaning is clamped for this reason).
