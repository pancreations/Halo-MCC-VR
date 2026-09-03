# C-H4-56 Halo 4 helmet visor frontend restoration

Status: **accepted in the user's headset (2026-09-01)**.

C-H4-56 is one Halo-4-only correction above C-H4-55. It was packaged without
installing into MCC and was not published to GitHub. The accepted source is
`271f6dffb8cf2e13dc4feafd85b9b4c61440ff25`; the packaged DLL SHA-256 is
`9a28247337eac1509048884c35517c43328ffcb97f2b0190c95e6cf67976e309`.

## Accepted headset result

Steam edition, SteamVR/OpenXR 2.17.7, Oculus runtime headset, 120 Hz. The user
confirmed that C-H4-56 restored the authored helmet/visor framing while the
native reticle and all other Halo 4 behavior remained correct: “fantastic. You
nailed it.” The Microsoft Store edition was not exercised by this result.

## Headset result that selects this correction

The user confirmed that C-H4-55 restored Halo 4's exact native reticle and
that the remaining Halo 4 behavior was in good standing. The only reported
failure was the missing authored Mjolnir helmet/visor framing.

The supplied Steam / SteamVR OpenXR 2.17.7 / Oculus 120 Hz log for source
`6cccbc8ec65d4f690869f8fcf6224d151f39f1cd` proves that the exact visor shader
path itself was healthy:

- pixel-shader hash `4BE62AC49C2BF210` was found and registered;
- `halo4_helmet` changed between visible and hidden at runtime; and
- hidden intervals produced exact-shader suppressions while visible intervals
  forwarded it.

The failure is therefore before `PSSetShader`: the visor shader exists and is
bound, but its authored edge geometry is not in the VR eye crop.

## Positive engine and donor evidence

H4EK's `ui\hud\player_huds\mc_hud\mc_hud.cui_screen` binds
`prop_hud_helmet_visible` to `container_visor` and `container_visor_glow`. Its
eight authored polyart leaves are `helmet_top/bottom/left/right` and
`glow_top/bottom/left/right`. This is stock CUI art, not a mesh asset that the
mod must inject.

The supplied headset-working V6 donor's Halo 4 HUD wrapper at `.h4hs+0x2EA`
applies size/aspect/height to every root beneath the mod's complete gameplay-
CUI frontend callback depth. Its curvature bridge uses that same depth. The
Stage 3X recovery record independently identifies this admission as the CUI
frontend callback-depth field. C-H4-55 instead admitted the affine only while
the narrower reticle-owned subpass flag was set.

That difference excludes sibling visor roots from the VR HUD transform. Large
edge framing can remain on the stock flat-screen canvas and outside the eye
crop even though its exact pixel shader is visibly active in telemetry.

The Nexus mod **Toggle HUD for Halo 4** independently exposes the helmet visor
as a separate stock-HUD element. It supports the same content identity but is
not used as a binary or address source.

## The isolated change

C-H4-56 restores the donor's complete frontend admission for Halo 4's existing
native HUD affine and curvature paths. Sibling visor roots now receive the
same configured size, aspect, curvature, and vertical offset as the rest of
the gameplay HUD.

The C-H4-55 private native-reticle replay remains explicitly excluded and
keeps its stock affine/curvature canvas. Pause presentation also remains stock.
The exact visor shader hash, default-visible checkbox, suppression hook,
reticle selection, reticle capture, effects, pause, camera, hands, stereo, and
OpenXR paths are otherwise unchanged.

## Static verification

- A pure admission test pins complete frontend depth, nested depth, private
  reticle exclusion, pause exclusion, and out-of-scope rejection.
- The normal Release build and core tests must pass.
- The Reach consistency gate and a Halo-4-disabled compile must pass.
- Packaging must remain non-deploying and produce matching binary/source ZIPs
  from the exact committed source.

## Headset test

1. Enter Halo 4 gameplay with **Show Halo 4 helmet frame** enabled. Confirm the
   green authored helmet top/bottom/side framing and its lighting are visible.
2. Disable that option. Only the helmet/visor framing should disappear.
3. Re-enable it and confirm the framing returns immediately.
4. Confirm the C-H4-55 native weapon reticle remains exact and controller-
   aimed, including a weapon change.
5. Briefly pause/resume and verify ordinary HUD, muzzle/Promethean suppression,
   hands, stereo, and tracking remain unchanged.
6. Preserve the log and report the edition, runtime, headset, and result.
