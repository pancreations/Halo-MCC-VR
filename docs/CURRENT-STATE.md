# Current state

Authoritative as of 2026-07-20. If another note conflicts with this file, this file wins. Historical experiments remain available in Git history; they are not implementation instructions.

## Recovery points

- Headset-proven baseline: 330a568
- Protected branch: recovery/best-working-20260719-1300
- Safe documentation-cleanup branch: cleanup/production-baseline-20260719; runtime restored by ddfe109
- Failed broad runtime-cleanup commit: 42a1276 (preserved for diagnosis; never deploy)
- Pre-recovery master: ad61f8d
- Lean HUD scaling and reticle-hider checkpoint: 65113ab
- Runtime-FOV HUD aspect checkpoint: 1b53139
- Failed runtime reticle-classifier experiment: f0d5a88; narrowly reverted at 7fdf019
- Headset-confirmed native CHUD crosshair-class fix: c923842; confirmation notes at 8aa45d7
- Current resolution-preset checkpoint: 1fc56c8 on feature/resolution-scale
- User-designated best-working pause/controls checkpoint: `8ea1c04` on
  `feature/menu-controls` (runtime changes in `73f81f1` and `a06ebd5`)
- User-designated best-working dual-wield runtime checkpoint: `c2e6a27` on
  `feature/dual-wield`; deployed DLL SHA-256
  `4D7FE27DD501AD9110DF9905DB825C9CA545021431ED4BE910CBF46D76064E5A`
- Current best-working checkpoint: `recovery/best-working-20260720`, also the
  `master` tip. Adds the headset-confirmed smooth-turn jitter fix and the
  optional floating-hands mode on top of the dual-wield build. Deployed DLL
  build 2026-07-20 12:35 AM, SHA-256
  `a9da286ddd983306010a812608aaefce41f912115bd5eb6b9cc7a8bfbfc61459`
- Headset-confirmed camera-recoil runtime checkpoint: `56dad79` on
  `fix/vr-camera-recoil`. Deployed DLL build 2026-07-20 05:04 AM, SHA-256
  `6ED54EAC5084C0B8D76FCD5BE40A2023269FDC35D3D52054BB926DF97EA24177`.
  User result: the unwanted firing shake is fixed.
- Cutscene-culling checkpoint: `c828717` on `fix/cutscene-culling`. It
  disables Halo's cinematic-only widescreen FOV reduction while stereo VR is
  active, using the engine debug variable resolved by name. Headset result on
  2026-07-21: "thats good i think it works." Treat that as a positive initial
  result; broader cutscene coverage is still pending.
- User-designated best-working cutscene checkpoint: `dd1abc5` on
  `fix/cutscene-shot-facing`, preserved at
  `recovery/best-working-20260721-cutscenes`. It keeps the accepted culling
  correction and uses signature-verified cinematic scene/shot state to align
  yaw with each authored camera cut and the return to gameplay. Deployed DLL
  build 2026-07-21 02:05 AM, SHA-256
  `14A504EE4CE4CEB380BA94CAD1CBCE05CC2586D9D5D803651DE89AE50C4F61D2`.
  The user designated this the best-working version and wants further local
  bug-fixing from this build before any GitHub update.
- Pose-smoothing/gun-calibration branch: `a229dfb` passed the independent
  crosshair-smoothing slider and all three local gun-trim axes in the headset,
  but its HMD path caused nausea and 0% did not remove the perceived delay.
  Comfort correction `8d81f61` samples the next predicted display pose, makes
  raw 0% the default, caps opt-in smoothing at 10%, and adds a Raw button.
  Deployed DLL build 2026-07-20 05:41 AM, SHA-256
  `939DC5F9E86F5FCED1244BBB2AA584B0F277C6815AD855494FA6DEBB2413820D`.
  HMD comfort validation is pending; do not promote it over `56dad79` yet.

Do not rewrite or delete the recovery branch. Start new experiments from a named branch or commit.

- Exact-time OpenXR pipeline candidate 619ce49 is Quest 3
  headset-confirmed smooth at about 100 fps through Virtual Desktop. Authored
  per-weapon crosshairs and stock target-color states from 0ff031a are also
  headset-confirmed working on Quest 3. 1897b43 expands their angular-size
  control from a 5-degree maximum to 20 degrees.
- Quest Pro has limited early coverage, not full validation. The tester reported
  smooth HMD timing; their skyward aim was traced to MCC's inverted-Y game
  setting, not a Quest Pro pose failure. Touch Pro profile support from 846abc1
  remains enabled when advertised by the runtime. The latest shared
  weapon-calibration path still needs a Quest Pro result.
- Commit 4089b34 makes the obsolete procedural reticle fallback fully
  transparent so it cannot appear close to the viewer during death. The
  authored weapon texture upload is unchanged. The user confirmed the
  death-state crosshair problem is fixed.
- Shared-pose calibration commit 361789c moves gun pitch/yaw/roll offsets from
  mesh-only transforms into VR_GetAimPose and removes the duplicate right-hand
  mesh rotations. The visible weapon, muzzle, authored crosshair, target state,
  and bullet steering now consume one corrected pose. Quest 3 headset result:
  user loves the new implementation. Quest Pro confirmation remains pending.

## Headset-confirmed behavior

- Halo 3 level load, OpenXR session, true stereo, head rotation, and positional tracking work.
- PSVR2 Sense controls are merged through XInput and work in gameplay and menus.
- The right controller drives the weapon and aim. The floating VR reticle follows the actual aim ray.
- First-person arm IK bends the right and left arms from stable shoulder anchors.
- The shotgun support arm is free and follows the left controller. The user confirmed this after the native weapon-IK bypass.
- Dual wield is headset-confirmed nearly perfect in `c2e6a27`: the secondary
  gun is owned by the solved left hand, and the hand tracks the left controller.
  The user designated this the best checkpoint yet.
- The assault rifle and pistol were the known-good comparison behavior before the shotgun fix.
- The game's native HUD renders in both eyes. The class-gated CHUD fix hides native weapon crosshairs across the tested weapons while preserving the VR reticle and the rest of the HUD.
- Native HUD scaling at 0.38, CHUD scripting-class-2 crosshair hiding, and normal pre-regression GPU performance are headset-confirmed.
- Runtime-FOV HUD aspect correction is headset-confirmed on both Quest 3 and PSVR2 with OpenXR Toolkit disabled. It substantially improves the layout; a mild overall squeeze remains and is accepted for now.
- The left support-hand wrist-to-palm correction is headset-confirmed. `left_hand_forward_m` defaults to 0.12 m and drives both the rendered left-hand IK target and the two-handed aiming point; the F1 slider tunes them together.
- Halo motion blur is off by default because its previous-camera state creates stereo echo trails.
- Weapon firing no longer shakes the HMD. The dedicated post-observer camera-
  effect stage is bypassed while head tracking is active; the user confirmed
  the recoil fix in the deployed `56dad79` build.
- deploy.bat auto prevents stale-binary testing by refusing to deploy while MCC is open and comparing both the DLL and launcher byte-for-byte with the fresh Release outputs.
- L3+R3 opens the F1 menu and the headset pointer is usable. Controller
  vibration is working. The Status tab exposes independent F2 head-tracking and
  F11 stereo buttons plus their live state, and recenter resets both Halo's
  camera reference and the OpenXR screen reference.

## Production implementation

The active path is deliberately small:

1. RenderViewHook renders two eye passes and supplies each eye's camera/projection.
2. FpDriverHook and FpCameraRebuildHook stamp the active eye camera into Halo's first-person draw path.
3. The interpolation and visible-palette hooks preserve the authored skeleton, solve both arms once per stereo pair, and provide the render palette. During dual wield, slot 1 solves the left arm and then applies that solved hand delta to the gun/right-hand descendant subtree.
4. The marker/muzzle path follows the same ownership as the visible weapon: slot 0 keeps the established controller path, while dual slot 1 inherits the solved left-hand transform.
5. Halo's normal input/aim path steers projectile direction through the VR reticle point.
6. A signature-located chud_draw_widget patch removes only the normal-playback short-circuit around Halo's own scripting-class check. The hooked visibility predicate hides the widget only after Halo identifies it as class 2 (crosshair), preserving the VR reticle and all other HUD classes without runtime tag-table dereferences.
7. At startup, a unique signature changes the stock first-person weapon-IK decision from 74 05 to EB 18. This selects Halo's own no-weapon-IK branch and prevents shotgun-specific authored pump grip IK from overriding the left controller arm.
8. A unique owner-code signature resolves Halo 3's native pause byte. The H3EK
   `game_paused` external global is not used because live testing proved it does
   not follow MCC's pause menu. When the native byte is available it owns the
   2D/3D transition; missing or ambiguous signatures retain the edge fallback.
9. A unique signature hooks Halo's post-observer camera-effect stage. While VR
   head tracking is active, authored recoil/explosion screen-shake transforms
   are bypassed so the HMD view remains owned by OpenXR. With head tracking off,
   Halo's original function runs unchanged. Firing-recoil suppression is
   headset-confirmed in build `56dad79`.
10. Candidate `8d81f61` samples and filters the HMD exactly once per predicted
    OpenXR frame. Because Present occurs after Halo rasterizes, it locates the
    next display-period pose for the image Halo is about to render. Headset
    smoothing is raw by default and capped at 10%; the independent confirmed
    crosshair-only filter uses the existing aim-stabilization setting. Runtime
    logs report HMD samples/sec and camera transforms/sec.
    Weapon automatic alignment now establishes the zero-trim pose before local
    pitch/yaw/roll calibration, so the default is preserved while all axes work.
    Extreme latched two-hand lines fall back to the right-controller ray instead
    of allowing a support hand far off the barrel to aim vertically.

The working runtime still contains dormant diagnostic and fallback code inherited from 330a568. Its defaults are the headset-proven behavior. Do not enable, remove, or consolidate those paths in bulk: commit 42a1276 performed a broad cleanup, built successfully, then produced a fatal error at the first level transition. Remove only one independently understood path per branch and headset test.

## Safety invariants

- Never write the gameplay camera, animation banks, model roots, or CHUD state bytes from an unproven guessed offset.
- Never hook halo3+0x120DF8; a pass-through detour crashed during level load.
- Verify every AOB is unique before hooking or patching. On zero or multiple matches, log and leave the stock path intact.
- Keep render/palette hooks deterministic: no file I/O, logging bursts, locks, COM queries, allocation, or scanning.
- Preserve finite-value and index/count guards around every bone and matrix operation.
- Do not deploy with a compiler failure. Do not manually copy a DLL.
- One hypothesis per headset build. A failed experiment is reverted, not hidden behind a config flag.
- Headset observation is the acceptance test. Desktop output and logs are supporting evidence only.

## Failure ledger: do not repeat

| Attempt | Headset/runtime result | Rule |
|---|---|---|
| Alpha uninstaller trusted any folder containing halo3xr.dll, then used recursive rd | A tester extracted/placed package files in the MCC root; confirming uninstall recursively deleted the entire game installation | Never infer ownership from an adjacent DLL and never recursively delete an install directory. Require exact halo3xr leaf plus MCC-parent validation, delete only an explicit allowlist, preserve unknown files, and keep the fake-tree regression test in CTest |
| Detour halo3+0x120DF8 | Crashed on level load, even pass-through | Never hook it |
| Force CHUD bytes near +0x144..+0x14A | Suppressed most HUD; offsets were misidentified | Do not write CHUD state bytes without proof |
| Capture/diff HUD panel | Captured only objective text and cost GPU time | Native HUD is the accepted path |
| Treat 0x278EE0 as HUD scale | Changed brightness/alpha, not HUD size | It is the brightness control only |
| Poke view+0x2B0+0x174 for HUD zoom | No visible HUD size change | Do not restore the setting |
| Rewrite composed wrist/output bones for bullet origin | Spun the hand and sent shots sideways | Projectile work requires a proven fire boundary |
| Post-edit composed weapon bones | Mesh and muzzle diverged under weapon lag | Transform at verified interpolation/palette boundaries |
| Guess body debug variables | Values did not resolve reliably; on-values were guesses | Full body needs a proven render path |
| Synthetic shotgun palette fallback | Did not affect the stuck support hand | Native weapon IK was the overriding consumer |
| Unverified manual deployment | Re-tested a stale DLL across sessions | Use deploy.bat auto and compare timestamps/hashes |
| Right-eye-first/trace/census modes | Useful once, no production behavior | Recreate temporary probes on a disposable branch only |
| Runtime tag-table classifier in the CHUD submit hook (f0d5a88) | OpenXR initialized and submitted frames, then the headset went black when stereo entered the level | Keep 7fdf019's narrow rollback; never dereference the runtime tag table from this hot hook |
| Treat 0x62C, 0xF70, or 0x1A90 as a portable crosshair element id | One chosen weapon could lose its cursor while other weapons retained theirs; the F1 checkbox could also hide a weapon HUD icon | r8w is a runtime chud_definition tag index, not a universal reticle id; use Halo's validated class-2 gate |
| Hook only halo3+0x2EDE38 | No crosshair change in normal gameplay | Normal play short-circuits before the class check; use the validated c923842 path |
| OpenXR Toolkit FSR with the current stereo-array path | VR View showed tiled/overlapping stereo regions rather than an intact lower-resolution eye | Do not bundle or depend on Toolkit FSR; scale Halo's internal raster and keep the OpenXR projection full-sized |
| Broad runtime/config cleanup at 42a1276 | Built and launched, then fatal error at the first level transition before weapon/palette logging | Never deploy 42a1276; clean one independently verified path per headset build |
| Filter the current-display HMD pose in Present (`a229dfb`) | Crosshair smoothing and gun axes passed, but head motion felt delayed/nauseating and 0% could not remove the stale-frame feel | Present is after Halo rasterization; sample the next predicted display pose for the upcoming game render |

## Known limitations

- Halo 3 only is validated. ODST is not, and is not a quick port: see the ODST
  scope section below for the measured signature/layout evidence.
- Full-body legs/torso are not implemented. Current VRIK is the first-person arms.
- Weapon coverage is not yet systematic. Re-test shotgun, assault rifle, and pistol from the restored baseline, then cover every weapon class.
- Scope rendering across all weapons, vehicles/turrets, cutscenes, co-op/split-screen, checkpoints across long sessions, and RTX 2070 Super performance need formal acceptance tests.
- Universal-scope Stage 1 is headset-confirmed on 2026-07-20 at commit `44d78a1`:
  the fixed blue-green 4:3 panel appeared correctly on every tested weapon.
  R3 toggled it independently of Halo's native zoom.
- Universal-scope Stage 2 is headset-confirmed on 2026-07-20 at commit `dde58af`:
  an existing valid eye render appeared small, flat, rectangular, and correctly
  oriented on the proven screen. As expected, its perspective felt head-mounted;
  Stage 3 therefore uses the right-hand weapon position as camera origin and the
  actual bullet ray as its forward axis.
- Universal-scope Stage 3 commit `0e0c773` displayed the right-hand aim-camera
  approach, but the headset result rejected its initial absolute zoom and
  reported excessive cost. The later crosshair-origin build at `15508ab`,
  restored by rollback `e1f608a`, is headset-confirmed on 2026-07-21: R3 shows
  and hides the gun-mounted image while the main stereo view remains intact.
  It is explicitly experimental and currently uses one configurable zoom for
  every weapon. The tester's active calibration is now the default: enabled,
  3.39x, 0.182 m wide, local right/up/forward offsets
  -0.081/0.207/0.222 m, refresh divisor 2.
- Per-weapon authored-tier attempt `6e1a4f8` failed in the headset because its
  required runtime signature group did not resolve. The input path still
  consumed R3, but weapon lookup always failed and no scope layer activated.
  It was fully reverted at `e1f608a`; repair the locator separately before
  attempting authored tiers again.
- HUD size: the chud_globals safe-frame lever and the automatic hud_size slider are headset-confirmed. A value of 0.38 visibly and correctly scaled the native HUD layout (2026-07-19).
- HUD aspect: commit `1b53139` was tested on Quest 3 and PSVR2 with OpenXR Toolkit disabled. The headset-derived anisotropic safe frame is a clear improvement on both, though still mildly squished.
- Resolution scaling is headset-confirmed at the 0.67 Low setting with Toolkit scaling disabled: the complete eye remains intact and fills the unchanged OpenXR projection. The launcher scales Halo's 2912x2100 internal raster evenly; the values other than Low still need headset coverage, and nothing above 1.10 has ever been run in a headset — the high end is an untested option, not a validated tier.
- `resolution_scale` is free-form as of 2026-07-20: any value from 0.35 to 2.00 is honored exactly. It previously snapped to six tiers in three separate places (`Clamp()`, the launcher's reader, and the F1 combo), so a hand-typed 0.90 silently became 0.80. `kResolutionScaleMin/Max` and `kNativeRenderWidth/Height` now live in `src/common/config.h` and are read by both the DLL and the launcher, which does not link `config.cpp`. F1 shows a free slider with the six tiers as shortcut buttons and the resulting pixel count. Desk-verified in `core_tests.cpp`; not yet headset-confirmed at a non-tier value.
- `halomccvr.cfg` is the documented manual-install settings path. It is created with defaults on first run and every setting is written with its description, default, and range; deleting the file regenerates it. F1 → Advanced has a two-click "Reset ALL settings to defaults". Hand-editing is safe by construction: unknown keys are logged and ignored, malformed floats keep the previous value, and every value is clamped on load. Note that `ConfigSave()` rewrites the whole file, so a hand-editor's own comments do not survive an F1 save.
- Install is manual as of 2026-07-20 (installer scripts removed): the tester creates `<MCC>\Halo_MCC_VR` and copies the two binaries in. Updating is copying the new files over the old ones, which leaves `halomccvr.cfg` alone. Uninstalling is deleting that one folder. A legacy `halo3xr` folder is renamed by hand; the DLL still imports a legacy `halo3xr.cfg` automatically via `ConfigLoadMigrating`.
- Local canonical-path smoke passed on 2026-07-20: the existing diagnostic-only folder was manually renamed intact, deploy.bat byte-verified both binaries under `Halo_MCC_VR`, the renamed launcher injected successfully, MCC remained running, and OpenXR reached focused state. Automatic migration on a tester install and visual confirmation inside a Halo 3 level remain pending.
- Built-in FSR is intentionally out of scope. The user prefers the simpler resolution scale and can use external tools separately; do not bundle FSR into this test.
- OpenXR Toolkit FSR produced a tiled/overlapping VR View in the observed configuration. The exact third-party incompatibility was not isolated; the supported mod path is the internal resolution preset with a full-size runtime swapchain/imageRect.
- H3EK/ManagedDonkey evidence replaces the runtime-ID theory: all native reticle collections are scripting class 2, while 0x62C / 0xF70 / 0x1A90 are runtime chud_definition tag indices and cannot be universal defaults.
- The earlier 0x2EDE38 hook alone did nothing because normal gameplay short-circuits before Halo's class-2 check. Commit c923842 validates and removes only that short-circuit, then uses the existing class-gated predicate. Headset testing confirmed the result across multiple weapons: native crosshairs are gone, the VR reticle and remaining HUD stay visible, and the prior black-screen failure is absent.
- The attempted scripting-class classifier inside the element-submit hook at halo3+0x2EDF24 caused a black headset view when stereo entered a level (2026-07-19 16:45 build). It was fully removed; do not restore its runtime tag-table dereferences.
- HUD performance regression resolved: remove the status/toast render path, keep HUD writes out of CamCopyHook, apply only on slider changes, and validate the three safe-frame pairs once per second. The user confirmed normal performance returned with the 0.38 HUD scale and remembered-id crosshair hider active (2026-07-19 15:39 build).
- Projectile direction is controller-aligned, but Halo still owns the actual
  fire origin. The user headset-confirmed the symptom on 2026-07-20: bullets
  appear to spawn from the wrong place but land perfectly at the VR crosshair.
  Do not claim a muzzle-origin hook exists.
- Dual wield is no longer awaiting its first headset result. Commit `c2e6a27`
  is headset-confirmed nearly perfect for the tested pairing and is the current
  best checkpoint. Broader per-weapon dual-wield coverage remains pending; do
  not generalize this result to every pairing until the weapon matrix passes.
- The native pause-state build is the current best-working checkpoint, but the
  focused Pause -> Resume -> Restart Level -> 3D acceptance sequence still
  needs an explicit recorded headset result before pause transitions are called
  complete.

## Pause, controls, and menu checkpoint (2026-07-19)

- Checkpoint commit: `8ea1c04` on `feature/menu-controls`.
- Pause implementation commit: `73f81f1`. Four alternating read-only snapshots
  and a 2 ms transition trace located the native engine state; production code
  resolves it by a unique signature rather than retaining the observed RVA.
- Controls/recenter commit: `a06ebd5`. F1 Status has separate F2 and F11
  controls and a live head-tracking/stereo/view readout. Every public recenter
  path resets Halo and OpenXR together.
- Deployed Release DLL SHA-256:
  `D7484C404F19A16979FB6A9F0789FB2C7C70AD36AC7ACDBEDD4BFADF96069AE9`.
  Deployed launcher SHA-256:
  `AABE5EBFFFBBA332C07A8DBA0B4B3C92ED8D2CE4714C3F355DE8158AF0B7928B`.
  Both installed files byte-matched the Release outputs.
- Generated process-memory snapshots were deleted after analysis and
  `pause_scan/` is ignored. Game/editing-kit binaries and memory dumps must not
  enter Git or release packages.
- Historical handoff: dual-wield work started from `8ea1c04` and remained
  isolated from the pause/menu implementation.

## Dual-wield checkpoint and next work (2026-07-20)

- Runtime checkpoint: `c2e6a27` on `feature/dual-wield`.
- Release/deployed DLL SHA-256:
  `4D7FE27DD501AD9110DF9905DB825C9CA545021431ED4BE910CBF46D76064E5A`.
  Build timestamp: 2026-07-20 00:12:19 local time. The installed DLL
  byte-matched the Release output before the headset test.
- Headset result: dual wield is almost perfect and this is the best checkpoint
  yet. The left hand follows the left controller; the secondary gun follows the
  solved hand rather than being independently locked to the controller.
- The accepted implementation applies the desired-left-wrist versus authored-
  left-wrist delta to the secondary gun/right-hand descendant subtree and uses
  the left wrist as the dual marker/muzzle ownership anchor.
- Slot 0 and the previously accepted two-handed support-hand/barrel-alignment
  path were not changed by this final ownership correction. Preserve them.
- Isolated task 1 DONE (headset-confirmed 2026-07-20): smooth-turn jitter fixed
  on branch `fix/smooth-turn-jitter`. Root cause was `ApplyVrTurn` timing its dt
  with `GetTickCount` (~15.6 ms resolution) while running several times per 90 Hz
  frame from CamCopyHook, so the yaw advanced in ~15 ms lumps on ~every third
  frame (a ~5 Hz stutter). Fixed by switching that dt to QueryPerformanceCounter;
  no other path changed. Deployed DLL build 2026-07-20 12:25 AM, SHA-256
  `f83e7ee03f193ab4fd64b591c7a4d5a9c0a35bbed20bb113b2d6eeae448f12ec`. User result:
  "flawless... smooth as butter."
- Isolated task 2 DONE (headset-confirmed 2026-07-20, "works beautifully"): the
  optional `floating_hands` mode (OFF by default) is on branch
  `feature/floating-hands` (based on the confirmed jitter fix). It is a pure
  presentation filter in `FpVisiblePaletteHook` applied AFTER the untouched VRIK
  solve: it collapses every bone not in `wristDescendants | lWristDescendants`
  to an invisible speck via the proven `BoneMatrix.scale` render input. F1 →
  Aim & Weapons → Body (VRIK) → "Floating hands (hide arms)". Deployed DLL build
  2026-07-20 12:35 AM, SHA-256
  `a9da286ddd983306010a812608aaefce41f912115bd5eb6b9cc7a8bfbfc61459`.
- SKELETON FACT (from the runtime log, FP model, `body_wip=0`): the first-person
  model is arms only — 0=root, 1/2=L/R shoulder, 3/4=L/R elbow, 5/6=L/R hand,
  7-36=finger bones, 37-43=weapon, 44=marker. There are NO pelvis/leg/torso
  bones. So `floating_hands` hides the ARMS only; the legs the user sees are a
  SEPARATE render (the player biped/third-person body, visible because the VR
  world-camera can look down at it), which this filter does not touch.
- Leg/hip hiding: CLOSED by user decision 2026-07-20 ("we can keep the legs
  its ok"). Do not build a leg-hider unless the user reopens it. Durable finding
  from the investigation, kept so nobody re-hunts blind:
  - The visible legs are Halo 3's `fp_body.render_model` — a COMPLETE first-
    person body (H3EK nodes: fp_body, legs, pelvis, l/r_thigh, spine, l/r_calf,
    spine1, l/r_foot, l/r_clavicle, neck, l/r_toe, l/r_upperarm, head,
    l/r_forearm, l/r_hand + 30 finger bones). Its regions are only default/base,
    so there is NO per-limb region cull; the whole body is drawn and only the
    legs fall in view (upper body rests at the sides, near-clipped).
  - `fp_body` does NOT flow through `FpVisiblePaletteHook`. Proof: the live
    "M3 VRIK PALETTE" log shows only the 43-bone weapon skeleton
    (`C3A348592F880D7F`, tags 0x3503/0x1157) and no ~53-bone body submission and
    no count=0 (non-weapon) submission. So `floating_hands` (which filters that
    hook) cannot reach the legs; a leg-hider would need a different boundary.
  - Candidate levers from the earlier VRIK live scan (untested for this purpose,
    RVAs are per-session and must be re-located by signature): the engine debug
    var `debug_first_person_models` (value slot RVA 0x7CE8F0), `render_first_person`
    (~0x7CBFA0), `director_disable_first_person` (~0x7D1B88), and the `legs_shared`
    render path (string at 0x8011F8). `body_wip` already flips
    director/render_first_person switches for the opposite (show-more-body) goal.

## ODST port scope (measured offline 2026-07-20)

No code was changed for this survey. All 20 production AOB signatures were
parsed out of `src/dll/game.cpp` and scanned against `halo3.dll` and
`halo3odst.dll` on disk. Findings are static-binary facts, not theories.

- 8 of 20 signatures match ODST uniquely and byte-for-byte: `kPrepareViewSig`,
  `kBuildViewportSig`, `kBuildMatricesSig`, `kComposeBonesSig`,
  `kComposeSpecialBonesSig`, `kFpVisiblePaletteSig`, `kHudXformSig`,
  `kFpRootCallSig`.
- 12 fail, and they cover the load-bearing stereo path: `kCamCopySig`,
  `kRenderViewSig`, `kFpCameraRebuildSig`, `kFpCameraUploadSig`,
  `kFpDriverSig`, `kFpDriverGuardSig`, `kFpInterpolateSig`, `kGunCamRefSig`,
  `kNativePauseOwnerSig`, `kHudElemSig`, `kSwayCallSig`,
  `kFpNativeWeaponIkDecisionSig`.
- The failures are recompiled functions, not missing ones. Longest-prefix
  analysis: `kFpNativeWeaponIkDecisionSig` matches 22 of 29 bytes at a single
  ODST site and breaks on one register byte (`41 0F 28 D8` vs `41 0F 28 D9`);
  `kCamCopySig` matches 24 of 34 at a single site and breaks where a short `je`
  became a near `je`, so that function grew.
- Root cause is shifted structure layouts. Confirmed concretely: the
  gun/overlay camera-object array stride is `0x2820` in Halo 3 and `0x2810` in
  ODST, so that camera object is 16 bytes smaller. The sway and FP-driver call
  sites also reference different field offsets. `game.cpp` currently contains
  53 distinct hardcoded struct offsets.

Consequences for planning:

- Repairing the 12 signatures is the cheap half (wildcard the volatile operand
  bytes, then re-verify uniqueness in both DLLs). Re-deriving the camera, view,
  and first-person struct layouts on the ODST binary and re-validating VRIK and
  the palette path in the headset is the expensive half. It is the same class of
  work that produced the Halo 3 build.
- Do not reuse any Halo 3 struct offset in ODST without confirming it on the
  ODST binary. The 0x2820/0x2810 difference proves the layouts diverge.
- The first real ODST step is confirming the eight matching signatures land in
  the same functions, then a live scan for the shifted camera struct. It is not
  a code change.

Shipping safety today: ODST is registered in `src/common/title_registry.cpp`
with `runtimeSupported=false`, and hook installation in `game.cpp` is gated on
`GameTitle::Halo3`. ODST therefore loads stock and untouched, so the Halo 3
alpha is safe to distribute on machines that also have ODST installed.

## 2026-07-19 session closeout

- Confirmed HUD checkpoint: `65113ab` on the history behind `fix/left-hand-wrist-offset`.
- Confirmed left-hand test build: Release/deployed SHA-256 `70E3CC78DCA878FEEC218ED2124C9B55896D53421C7A0DE7049C87D653EFBFB1`.
- Left support-hand visual alignment and two-handed aiming remained aligned with the shared 0.12 m forward correction. User result: "fantastic it worked."
- Continue from the tip of `fix/left-hand-wrist-offset`; do not reintroduce the HUD toast/status path or per-camera-copy HUD verification.

## HUD, crosshair, and resolution checkpoint (2026-07-19)

- HUD aspect: commit 1b53139 derives an anisotropic safe-frame correction from the runtime eye FOV instead of assuming the PSVR2-derived 2912x2100 aspect. Quest 3 and PSVR2 testing with OpenXR Toolkit disabled showed a substantial improvement; a mild squeeze remains accepted.
- Unsafe reticle attempt: f0d5a88 classified widgets by dereferencing runtime tag-table data in the submit hook and caused a black headset view on level entry. 7fdf019 reverted only that classifier and preserved the working HUD-aspect change. Deployed rollback DLL SHA-256: 1F7CB7822D51521C62A24969FE463B05C719A1AE4515A02A49011A42CF621488.
- Picker evidence: f462f17 restored the safe F1 observer, and a06d48c briefly defaulted the observed 0x62C value. Further observations 0xF70 and 0x1A90 proved these values varied by loaded chud_definition and were not crosshair IDs. The picker/default approach is retired.
- Production crosshair fix: H3EK's 65 ui/chud definitions and ManagedDonkey/disassembly evidence identified scripting class 2 as the stable native-crosshair semantic. Commit c923842 uses that engine gate; the user confirmed multiple weapons had no native crosshair while the VR reticle and HUD remained intact. Test DLL SHA-256: BC408423BADB187C83AAB44A16F926D77CF91BB4C7C00464724FC3B9F5EB1F60.
- Resolution architecture: d3af87d added a restart-applied internal render scale. Halo's 2912x2100 raster is scaled evenly and rounded to even dimensions, while the OpenXR stereo-array swapchain and submitted imageRect remain at the runtime-recommended full size. This preserves the complete image and lets the existing normalized shader blit upscale it.
- Preset checkpoint: 1fc56c8 exposes Potato 50% (1456x1050), Low 67% (1952x1408), Medium 80% (2330x1680), High 100% (2912x2100), and Ultra 110% (3204x2310). Arbitrary/legacy values normalize to the nearest tier. F1 states that a full game restart/relaunch is required. Low is headset-confirmed; the other four tiers remain unverified.
- Deployed 1fc56c8 hashes: DLL FDDF3364462B187BE17D4234541965631CA4BC02C93A27BE2596C76D678EDDD5; launcher D3317325C7158B6D897A58ACD3BF8CCF30A30D9BFDF7D9EB4B37A4B3E40FA7C4. Both installed files byte-matched the Release outputs.
- Product decision: no FOV slider and no built-in FSR. Resolution presets are the supported performance control; third-party upscalers remain external and unsupported.

## Alpha distribution (2026-07-19)

- RELEASE-BLOCKING SAFETY INCIDENT (2026-07-20): the original alpha
  uninstall.bat treated any directory containing halo3xr.dll as the mod folder
  and executed recursive rd after a single confirmation. A tester who placed
  the package in the MCC root reported that confirming uninstall removed the
  whole game installation. The public v0.1-alpha ZIP asset was immediately
  removed after 13 recorded downloads, and local copies were quarantined.
  Recovery is Steam Verify integrity of game files; saves are not intentionally
  targeted by the script. The replacement validates an exact halo3xr leaf under
  a real MCC parent, revalidates immediately before deletion, removes only an
  explicit mod-owned file allowlist, performs no recursive deletion, and leaves
  unknown files/directories untouched.

- RESOLVED BY REMOVAL (2026-07-20): install.bat, uninstall.bat, and their
  fake-tree tests (tests/uninstall_safety_test.ps1,
  tests/install_migration_test.ps1) were deleted from the project by user
  directive — "forget bat files its gonna be manual this is a waste of
  resources". Distribution is manual only: copy halo3xr.dll and
  halo3xr_launcher.exe into `<MCC>\Halo_MCC_VR`, delete that folder to
  uninstall. No script can delete a tester's game folder because there is no
  script. Setup lives in installer/MANUAL-README.txt and ALPHA-README.txt, and
  settings in the self-documenting halomccvr.cfg. Do not reintroduce installer
  scripts unless the user asks.
  Unresolved at the time of removal: the working-tree install.bat exited 255
  silently right after "Found the game at:" on a fake tree, with CRLF line
  endings ruled out. Any older ZIP still containing it is suspect.

- Release candidate exported 2026-07-20 02:17 to `dist\HaloMCCVR-alpha-0.1.zip` from clean commit `7fe358f`. DLL SHA-256 `E4F70A6C...2697FA`, launcher SHA-256 `D3C12C4E...C166042` (full hashes in the package's BUILD-INFO.txt). This binary is a fresh rebuild and has not itself been launched in a headset; the laptop test predates it.
- Product renamed Halo 3 VR -> Halo MCC VR on 2026-07-20 by user decision: the mod targets MCC and Halo 3 is only the first working title. The rename touched display strings only. OpenXR `actionSetName` stays `"gameplay"` (bindings unaffected); only `localizedActionSetName` changed. The desktop shortcut is now `Halo MCC VR.lnk`; a tester who made a `Halo 3 VR.lnk` under the old name deletes it by hand. Verified before shipping: both binaries contain only the new name (DLL ASCII x3, launcher UTF-16).
- Published to https://github.com/pancreations/Halo-MCC-VR on 2026-07-20 under MIT, with an explicit statement in LICENSE/README/release notes/ALPHA-README that the code was written by Claude and Codex under human direction and that no human reviewed every line. Only `master` is pushed; feature/fix/probe/recovery branches stay local.
- Prior alpha package exported 2026-07-19 18:10 to `dist\Halo3VR-alpha.zip` from commit `1fc56c8` (dirty tree). DLL SHA-256 `20B3CB0C...DA2D04`, launcher SHA-256 `F7B50B3F...EBCC7F`.
- `export-alpha.bat` is the required distribution workflow. It selects the exact CMake recorded in the configured build tree, removes only the two expected Release outputs, rebuilds them, assembles `dist\HaloMCCVR-alpha-0.1`, byte-compares both packaged binaries to the fresh outputs, writes build identity/SHA-256 values, and creates `dist\HaloMCCVR-alpha-0.1.zip`. A failed or stale build must not produce a testable package.
- The dist files were audited for personal information before sharing: no user paths, names, or email in any file; the binaries carry no PDB/debug record and empty version resources; the ZIP byte-matches the exported folder.
- First separate-machine clean install and launch passed on an RTX 4060 laptop GPU. A perceived 60 FPS cap was attributed to MCC's own V-Sync/framerate limit on the 60Hz laptop panel (the tester had not checked the in-game fps lock); the mod adds no cap and passes the game's sync interval through unchanged.
- Quest streaming: testers report the best performance with Steam Link rather than Virtual Desktop (2026-07-20). Point Quest stutter/latency reports at the streaming path before touching mod settings. Documented in installer/ALPHA-README.txt.
- Left-hand size was never adjustable until 2026-07-20: the palette size trim switched its anchor to the left wrist but kept the RIGHT wrist's bone mask, so no left-hand bone was ever in the scaled set. Each side now trims its own subtree (`left_hand_scale`, default 1.00 = authored), with the left mask excluding any bone the right pass already scaled. Applied in both the arm-IK path and the rigid fallback (the fallback scales the whole assembly, so only the remaining ratio is applied there). Headset-confirmed 2026-07-20 alongside the new defaults gun_scale 0.97 and left_hand_forward_m -0.093.
- Decision: the mod must NOT auto-force vsync off or override MCC video settings. Testers set V-Sync/framerate limit themselves; point future "capped at 60" reports to MCC Settings → Video.
- Required MCC settings, headset-established on the test laptop 2026-07-20: Max Frame Rate 120, V-Sync Off, and Halo 3 in-game Field of View 120. The FOV value is a rendering requirement, not a comfort preference: at the default FOV the engine culls geometry outside the flat-screen frustum and the tester sees scenery pop in and out at the edges of the headset view. FOV 120 pushes culling beyond the headset's field of view. This is the in-game FOV setting only; the mod's own VR projection is still driven by the runtime and there is no mod-side FOV slider. Also headset-observed: MCC's own FSR option must stay off because it breaks the VR image scale, and MCC video settings can be changed with the headset on from inside the VR session without disturbing the mod (no flat-screen launch required). Documented in README.md and installer/ALPHA-README.txt.
- Still pending: manual update-over-old-copy behavior on a tester machine, and broader external hardware/runtime coverage.

## Required test/deploy sequence

    git status --short
    cmake --build build --config Release
    .\deploy.bat auto

Then verify both DLL and launcher build times/hashes, launch without anti-cheat, and test only the behavior named for that build. Record the result before the next change.
