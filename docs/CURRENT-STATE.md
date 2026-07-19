# Current state

Authoritative as of 2026-07-19. If another note conflicts with this file, this file wins. Historical experiments remain available in Git history; they are not implementation instructions.

## Recovery points

- Headset-proven baseline: 330a568
- Protected branch: recovery/best-working-20260719-1300
- Safe documentation-cleanup branch: cleanup/production-baseline-20260719; runtime restored by ddfe109
- Failed broad runtime-cleanup commit: 42a1276 (preserved for diagnosis; never deploy)
- Pre-recovery master: ad61f8d
- Lean HUD scaling and reticle-hider checkpoint: 65113ab

Do not rewrite or delete the recovery branch. Start new experiments from a named branch or commit.

## Headset-confirmed behavior

- Halo 3 level load, OpenXR session, true stereo, head rotation, and positional tracking work.
- PSVR2 Sense controls are merged through XInput and work in gameplay and menus.
- The right controller drives the weapon and aim. The floating VR reticle follows the actual aim ray.
- First-person arm IK bends the right and left arms from stable shoulder anchors.
- The shotgun support arm is free and follows the left controller. The user confirmed this after the native weapon-IK bypass.
- The assault rifle and pistol were the known-good comparison behavior before the shotgun fix.
- The game's native HUD renders in both eyes. The centered game reticle can be hidden by its observed HUD element id.
- Native HUD scaling at 0.38, centered-reticle hiding, and normal pre-regression GPU performance are headset-confirmed.
- Runtime-FOV HUD aspect correction is headset-confirmed on both Quest 3 and PSVR2 with OpenXR Toolkit disabled. It substantially improves the layout; a mild overall squeeze remains and is accepted for now.
- The left support-hand wrist-to-palm correction is headset-confirmed. `left_hand_forward_m` defaults to 0.12 m and drives both the rendered left-hand IK target and the two-handed aiming point; the F1 slider tunes them together.
- Halo motion blur is off by default because its previous-camera state creates stereo echo trails.
- deploy.bat auto prevents stale-DLL testing by refusing to deploy while MCC is open and comparing the built/deployed files byte-for-byte.

## Production implementation

The active path is deliberately small:

1. RenderViewHook renders two eye passes and supplies each eye's camera/projection.
2. FpDriverHook and FpCameraRebuildHook stamp the active eye camera into Halo's first-person draw path.
3. The interpolation and visible-palette hooks preserve the authored skeleton, solve both arms once per stereo pair, and provide the render palette.
4. The marker/muzzle path receives the same controller transform as the visible weapon.
5. Halo's normal input/aim path steers projectile direction through the VR reticle point.
6. The CHUD element-submit hook hides only the configured centered reticle.
7. At startup, a unique signature changes the stock first-person weapon-IK decision from 74 05 to EB 18. This selects Halo's own no-weapon-IK branch and prevents shotgun-specific authored pump grip IK from overriding the left controller arm.

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
| Broad runtime/config cleanup at 42a1276 | Built and launched, then fatal error at the first level transition before weapon/palette logging | Never deploy 42a1276; clean one independently verified path per headset build |

## Known limitations

- Halo 3 only is validated; ODST is not.
- Full-body legs/torso are not implemented. Current VRIK is the first-person arms.
- Weapon coverage is not yet systematic. Re-test shotgun, assault rifle, and pistol from the restored baseline, then cover every weapon class.
- Scope rendering, vehicles/turrets, cutscenes, co-op/split-screen, checkpoints across long sessions, and RTX 2070 Super performance need formal acceptance tests.
- HUD size: the chud_globals safe-frame lever and the automatic hud_size slider are headset-confirmed. A value of 0.38 visibly and correctly scaled the native HUD layout (2026-07-19).
- HUD aspect: commit `1b53139` was tested on Quest 3 and PSVR2 with OpenXR Toolkit disabled. The headset-derived anisotropic safe frame is a clear improvement on both, though still mildly squished.
- The centered game reticle still depends on the remembered observed element id. The current config retains the headset-picked id 4425; a multi-id hide list remains the next fix if weapon variance persists.
- The attempted automatic type-2 visibility hook at halo3+0x2EDE38 installed but did not hide the visible weapon crosshair in-headset; do not restore it.
- HUD performance regression resolved: remove the status/toast render path, keep HUD writes out of CamCopyHook, apply only on slider changes, and validate the three safe-frame pairs once per second. The user confirmed normal performance returned with the 0.38 HUD scale and remembered-id crosshair hider active (2026-07-19 15:39 build).
- Projectile direction is controller-aligned, but Halo still owns the actual fire origin; do not claim a muzzle-origin hook exists.

## 2026-07-19 session closeout

- Confirmed HUD checkpoint: `65113ab` on the history behind `fix/left-hand-wrist-offset`.
- Confirmed left-hand test build: Release/deployed SHA-256 `70E3CC78DCA878FEEC218ED2124C9B55896D53421C7A0DE7049C87D653EFBFB1`.
- Left support-hand visual alignment and two-handed aiming remained aligned with the shared 0.12 m forward correction. User result: "fantastic it worked."
- Continue from the tip of `fix/left-hand-wrist-offset`; do not reintroduce the HUD toast/status path or per-camera-copy HUD verification.

## Required test/deploy sequence

    git status --short
    cmake --build build --config Release
    .\deploy.bat auto

Then verify the DLL build time/hash, launch without anti-cheat, and test only the behavior named for that build. Record the result before the next change.
