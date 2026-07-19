# Current state

Authoritative as of 2026-07-19. If another note conflicts with this file, this file wins. Historical experiments remain available in Git history; they are not implementation instructions.

## Recovery points

- Headset-proven baseline: 330a568
- Protected branch: recovery/best-working-20260719-1300
- Cleanup branch: cleanup/production-baseline-20260719
- Pre-recovery master: ad61f8d

Do not rewrite or delete the recovery branch. Start new experiments from a named branch or commit.

## Headset-confirmed behavior

- Halo 3 level load, OpenXR session, true stereo, head rotation, and positional tracking work.
- PSVR2 Sense controls are merged through XInput and work in gameplay and menus.
- The right controller drives the weapon and aim. The floating VR reticle follows the actual aim ray.
- First-person arm IK bends the right and left arms from stable shoulder anchors.
- The shotgun support arm is free and follows the left controller. The user confirmed this after the native weapon-IK bypass.
- The assault rifle and pistol were the known-good comparison behavior before the shotgun fix.
- The game's native HUD renders in both eyes. The centered game reticle can be hidden by its observed HUD element id.
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

No diagnostic mode is part of the saved configuration. Old keys are accepted and ignored so existing config files continue to load.

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

## Known limitations

- Halo 3 only is validated; ODST is not.
- Full-body legs/torso are not implemented. Current VRIK is the first-person arms.
- Weapon coverage is not yet systematic. Re-test shotgun, assault rifle, and pistol after this cleanup, then cover every weapon class.
- Scope rendering, vehicles/turrets, cutscenes, co-op/split-screen, checkpoints across long sessions, and RTX 2070 Super performance need formal acceptance tests.
- The native HUD remains full-size. The centered game reticle requires selecting the correct observed element id.
- Projectile direction is controller-aligned, but Halo still owns the actual fire origin; do not claim a muzzle-origin hook exists.

## Required test/deploy sequence

    git status --short
    cmake --build build --config Release
    .\deploy.bat auto

Then verify the DLL build time/hash, launch without anti-cheat, and test only the behavior named for that build. Record the result before the next change.