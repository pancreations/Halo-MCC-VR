# Stage 3X — Halo 4 full compatibility restoration

Status: **headset test candidate** built directly from the Stage 3V DLL that the user confirmed restores Halo 4 gameplay. Stage 3W is rejected and is not the runtime base.

## Restored Halo 4 stack

1. **C-H4-50 procedural bullet-ray reticle remains the only H4 reticle presentation.** The exact fail-closed branch is not changed.
2. **Recovered H4-specific pause exit.** A raw B edge while the H4 head-locked pause target is active requests stereo before H4 leaves its pause screen. A short resume grace prevents the lifecycle worker from racing the first resumed camera callback.
3. **Real-camera lifecycle.** Ownership heartbeat comes only from the admitted H4 camera SetupDetour, not the MCC Present loop. An installed core with no real H4 camera heartbeat is routed through the existing normal teardown.
4. **Complete H4 teardown.** The Stage 3V HMODULE pin remains until MinHook teardown is complete and the direct effect/HUD/curvature splices have been restored to exact retail bytes. A mismatch fails closed and retains the loader reference.
5. **Recovered H4 HUD.** The working V6 affine root (`halo4.dll+0x3F313C`) is ported to the current binary. `hud_size`, `hud_aspect`, and `hud_vertical_offset` affect the gameplay CUI root. The old V6 TLS gameplay-CUI admission is mapped to the current TLS callback-depth field at `+0x38C`. Native H4 curvature is restored through the proven `prop_curvature_theta` consumer at `halo4.dll+0x420D7E`.
6. **Muzzle / Promethean first-person effect suppression.** The existing guarded effect splice remains the boundary. Stage 3X admits all authored locations only when the descriptor is on the local first-person attachment path, rejects third-person-only camera mode, and finite-translates the selected effect matrix out of play space. This is suppression, not another reroot experiment.

## Frozen/protected titles

Stage 3X deliberately does not redesign Halo 2, Halo 3, ODST, or Reach. All H2 Classic alignment/gun-stock work, H2/H2A stereo/HUD/crosshair behavior, H3/ODST behavior, and the accepted Stage 3U Reach shell/re-entry/HUD behavior are inherited from Stage 3V without changes outside the audited H4 sites.

## Reproduction

The exact shipped DLL is deterministically reproduced from `built/Stage3V-HaloMCCVR.dll` with:

```text
python tools/build_stage3x_h4_full_restore.py built/Stage3V-HaloMCCVR.dll HaloMCCVR.dll
```

The builder refuses any input other than the exact Stage 3V SHA-256 and guards every patched instruction sequence before mutation.
