# Stage 3H — Halo 2 sliders and Halo 4 native reticle

Status: **unaccepted headset test candidate**. The deterministic post-link
build and focused portable logic checks pass. No headset acceptance is claimed.

## Included changes

- Halo 2 keeps Stage 3G's working native crosshair capture and now applies the
  live `hud_size`, `hud_aspect`, and `hud_vertical_offset` values to the proven
  gameplay-HUD shader draws. Positive vertical offset raises the HUD. The
  crosshair draw remains outside this HUD transform.
- Halo 4 keeps its weapon-specific authored CUI reticle art, animation, spread,
  hit marker, and red/green state. Each eye moves the native transform to the
  finite controller/bullet-ray target using the exact live gameplay viewport
  and that eye's camera/FOV. The procedural marker remains the fallback until
  both native eye writes prove the current render serial.
- Halo 4 broad whole-CUI replay is removed. Gameplay CUI executes once through
  the normal pass.
- Halo 4 locally suppresses only the first-person effect branch matching
  authored location 0, local effect, camera mode 1, and a nonnegative
  first-person attachment/designator. World, enemy, explosion, and environment
  effects keep the stock path.
- Halo 2 Classic weapon alignment remains deliberately deferred.

## Headset test order

1. Back up the currently installed files, then install the DLL, launcher, and
   config from the matching Stage 3H test ZIP with MCC closed.
2. In Halo 2 Anniversary gameplay, change HUD size, aspect, and vertical offset
   separately in the F1 menu. Confirm each control visibly changes the existing
   HUD while the native weapon crosshair remains correct.
3. Verify Halo 2 stereo, 6DOF, aim, hands, two-hand behavior, renderer switching,
   and Y+B pause/resume. Do not evaluate Classic weapon alignment in this pass.
4. In Halo 4, swap weapons and aim near and far. Confirm the authored reticle
   follows the controller/bullet-ray target in both eyes and retains weapon art,
   animation, spread, and target color.
5. Zoom and unzoom repeatedly. The native reticle must not become a black square,
   disappear permanently, or remain head-centered. A procedural fallback during
   an unproven frame is intentional.
6. Fire UNSC, Covenant, and Promethean weapons. Local first-person muzzle flash
   and Promethean weapon detail effects should be invisible; enemy fire, world
   effects, explosions, and environmental effects should remain stock.
7. Smoke-test Halo 3, ODST, and Reach, plus the existing default resolution and
   launcher/config behavior.
8. Save `HaloMCCVR.log` from the run and report the exact first failure, if any.

## Static verification record

- Exact Stage 3G input SHA-256:
  `40291c25af302046675d55f680d9f656105b766b388c72078ea4a15832adcd6e`
- Stage 3H output SHA-256:
  `0d0338b7c0b749ce8c7f956914eace9812bcc2d5d26bda14c4024043416f6249`
- Output size: 2,880,512 bytes.
- Two independent post-link builds compared byte-for-byte equal.
- Every C50 edit is guarded by its exact original bytes.
- Focused portable H2 HUD and H4 reticle logic assertions pass.
- Final PE disassembly confirms one normal CUI pass, current-pair proof,
  position-only serial publication, and the narrow local-effect wrapper.

The full MSVC/Windows test suite was not runnable in the Linux packaging
environment. Blender-only unrelated helper tests were also unavailable. Those
limitations do not substitute for the required Windows and headset test.
