# C-H4-54 Halo 4 native-reticle correction

Status: **rejected by headset test on 2026-09-01; behavior disabled**.

The Steam / SteamVR OpenXR 2.17.7 / Oculus 120 Hz result for source
`18111adf27fbfccd508baec744262bb9387eb236` showed that the native authored
reticle was still not restored and that the helmet frame remained absent with
an inert toggle. The two premises below therefore remain historical leads, not
accepted runtime facts. The transform-X helmet discriminator and the replay
HUD exclusion were disabled before the next candidate was started.

This candidate corrects the two connected CUI regressions reported on top of
C-H4-53. The user's headset result confirmed the C-H4-53 pause/resume, local
first-person effect suppression, and adjustable HUD behavior; those paths are
carried forward unchanged. The native weapon reticle and authored helmet frame
still require headset validation. This candidate does not advance
`docs/CURRENT-STATE.md`.

## Halo 3 behavior being matched

Halo 3 captures the game's authored weapon-reticle pixels without applying the
visible gameplay HUD placement a second time. The native art is then presented
on the existing VR reticle quad, while the ordinary gameplay HUD remains under
the configured size, aspect, curvature, and vertical-offset controls.

## Runtime evidence and correction

The Steam / SteamVR OpenXR 2.17.7 / Oculus 120 Hz C-H4-53 log supplied at
13:28 showed that the authored-reticle hook and private replay remained live,
but every capture was blank. At 13:29:28 it reported 69 authored captures and
204 exact capture reroutes, while the upload report stayed at zero uploaded,
zero art pixels, and 76 blank-held frames. The captured native base had changed
to approximately `-763.818/336.072`.

The earlier log supplied at 12:06 showed the same authored-reticle path
successfully uploading Halo 4 art before Stage 3X HUD restoration. It reported
6 uploaded frames with 830 art pixels, followed by repeated nonblank uploads;
its native base was approximately `-1456.000/818.772`.

C-H4-54 isolates the private authored-reticle capture replay from the Stage 3X
native HUD transforms:

- the CUI-root affine hook explicitly rejects `captureReplay`;
- the curvature bridge receives no gameplay-thread ownership during the replay;
- the ordinary visible gameplay pass immediately regains both adjustable
  affine and curvature behavior;
- pause/shell CUI remains stock as before;
- capture failure remains feature-local and retains the existing procedural
  fallback without disarming camera, stereo, hands, effects, or OpenXR.

The second Steam / SteamVR OpenXR 2.17.7 / Oculus 120 Hz log supplied at 13:35
proves why the helmet option did nothing: every status sample said
`helmet=stock-fallback` even while `config-visible` and `config-hidden`
changed. The attempted `helmet_armor` lookup was not a valid HUD binding.

The same log also reports exactly three type-`0x28` begin markers for every
gameplay CUI pass (for example 417 markers for 139 passes), and C-H4-53 hid all
417 as if every marker were the reticle. Existing H4EK evidence proves that
`ReticuleOffsetContainerWidget` emits the `0x0C`-byte payload
`{int32 transform_id; float x; float y}` with reticle X exactly `+/-0.0f`.
C-H4-54 now uses that native discriminator in both the visible and capture
passes:

- only the zero-X transform is hidden as the native face-centred reticle and
  selected for the authored reticle capture;
- the other two authored overlay transforms remain completely stock when
  `halo4_helmet=1`, which is the default;
- when `halo4_helmet=0`, only those two non-reticle gameplay transforms move
  offscreen; re-enabling the option immediately returns them to stock;
- malformed/unreadable payloads and unavailable CUI hooks leave the authored
  helmet and reticle stock for that feature only.

No address, reticle marker, pause path, effect path, HUD control, camera core,
or shared compositor code changed.

## Static verification

- `halomccvr_core_tests` pins the HUD-transform admission contract: visible
  gameplay is admitted, while capture replay, pause presentation, and
  non-gameplay calls are rejected.
- It also pins the H4EK zero-X reticle identity, default-visible helmet policy,
  off-only overlay suppression, malformed-input fallback, and ownership gates.
- The package source gate requires the replay curvature-thread exclusion and
  both pure CUI classifiers.
- The normal candidate pipeline builds, tests, runs the Reach consistency
  check, verifies the manifest, and installs the exact package into every
  detected MCC edition without changing an existing config.

## Headset test

On the log-identified edition/runtime/headset:

1. Enter Halo 4 gameplay with an authored weapon reticle. Confirm the game's
   native weapon-specific reticle is present, not the temporary procedural dot,
   and the helmet/visor frame is visible immediately with the default option.
2. Switch weapons or zoom where practical and confirm the native reticle art
   changes normally and remains aligned with controller aim.
3. Turn **Show Halo 4 helmet frame** off: only the helmet/visor shading should
   disappear. Turn it back on: the authored frame must return immediately while
   the weapon reticle and rest of the HUD remain present.
4. Change HUD size, aspect, curvature, and height and confirm the visible HUD
   still responds while both authored elements remain valid.
5. Pause and resume twice, then fire a conventional and a Promethean weapon to
   confirm the already-working C-H4-53 behavior remains intact.
6. Provide the resulting log. Acceptance requires the user's explicit headset
   result plus its MCC edition, OpenXR runtime, and headset identity.
