# C-H4-55 Halo 4 native reticle and helmet shader restoration

Status: **ready for headset test; unaccepted**.

C-H4-55 is one cumulative correction above the headset-rejected C-H4-54
candidate. It carries the headset-confirmed C-H4-53 pause/resume, local
first-person effect suppression, and adjustable HUD paths unchanged. It does
not advance `docs/CURRENT-STATE.md`.

## Halo 3 behavior being matched

Halo 3 captures the game's authored weapon-reticle pixels on a private stock
HUD replay, presents those pixels through the controller-aim VR reticle, and
keeps unrelated authored HUD framing visible by default. A visibility option
may suppress the framing feature without changing radar, reticle, ammo, shield,
camera, stereo, hands, or OpenXR behavior.

## Reticle correction

The supplied Steam / SteamVR OpenXR 2.17.7 / Oculus 120 Hz C-H4-54 log for
source `18111adf27fbfccd508baec744262bb9387eb236` still reported no usable
authored art. Its capture framing consumed the adjustable visible HUD canvas,
approximately `-763.818/336.072`.

The supplied last-known-good native-reticle log for source
`bda7ecb93cdfcc982469e8ba92f888e05490511a` reported nonblank Halo 4 uploads,
including six uploaded frames and 830 art pixels, on the stock canvas near
`-1456.000/818.772`.

C-H4-55 keeps the private capture replay outside the Stage 3X affine and
curvature consumers, measures that replay's own live transform, and uses that
stock replay canvas for the existing 3CX capture bias, scale, and hide framing.
The ordinary visible pass immediately regains the four HUD controls. Before a
valid replay measurement exists, capture uses the prior visible-pass canvas as
a bounded startup fallback. Invalid measurements fail open for the reticle
feature only.

The rejected C-H4-54 zero-X payload discriminator is not used. The visible and
private passes again admit every retail type-`0x28`, size-`0x0C` marker exactly
as the headset-confirmed `bda7ecb` implementation did.

## Helmet correction

The supplied known-good `7a24814` runtime log proves that the working helmet
option was not a CUI transform or `helmet_armor` property. It reports:

- discovery of exact 3DMigoto pixel-shader hash `4BE62AC49C2BF210`;
- the bridge active without modifying CUI or radar properties; and
- suppression of only that visor pixel shader when the HUD checkbox is off.

Disassembly of the supplied working V6 donor independently confirms the same
unseeded 64-bit FNV-1 hash at pixel-shader creation and a `PSSetShader` bridge
that replaces only the exact visor shader pointer with null. The device
context, class-instance pointer, and class-instance count are forwarded
unchanged.

C-H4-55 implements that source-native path. `halo4_helmet=1` remains the
default and forwards every shader binding unchanged. With the option off, only
the registered exact visor shader is nulled. Missing or ambiguous hook state
leaves the authored helmet stock and does not affect any other feature.

## Static verification

- Core tests pin the exact shader hash and every fail-open suppression gate.
- Core tests pin stock replay-canvas preference, visible-canvas startup
  fallback, and invalid-sample rejection.
- The package gate requires the replay/visible canvas split and exact shader
  bridge while preserving the C-H4-53 pause, effects, and HUD evidence.
- The normal release build, core tests, Reach consistency gate, and a Halo-4-
  disabled compile must pass before packaging.
- Packaging is non-deploying by default. No MCC installation or GitHub publish
  is part of this candidate handoff.

## Headset test

1. Enter Halo 4 gameplay with `halo4_helmet=1`. Confirm the authored helmet
   frame is visible and the weapon's native reticle—not the procedural dot—is
   presented at controller aim.
2. Switch weapons and zoom where practical. Confirm native reticle art changes
   and animates normally.
3. Disable **Show Halo 4 helmet frame**. Only the visor/helmet framing should
   disappear. Re-enable it and confirm the frame returns immediately.
4. Exercise HUD size, aspect, curvature, and vertical offset. Confirm the
   visible HUD responds without changing the captured native-reticle art or
   helmet toggle behavior.
5. Pause/resume twice and fire conventional and Promethean weapons to verify
   the confirmed C-H4-53 paths did not regress.
6. Preserve the resulting log. Acceptance requires the edition, OpenXR
   runtime, headset, source identity, and explicit headset result.
