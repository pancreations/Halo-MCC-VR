# Stage 3M — all-title gun-stock calibration

Status: source-complete, not headset-accepted. This pass adds visual gun-stock
translation controls to every currently supported VR gameplay title: Halo 2
(Classic and Anniversary share the H2 carrier), Halo 3, Halo 3: ODST, Halo:
Reach, and Halo 4.

## User-facing controls

The F1 **Weapon calibration** area now contains:

- `Gun right offset (m)` — range -0.30 to +0.30 m, default 0.00.
- `Gun up offset (m)` — range -0.30 to +0.30 m, default 0.00.
- A `Center` button beside each control.

Both values persist as `gun_right_m` and `gun_up_m` in `halomccvr.cfg`.

## Semantics

This follows marcusau2's pull request #1 design: translation is applied on the
visible weapon's **post-mount-rotation controller basis**. It is visual stock
calibration, not aim calibration. The controller/bullet/crosshair ray is not
translated by these values, and the left/support hand does not receive them.

- Halo 3 + ODST: exact shared `ControllerWorldPoseEx` right-weapon carrier.
- Reach: exact `ReachBuildPreparedControllerTarget` right-weapon carrier.
- Halo 2: the right visible first-person carrier receives right/up trim after
  the aim quaternion has supplied the mount rotation. Compatibility overloads
  keep all existing zero-right/up callers unchanged; the left carrier still
  receives only `left_hand_forward_m`.
- Halo 4: the final right gun carrier receives right/up translation after
  `Halo4BuildFloatingControllerCarrier` resolves the weapon mount basis. The
  left/support carrier remains untouched.

## Baseline preservation

Stage 3L was a post-link H4 rollback, while its source tree still contained the
older Stage 3H native-position source. This handoff reverses those Stage 3H H4
source changes back to the Stage 3G/C-H4-50 fail-closed/procedural source state
so a normal source rebuild does not resurrect rejected H4 reticle behavior.
This is source/runtime normalization, not a new H4 feature.

No Stage 3K muzzle reroot is included. No new H4 HUD, muzzle, reticle-capture,
or native-reticle experiment is included. Halo 4 remains parked at the safe
procedural-reticle behavior plus this requested gun-stock calibration.

Halo 2's accepted stereo/native-HUD/native-crosshair implementation is not
changed by this feature. In particular, this is not an attempted fix for the
separate Halo 2 Classic authored-gun alignment issue.

## Validation performed here

- Portable Halo 2 carrier math test: PASS. Forward/right/up visual trims move
  on the expected post-rotation basis, and the compatibility wrappers reproduce
  the previous zero-right/up result.
- Static source coverage audit: PASS for config clamp/load/save, F1 controls,
  H3/ODST, Reach, H2 and H4 right-weapon paths, left-hand exclusion, and H4
  Stage-3L source normalization.
- Unrelated file/line-ending churn was removed from the feature patch.

A full Windows/MSVC DLL build was not possible in this Linux container because
it does not contain the Windows SDK/D3D libraries or FXC used by this project.
Do not represent this source-only pass as headset-accepted until a Windows build
and headset smoke test have been run.
