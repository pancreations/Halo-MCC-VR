# C-H4-53 Halo 4 restoration candidate

Status: **unaccepted; headset test required**.

Runtime result (2026-09-01): pause/resume, local first-person effects, and the
adjustable HUD passed in the user's Steam / SteamVR OpenXR / Oculus headset.
The candidate is not accepted as a whole: authored-reticle capture regressed
to the procedural fallback, and every log sample reported the attempted
`helmet_armor` binding as `StockFallback`, leaving the new menu toggle inert.
Those two CUI defects are addressed by C-H4-54.

This cumulative candidate carries the headset-accepted C-H2-88 behavior
forward unchanged. It does not advance `docs/CURRENT-STATE.md`.

## Halo 3 behavior being matched

Halo 3 keeps its camera core armed across pause, presents the game's native
pause screen as a head-locked 2D layer, then returns to stereo from the native
resume state. Its HUD controls remain live in gameplay, optional visual-feature
failure leaves only that feature stock, and first-person muzzle artifacts do
not sit at the wrong stereo depth.

## Restored Halo 4 paths

- **Pause:** the unique retail
  `game_is_paused_for_reason(int32_t)` body at `halo4+0xA0AE4` is called with
  reason 3. After a 50 ms stable mismatch it authoritatively drives the
  presentation target. While the pause target is active, the Halo 4 render
  wrapper takes its stock path so the native menu/backbuffer remains visible.
  This is the Stage 3AW behavior, not a second inference from raw Start input.
- **First-person effects:** the exact Stage 3AI C50 negative, helper, transient,
  and mode-1 bindings are restored at `+0x1059A2`, `+0x100EE8`, `+0x1012D5`,
  and `+0x27BD36`. Only a selected local-first-person descriptor has its
  resolved effect origin moved to finite-far coordinates. The Promethean and
  muzzle-effect route is isolated from camera ownership.
- **HUD:** Stage 3X's native gameplay-CUI root at `+0x3F313C` applies
  `hud_size`, `hud_aspect`, and `hud_vertical_offset` after the stock function.
  The native `prop_curvature_theta` consumer at `+0x420D7E` applies
  `hud_curvature`. Pause and shell CUI calls remain stock. The rejected
  C-H4-44 tag-basis writer remains dormant.
- **Helmet:** `halo4_helmet` defaults to 1 and appears in the HUD menu as
  **Show Halo 4 helmet frame**. The implementation resolves the boolean
  `helmet_armor` debug variable by name and type; no fixed data RVA is used.
  Resolution failure preserves all authored helmet art.

Every binding requires a unique loaded-image signature at its pinned retail
RVA. Zero/multiple/moved matches leave that feature stock and log
`StockFallback`; they do not disarm the camera, stereo, or OpenXR. Code-patch
teardown restores only bytes owned by this candidate and retains cleanup state
on any unknown byte sequence.

## Static verification

- The Steam 1.3528 `halo4.dll` image has one match at every selected binding.
- The C50 reserved cave `+0xB79C10..+0xB79CFF` is zero in the pinned image.
- `halomccvr_core_tests` covers the exact pause getter body, descriptor
  admission, HUD affine/clamps, Halo 4 HUD capability, and config defaults.
- The normal package pipeline builds/tests, verifies the candidate manifest,
  and installs the exact candidate into every detected MCC edition without
  changing an existing config.

## Headset test

On the log-identified edition/runtime/headset:

1. Enter Halo 4 gameplay and verify normal stereo, hands, aiming, and reticle.
2. Pause and resume at least three times. Every pause must show the native
   head-locked screen on the first attempt; every resume must return to stereo
   on the first attempt.
3. Fire a conventional weapon and a Promethean rifle. Confirm no local muzzle
   flash or Promethean first-person effect remains misaligned in either eye.
4. Change HUD size, aspect, curvature, and height in the F1 menu. Confirm each
   changes the gameplay HUD and does not alter the pause/shell UI.
5. Toggle **Show Halo 4 helmet frame** off and on. Confirm only the authored
   helmet/visor shading changes and the rest of the HUD remains visible.
6. Exit to the MCC shell, re-enter Halo 4, then run a Halo 3 regression pass.

Acceptance requires the user's explicit headset result plus a log identifying
the MCC edition, OpenXR runtime, and headset.
