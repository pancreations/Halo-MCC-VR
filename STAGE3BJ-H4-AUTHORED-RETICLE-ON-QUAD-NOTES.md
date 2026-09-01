# Stage 3BJ - stop forcing Halo 4 to the placeholder crosshair

**DLL:** `built/Stage3BJ-HaloMCCVR.dll`
**SHA-256:** `22ea1c1cb18554874fc1255b7440e3a6bc32c0528faf89a200a997b6b4a5a0ad`
**Base:** Stage 3BI `dad8a373...`  |  **Change:** 2 bytes at `0x199B3`

## Why the loop kept happening

`src/common/halo4_cui_reticle_logic.h`:

```
// Until the authored H4 capture has a narrower, independently proven
// widget boundary, keep that procedural art as the only H4 quad content.
constexpr bool Halo4CuiReticleUsesProceduralFallback(
    bool authoredCaptureLive, bool crosshairEnabled, bool killNativeReticle)
{ return authoredCaptureLive && crosshairEnabled && killNativeReticle; }
```

Unconditionally TRUE for Halo 4 whenever the capture is live. **No capture
work could ever have shown up on the quad.** Every stage from 3BC to 3BI
was improving a picture the shipped policy then refused to display. That
is the "endless loop back to the placeholder crosshair".

In `EnsureReticleChain` (0x19920) the flag lands in `dl` and drives two
things:

```
0199F0  test dl,dl / je 0x19B3F   ; authoredThisFrame && !bootstrap
                                  ;   -> keep the authored art  (SKIPPED for H4)
019A72  test dl,dl / jne 0x19A33  ; kProceduralOpacity
                                  ;   -> 1.0f OPAQUE procedural painted over it
```

## The change

```
019998  cmp bl, 4                 ; GameTitle::Halo4  (dominates the site)
01999B  jne 0x199B7
01999D  test al,al / je 0x199B7   ; titleHasAuthoredCapture
0199A1  cmp [0x2ADBB1],0 / je     ; g_config.crosshair
0199AA  cmp [0x2ADC04],0 / je     ; g_config.kill_reticle
0199B3  mov dl,1   ->  xor dl,dl  ; <-- THE PATCH
0199B5  jmp 0x199B9
0199B7  xor dl,dl                 ; every other title already lands here
```

`B2 01` -> `30 D2`. Same length, flag-safe (the next flag consumer is
`cmp byte [0x2AE43E],0`, which sets its own).

Halo 4 now behaves like Halo 3 / ODST / Reach:
- authored art in the swapchain returns early and is never repainted, and
- the procedural fallback becomes fully transparent (0.0f) instead of
  opaque, so it can no longer cover the authored reticle.

**Scope:** the site is dominated by `cmp bl,4`; all four guard exits go to
`0x199B7`. Halo 3, ODST, Reach and Halo 2 are bit-for-bit unaffected.

## Why it is safe to lift the policy now

The policy names its own release condition - "a narrower, independently
proven widget boundary". Stage 3BI supplied it, and the 20:31 log proves
it: the capture map lost every full-width HUD band (rows 1-14 blank, one
small element left) and `art` fell from ~1700-2500 to 407-457 with
`blankHeld 0` and uploads succeeding.

## Test

`python tools/test_stage3bj_h4_authored_reticle_on_quad.py` - PASS.
Proves exactly 2 bytes changed, the new encoding decodes as `xor dl,dl`,
all four guards exit to the shared false path (Halo-4-only scope), `dl`
is not reassigned before either consumer, and the 3BI/3BH payloads,
CREDIT/ODST bytes and 12 sections are intact.

## Deployment

Installed 2026-08-28 into both editions, each verified as
`22ea1c1c...`. MCC confirmed closed, game never launched, no config
touched. Prior 3BI installs and the 3BI log preserved under
`out/deploy-backups/2026-08-28-pre-3BJ/`.

## Oracle

- **Authored reticle on the crosshair** -> the request is delivered.
- **Nothing at all on the crosshair** -> the procedural is now correctly
  transparent, so the small element 3BI captures is not the reticle; the
  map's remaining blob (rows 21-23, cols 13-18) is then the thing to
  identify, and H4EK `tags/ui/hud/...` is the reference.
- **Wrong small art** -> same, and the blob's map coordinates give its
  offset directly.
