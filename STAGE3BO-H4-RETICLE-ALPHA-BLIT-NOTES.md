# Stage 3BO - the captured Halo 4 reticle gets an alpha channel

**DLL:** `built/Stage3BO-HaloMCCVR.dll`
**SHA-256:** `129a24df07edf78ca1919e2ddaa36663d15fcbb34d5981b7fc0ab1e49b107f1b`
**Base:** Stage 3BN `1e440f34...` (headset result 2026-08-30: "the reticle is
completely invisible").

## What the capture actually holds (offline, no new probe)

The 3BM instrument wrote four raw 512x512 dumps of the published capture
during the 3BN run (Steam, 10:23). Each holds one 73x69 px figure at the
centre: an inner ring plus four thick arcs at N/S/E/W. Dumps 1/3/4 are HUD
blue (0,152,229); dump 2 is red (176,0,0) - the reticle over an enemy.
`H4EK/tags/ui/hud/reticles/unsc_assaultrifle/ar_corner.bitmap` decodes to a
96x96 white bitmap whose alpha is exactly such a thick arc. So the capture
has held the assault-rifle reticle - ring + four `ar_corner` arcs - since
3BI. Every texel of it has **alpha 0**.

The VR crosshair quad is composited with
`XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` (vr.cpp 10281): a
picture with no alpha draws as nothing. That is the invisibility. The
upload copies the capture with a plain `CopyResource` (the only blit-path
line in the log is "FAST CopyResource"), so nothing ever touched the alpha.

Earlier readings of the capture maps ("health bar", "damage indication",
"squashed by the viewport aspect") were wrong about what the figure was;
3BN's aspect change did make it round, which is what let the shape be
matched to the kit bitmap.

## The change (Halo 4 only; other titles bit-for-bit)

1. `Blit` (0x11E00) fast-path decision at 0x11E76 -> `s3bo_fastpath`:
   returns "slow" (shader path) when `ACTIVE_TITLE == 4` and the destination
   height is 512 (the reticle chain); the multisample rule and every other
   case return the original answer. Only `bl` is written.
2. The PERF transition log gate at 0x11EB6 -> `s3bo_loggate`: yields ZF=1
   (skip) for that same case so eye blits never see a FAST/SLOW transition
   and the log stays quiet; otherwise the original `cmp [loggedPath],eax`.
3. The blit pipeline's HLSL text pointer (`static const char* src`,
   0x2A6770, DIR64-relocated) -> a copy at 0x2F9540 whose two pixel shaders
   call `fix()`: for a 512x512 source only, `a = max(c.a, max(r,g,b))` and
   the colour is un-premultiplied by `a`. A 3786x2730 eye blit returns `c`
   unchanged. The shipped text at 0x1B6570 is left in place.

The reticle blit now runs `ps_linearize` (source UNORM 28, target sRGB 29),
which stores the same colour bytes `CopyResource` did, plus alpha. The SRV
for the source is cached (`AcquireSrcSrv`), so a settled frame allocates
nothing.

## Expected headset result

The Halo 4 weapon reticle (assault rifle: ring with four arcs) on the VR
crosshair, blue, turning red over an enemy, changing shape on weapon swap.
Small (~14% of the quad); size is a separate lever.

## Test

`python tools/test_stage3bo_h4_reticle_alpha_blit.py` - PASS: byte identity
outside the four regions, both thunks decoded and register-write checked,
the `je` and the `xor bl,bl` jump target intact, relocation present, text
exact and compiled with d3dcompiler_47 for all three entry points, vertex
shader and `lin()` unchanged, 3BN/3BM/3BL/3BI artifacts, 3BJ absent.
