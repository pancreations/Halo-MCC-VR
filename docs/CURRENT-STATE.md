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

Do not rewrite or delete the recovery branch. Start new experiments from a named branch or commit.

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

## Known limitations

- Halo 3 only is validated. ODST is not, and is not a quick port: see the ODST
  scope section below for the measured signature/layout evidence.
- Full-body legs/torso are not implemented. Current VRIK is the first-person arms.
- Weapon coverage is not yet systematic. Re-test shotgun, assault rifle, and pistol from the restored baseline, then cover every weapon class.
- Scope rendering, vehicles/turrets, cutscenes, co-op/split-screen, checkpoints across long sessions, and RTX 2070 Super performance need formal acceptance tests.
- HUD size: the chud_globals safe-frame lever and the automatic hud_size slider are headset-confirmed. A value of 0.38 visibly and correctly scaled the native HUD layout (2026-07-19).
- HUD aspect: commit `1b53139` was tested on Quest 3 and PSVR2 with OpenXR Toolkit disabled. The headset-derived anisotropic safe frame is a clear improvement on both, though still mildly squished.
- Resolution scaling is headset-confirmed at the 0.67 Low setting with Toolkit scaling disabled: the complete eye remains intact and fills the unchanged OpenXR projection. F1 now exposes five restart-applied presets: Potato 50%, Low 67%, Medium 80%, High 100%, and Ultra 110%. The launcher scales Halo's 2912x2100 internal raster evenly; the tiers other than Low still need headset coverage.
- Built-in FSR is intentionally out of scope. The user prefers the simpler resolution scale and can use external tools separately; do not bundle FSR into this test.
- OpenXR Toolkit FSR produced a tiled/overlapping VR View in the observed configuration. The exact third-party incompatibility was not isolated; the supported mod path is the internal resolution preset with a full-size runtime swapchain/imageRect.
- H3EK/ManagedDonkey evidence replaces the runtime-ID theory: all native reticle collections are scripting class 2, while 0x62C / 0xF70 / 0x1A90 are runtime chud_definition tag indices and cannot be universal defaults.
- The earlier 0x2EDE38 hook alone did nothing because normal gameplay short-circuits before Halo's class-2 check. Commit c923842 validates and removes only that short-circuit, then uses the existing class-gated predicate. Headset testing confirmed the result across multiple weapons: native crosshairs are gone, the VR reticle and remaining HUD stay visible, and the prior black-screen failure is absent.
- The attempted scripting-class classifier inside the element-submit hook at halo3+0x2EDF24 caused a black headset view when stereo entered a level (2026-07-19 16:45 build). It was fully removed; do not restore its runtime tag-table dereferences.
- HUD performance regression resolved: remove the status/toast render path, keep HUD writes out of CamCopyHook, apply only on slider changes, and validate the three safe-frame pairs once per second. The user confirmed normal performance returned with the 0.38 HUD scale and remembered-id crosshair hider active (2026-07-19 15:39 build).
- Projectile direction is controller-aligned, but Halo still owns the actual fire origin; do not claim a muzzle-origin hook exists.
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

- Alpha package exported 2026-07-19 18:10 to `dist\Halo3VR-alpha.zip` from commit `1fc56c8` (dirty tree). DLL SHA-256 `20B3CB0C...DA2D04`, launcher SHA-256 `F7B50B3F...EBCC7F` (full hashes in the package's BUILD-INFO.txt).
- `export-alpha.bat` is the required distribution workflow. It selects the exact CMake recorded in the configured build tree, removes only the two expected Release outputs, rebuilds them, assembles `dist\Halo3VR-alpha`, byte-compares both packaged binaries to the fresh outputs, writes build identity/SHA-256 values, and creates `dist\Halo3VR-alpha.zip`. A failed or stale build must not produce a testable package.
- The dist files were audited for personal information before sharing: no user paths, names, or email in any file; the binaries carry no PDB/debug record and empty version resources; the ZIP byte-matches the exported folder.
- First separate-machine clean install and launch passed on an RTX 4060 laptop GPU. A perceived 60 FPS cap was attributed to MCC's own V-Sync/framerate limit on the 60Hz laptop panel (the tester had not checked the in-game fps lock); the mod adds no cap and passes the game's sync interval through unchanged.
- Decision: the mod must NOT auto-force vsync off or override MCC video settings. Testers set V-Sync/framerate limit themselves; point future "capped at 60" reports to MCC Settings → Video.
- Still pending: reinstall/update behavior, uninstall, non-default Steam-library discovery, and broader external hardware/runtime coverage.

## Required test/deploy sequence

    git status --short
    cmake --build build --config Release
    .\deploy.bat auto

Then verify both DLL and launcher build times/hashes, launch without anti-cheat, and test only the behavior named for that build. Record the result before the next change.
