# Continuation / handoff guide

> **Current weapon/HUD authority:** read
> [`TEST-CHECKPOINT-2026-07-18.md`](TEST-CHECKPOINT-2026-07-18.md) before using any historical
> weapon or CHUD section below. It records the exact deployed hash, proven final-palette path,
> failed approaches, residual risks, and headset acceptance test.

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

## SOLVED 2026-07-19 — the "left-eye ghost" was CAMERA MOTION BLUR

**User-confirmed in the headset: "echoes are gone! bug fixed!!!"**

Root cause: Halo 3's multi-tap camera motion blur (`motion_blur_taps`, 2-20 stacked copies
along a velocity vector) derives its velocity from a previous-frame camera. With two eye
renders per frame, an eye's "previous" camera is the OTHER eye's — a constant fake velocity
(IPD + cant) that smeared bright content into discrete repeated echoes even with the head
perfectly still, worst in the first-rendered eye, bleeding into the second. Every symptom in
the falsified-fixes table below fits: ordering changes moved/flickered it, the discarded
warm-up render "fixed" it (it gave the blur a matching prev frame), and no bind-time RTV/SRV
hook could see it (the poison was a shader-constant velocity, not a texture).

Fix shipped: the engine's live blur tuning globals (`motion_blur_scale_x/y`, `motion_blur_max_x/y`)
are resolved BY NAME at runtime through the engine's own debug-var table ({name_ptr, type,
value_ptr} entries in .data — no hardcoded RVAs) and forced to zero every frame from
CamCopyHook while the F1 "Motion blur" toggle is off (config `motion_blur`, default 0 — also
the standard VR comfort choice). Toggling on restores the captured engine values live.
MCC's own menus expose no motion-blur option; this is the only switch.

The falsified-fixes catalogue below is retained as history — those categories were all real
work that ruled out every texture-side mechanism, which is what eventually left only the
camera/constant side standing.

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

**The `{+0x158,+0x1E8}` lead is OPEN AGAIN (corrected 2026-07-19; user reopened the bug):** the
2026-07-18 claim that this pair is the FP overlay view was WRONG — the FP pair is the sub-view
at `view+0x6C8` (`{+0x6D0, +0x8B0}`, see RE-notes). The main view's `{+0x158, +0x1E8}` is
consumed by vtable render method `0x28331C` via `0x295DC0`; its role is unproven, and the
previous-frame/temporal reading is back on the table.

**UNVERIFIED HYPOTHESIS with a free binary probe (2026-07-19):** the artifact's visual signature
(discrete repeated echoes of bright/high-contrast content along one direction, present with the
head still, first-rendered eye + mirror) matches **camera motion blur** whose "previous frame"
camera is the OTHER eye's camera — in two-renders-per-frame stereo, each eye's previous VP is the
other eye's VP, a constant fake motion equal to the eye separation + cant. None of the 14 logged
attempts ever tried the one decisive zero-code test: **MCC's own Video-settings Motion Blur
toggle.** If OFF removes the echoes, the target becomes precise: find the motion-blur pass's
previous-VP source and make it per-eye (the constant-ID map through uploader `0x2AF478` is the
offline route; `0x270000` is the known VP id). If OFF changes nothing, the hypothesis is dead —
either way one checkbox settles it.

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

### THE REMAINING DEFECT — head-motion wobble is a CLOCK problem, not a frame problem

Two bracketing experiments (~05:13/05:19 builds) settle the geometry for good:
- record = pure controller world pose (zero head terms) -> gun FOLLOWS the head.
  **Proof the render-side root carries the full camera orientation.**
- record = head-cancelled controller pose (engine-exact camera floats) -> gun correct at rest,
  counter-wobbles DURING head motion, one-tick fling on snap turn.

Conclusion: the cancellation frame is right; the *timing* is not. We write records on the 60Hz
sim clock; the renderer rebuilds the mesh on its own interpolated 120Hz clock, so every write is
stale by up to one sim tick of head motion. No sim-side formula can fix this — both directions
around the truth are falsified (DO NOT retry frame-algebra variants). The real fix is to apply
the cancellation on the RENDERER'S clock: locate the render-side FP rebuild (the recomposer that
consumes the bank halves + its root fetch) and intercept there, with the call-site disp32-patch
technique and the LTCG register-contract check from RE-notes. Candidate leads are in RE-notes
("the unresolved contradiction" section lists the interpolated buffers) plus a RenderDoc frame
capture to identify the skinning constant buffer writer.

Shipping state (05:19): head-relative exact-basis — correct whenever the head is not mid-turn.

Follow-ups from the same night's tuning passes:
- **hud_scale FALSIFIED** (user: "never worked"): the HUD renders through overlay camera 0 —
  the weapon's — whose tangents must stay world-matched for controller registration; elements
  1-3 are unused split-screen slots. Proper HUD sizing = find the chud system's own scale.
  Leads: `render_first_person_fov_scale` debug var (string at 0x7CAA40) and the chud block
  reachable via TLS+0x220 (we already write `chud+0x146` to hide the stock crosshair).
- **gun_scale is inert by design** (record scale zooms the camera via camera_control descent);
  mesh-only size lever still open.
- Orientation: the render root is the head camera, so replace the wrist-ancestor record with
  `Head^-1*Ctrl`; do not compose onto the engine record because it already contains the
  head bake. `Ctrl*Head^-1` produces `Head*Ctrl*Head^-1` (the observed inversion).
- `build/Release/camscan_dump.bin` (6.8 GB) is stale scan scratch — safe to delete; make
  camscan clean up after itself.

### FALSIFIED: shared HUD/weapon camera override

Never write a controller pose to `halo3+0x2D2F680` or CamCopy's destination at that object
`+0x08`. The headset proved this path also feeds the gameplay view: after F2+F11 the gameplay
camera followed the hand while the gun did not. Affecting HUD/gun during an external forward-vector
spin established correlation, not exclusive ownership. The entire `controller_overlay_camera`
experiment has been removed. Gun separation must happen after the render-packet interpolator
(`halo3+0x184B40`), where the weapon consumes the camera, without changing the camera itself.

### 2026-07-18 render-packet gun separation

The CE proof at halo3+0x184C84 moved gun/arms only; HUD and gameplay camera did not move. The real
interpolator entry is halo3+0x184B08. The DLL now hooks that function boundary and re-anchors every
returned 0x34 bone matrix rigidly to the right controller, using the cached wrist and camera_control
indices. Older bank/root/sway overrides are disabled whenever the new hook is installed. The HUD is
confirmed to be a separate CHUD consumer and must not be moved through any camera write.

User headset result: orientation, custom cursor, bullets and muzzle flash are accurate, but a
smaller inverse head component remains. The live final projection was 0.54296 horizontally
(tan=1.8418, about 123 degrees), proving the prior matrix override did run. The saved gun scale
was also 1.45; unlike older builds, the render-packet path now applies it, explaining much of the
face-filling weapon size. The installed test build resets gun_scale to 0.65 and adds a 121-160
degree game_fov_deg control, default 140. It writes compact-camera tangents before Halo rebuilds
viewport/culling/matrices and enforces the same tangents in the final projection and overlay camera.

Follow-up falsified the 140-degree override: it broke the submitted VR image and made CHUD worse,
so the slider and compact-camera override were removed. Downstream RE found the missing exact root:
after 0x184B08 interpolation, 0x2C13B8 builds the final 0x70-byte FP packets, then
0x2C17EF..0x2C1815 multiplies every packet matrix in place by the shared
`playerWeaponBase+0x23F0` transform via 0x120DF8. The render hook must therefore target
`Root^-1*ControllerWorld`, not an independently sampled `Head^-1*Controller`.

The first exact-root build regressed because it compared the sim-thread weapon pointer with
`TLS+0x568` on the render thread. Those are different engine TLS blocks, so every packet was
rejected and Halo's untouched face-locked gun rendered. The next build removed that comparison but
still inverted `simWeapon+0x23F0`; that did **not** turn it into the render root. Halo subsequently
applied `Root_render * Root_sim^-1 * Controller`, leaving residual head motion and distorting hand
translation into the reported forward-reach "wall".

The corrected hook passes 0x184B08's first argument (the player index) through and mirrors
0x2C1433..0x2C1463 byte-for-byte on the hook's current thread:
`GS TLS -> TLS+0x568 + player*0x2430 + 0x23F0`. That is the exact root later passed to 0x120DF8 at
0x2C17F7, so the active result is `Root_render * Root_render^-1 * Controller = Controller`.
`VirtualQuery` and the sim weapon pointer are no longer part of the render hot path. Controller
translation is also mapped linearly through the fixed F3 calibration instead of current-head
inverse followed by a separately sampled camera basis. There is no reach clamp. The hook reads the
root and writes only returned gun/arm bones; it never writes the root, HUD camera, or gameplay
camera. The old
`hud_scale` path was also removed from the menu: its writes hit inactive split-screen player slots
1-3 and never moved or resized the active CHUD. Never present those writes as HUD placement.

### AUTHORITATIVE 2026-07-18 final visible-palette correction

This section supersedes the older exact-root description immediately above. Final offline RE proved
that 0x2C13B8 builds marker/effect packets, not the visible gun. The visible mesh path is
`0x2C0D20 -> 0x184B08 -> 0x2C5A38 -> 0x2C561C`. Function 0x2C561C is the final skin-palette
consumer and receives the actual render root as argument 2, destination as argument 3,
interpolated source as stack argument 5, and bone map as stack argument 6.

The active solution pairs 0x184B08 and 0x2C561C function-boundary hooks. The first identifies the
right-hand slot and snapshots untouched interpolation. The second reconstructs a private
thread-local source using `Root_render^-1 * RightController`; Halo then produces
`Root_render * Root_render^-1 * RightController = RightController`. The composer caches the
authored `wrist^-1 * camera_control` relation before camera lag. The final hook rebuilds a
lag-consistent camera_control from the current wrist and applies one coherent rigid delta to every
influence, preserving the already-correct barrel alignment without inverse head motion.

Controller position is `pre-head gameplay origin + fixed F3 recenter map *
(controller - recentered head origin) * world scale`. It contains no current-head subtraction and
no reach clamp. Forward, lateral, and vertical hand motion are therefore linear and use one scale.
The active path writes only private palette scratch. It never writes a root, camera, HUD camera,
animation bank, or gameplay camera; all older bank/root/sway paths remain bypassed.

The first native-CHUD build hooked the final leaf transform at halo3+0x2F18E8 and guessed a
1152x640 canvas. Headset result: HUD broken in both eyes and it did not follow the hand. Do not
repeat this. That function is called for every expanded leaf; applying a guessed displacement and
scale there corrupts the widget hierarchy.

The replacement hooks the earlier CHUD anchor builder at halo3+0x2EE234. Each eye still projects
the right controller through that eye's already-built camera, but Halo's real anchor translations
are observed over the first complete stereo frame. Subsequent frames add one uniform
controller-projected offset in those measured native units at each root anchor. No canvas
resolution is guessed, leaf transforms are untouched, and no gameplay/gun camera is written.
This mirrors the supplied UEVR Borderlands 3 profile's architecture: weapon components are
attached independently to motion-controller hand 1, while UI is a separate overlay rather than
part of the gameplay camera.

Headset result: the anchor-builder version was also broken and did not follow the hand. FALSIFIED.
Screen-coordinate relocation is the wrong abstraction for a native VR HUD, even at an earlier CHUD
matrix boundary.

The first OpenXR-quad build also contained a concrete boundary error. 0x2D2670 is only one of
three sibling CHUD passes in the per-view overlay caller; 0x285AE9 calls 0x2D27E8, 0x285B99 calls
0x2D289C, and only then 0x285BD5 calls 0x2D2670. Hooking the last pass produced all the expected
"capture/quad active" log markers while the dominant native HUD remained in both eyes, so moving
the size slider appeared to do nothing.

The forward solution hooks all three unique function boundaries. Every first-eye CHUD subpass
accumulates in the same transparent D3D11 texture; all duplicate other-eye subpasses are suppressed.
During each tightly scoped subpass, the OM hook keeps Halo's internal scene-color rebinds pointed at
the capture RTV. The Present path repairs missing destination alpha, downsamples into a 1280x720
OpenXR swapchain, and submits the quad from the predicted right-controller pose. Its local offset is
+0.16 m up / -0.30 m forward; physical width is `0.38m * hud_scale` (clamped 0.18-0.80 m).
The gameplay camera remains head-controlled and never receives a controller pose. The independent
gun/arms render-packet hook remains active.

Performance: the old RTV census did COM queries and a 128-entry scan on almost every
OMSetRenderTargets call. It is now one-time discovery followed by an exact RTV pointer comparison.
The gun hook now follows current render TLS directly and performs no VirtualQuery calls.

### AUTHORITATIVE native-HUD GPU boundary correction

The three CHUD functions at 0x2D27E8, 0x2D289C, and 0x2D2670 build queued CPU render commands.
They do not execute the pixel draws while their hooks are active. The scoped RTV-capture approach
therefore restored the eye target before Halo flushed those commands, captured black, and suppressed
the duplicate eye; that is the exact cause of the headset result where the HUD vanished.

The current path uses those CPU hooks only to choose one CHUD-build eye and suppress its duplicate.
D3D11 VS/PS bindings and draw calls are observed at the real GPU boundary. Ten valid stereo samples
alternate which physical eye builds CHUD, and only shader pairs that follow the chosen ON eye while
remaining absent from the suppressed OFF eye are promoted. Matching draws are redirected to a
transparent texture and submitted as a controller-space OpenXR quad from the predicted right-hand
pose. No camera transform participates. Empty calibration batches retry, stale classifications
auto-reset, swapchain wait timeouts retain and retry the acquired image, and the quad uses the native
capture aspect so the fixed 1280x720 transfer does not stretch the HUD.

### 2026-07-19 — HUD panel headset-DISPROVEN and fully removed; native HUD is the accepted state (CURRENT STATE)

Two HUD builds happened this day. First, the `hud_zoom` layout poke (`[view+0x2B0]+0x174`) was
deleted end-to-end (config/menu/game.cpp; key accepted-and-ignored) — it never resized anything,
and 0x278EE0 remains brightness only ("only the opacity changed").

Second, the capture-diff panel was completed (both-eye snapshots, union blend, native-HUD erase)
and **failed in the headset: the panel carried ONLY the objective text; the rest of the HUD
(health/ammo/radar/grenades) was gone, and the on/off checkboxes had no visible effect.** The user
directed a full revert, so the ENTIRE panel machinery is deleted, not just disabled: ps_huddiff
shader, union blend state, per-eye scene-only capture textures + `VR_CaptureSceneOnly()` (which
cost ~3 full-res 4K CopyResource per eye per frame — real GPU time), `RenderHudPanel`, the panel
swapchain/quad, the Frame() erase/blit-source switch, and the `hud_panel*` config keys + menu
block (keys accepted-and-ignored). Do NOT rebuild a diff-based HUD capture without new evidence
about what the finished eye frames actually contain at Present time.

**ACCEPTED HUD STATE (user's explicit direction): the native HUD, full-size, drawn by the game in
both eyes, with ONLY the centered weapon reticle hidden via the verified 0x2EDF24 element hook
(F4-picked id, `kill_reticle`). Stop iterating on HUD size.** The eye-split (grenades+radar left,
ammo right) is part of that accepted state.

Bloat cleanup in the same build: dead `bullet_snap` config key removed (the code behind it was
reverted earlier); `bullet_probe` per-shot logger now defaults OFF (set `bullet_probe=1` in the
cfg when hunting the fire hook); `weapon_probe`/`hud_probe` stay available but off. The "Game
brightness" slider moved out of the HUD menu block into its own "Picture" section.

**EVENING FOLLOW-UP — the REAL HUD breaker found and removed.** The panel revert did NOT restore
the HUD (user: still only the objective visible). Root cause was neither the panel nor hud_zoom:
`EnforceHudElements()` (every frame from CamCopyHook) and `ChudStateCopyHook` (0x32F97C hook)
force-wrote `chud+0x144..0x14A` with the chud_show_* offset map — which the headset had already
DISPROVEN (0x146 = nav dot, not crosshair; the snapshot copies only 0x144..0x147). The stomping
suppressed everything but the objective text, and made the F1 "Show HUD"/element checkboxes dead.
Both functions, the 0x32F97C hook install, and the `show_hud`/`hud_*` config keys + menu block
are REMOVED — the CHUD byte block is fully game-managed again. HUD element control is ONLY the
verified 0x2EDF24 element-hider (reticle id 1580 in the user's cfg). Also reset the user's saved
`game_brightness` 1.29 → 1.0 (washed-out "white" look). NEXT TARGET (user): runs on a 4060M
laptop w/ 16 GB RAM — keep hot paths lean; user reports VRAM ~2 GB higher than the F4-era build,
unexplained after these removals — measure before chasing.

### 2026-07-18 EVENING — flat-gun root cause found; HUD machinery retired

The morning headset test ran the full pipeline above (log-proven) and still showed: gun in "its
own near field" at a different depth than the world, no forward reach, residual head-follow, HUD
invisible, fps 67-90. Offline RE then found the actual cause, and it was never a transform bug:
the first-person layer (gun + arms + CHUD) renders through the view's SECOND camera pair
`{+0x158, +0x1E8}`, rebuilt by `0x279BEC` from the CENTER-eye pose with a fixed viewmodel FOV
inside every per-view render, immediately before each FP draw pass — so the FP layer was drawn
identically in both eyes: a zero-disparity flat mono layer over a stereo world. Full proven chain,
addresses, and ABI notes: RE-notes "AUTHORITATIVE first-person camera chain".

Shipped (see TEST-CHECKPOINT-2026-07-18.md for the current hash + acceptance test):
1. `FpCameraRebuildHook` on `0x279BEC` — after the engine's rebuild, while one of our eye renders
   is active and the argument equals that view's FP SUB-VIEW (`view+0x6C8`), it substitutes the
   eye's world compact camera into `sub+0x08` and the eye's derived block into `sub+0x1E8`, then
   re-calls the engine's own constant uploader `0x2770F0`. The gun/HUD layer therefore renders
   per-eye with the world-matched projection. (The first build of the hook matched the view BASE
   — `per-eye FP camera active` never logged and the gun stayed a mono layer; corrected
   2026-07-19. If the marker is ever absent again while the install line is present, read the
   `M3 DIAG: FP rebuild on ...` pointer log.)
2. The ENTIRE CHUD steal-and-requad machinery is deleted (three CHUD hooks, PS/VS/draw-call
   classifier, capture texture, hand-HUD quad, hud_scale). It removed the HUD from both eyes,
   never displayed its quad, and its calibration retry loop cost ~30 fps. The native HUD renders
   untouched again; the stock crosshair stays suppressed and the floating reticle stays.
HUD comfort (size/centering/edge-split) is the explicit next iteration — the FP camera is now
under our control, which is the first clean lever for it.

## Current state: M0, M1, M2, M3 all working

- **M0** — inject, launch EAC-off, D3D11 Present hook, OpenXR session, in-headset ImGui menu (F1).
- **M1** — head rotation + leaning drive the game camera; camera found by AOB signature.
- **M2** — true per-eye stereo at **120 fps**, full sharpness. Confirmed "3D is great".
- **M3** — full VR controls: hand aim, sticks, buttons, snap/smooth turn, D-pad gesture.

**Baseline to protect (commit `5485596` lineage): 120 fps + controls + stereo.**

**Known open issues:** the left-eye ghost above; mono screen mode runs ~62-67 fps while stereo
runs 117-118 (not investigated; only matters as the fallback view); the gun MODEL is still
camera-glued (only aim follows the hand); HUD elements split between the eyes (grenades+radar
left, ammo right) — ACCEPTED as-is per the user 2026-07-19; do not iterate on HUD size.

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
