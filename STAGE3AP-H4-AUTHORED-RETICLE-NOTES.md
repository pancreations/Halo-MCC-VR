# Stage 3AP — Halo 4 authored reticle on the VR crosshair

Baseline: the user's accepted Stage 3AL DLL, SHA-256
`fb1e6b5d7a584930303b2f6aed6696c4012e24f007538d7c7955ad75ca583da2`.

## What was actually wrong

Stage 3AL already contains the complete Halo 4 authored-reticle path, identical
in shape to the one Halo 3, ODST and Reach use:

- `Halo4CuiGameplayRenderBody` replays the gameplay CUI stream once per sampled
  frame into the private centred capture target;
- the visible pass moves only the native flat reticle copy offscreen;
- the shared OpenXR reticle quad presents whatever art the capture holds;
- `Game_TitleCapturesAuthoredCrosshair()` already returns true for Halo 4.

Every one of those pieces shipped. The capture never ran, because
`VR_ShouldCaptureAuthoredReticleThisFrame` opens with a Halo 4 early return
added by C-H4-50:

```c
if (TitleAdapter_GetActiveTitle() == GameTitle::Halo4)
    return false;
```

Stage 3X's notes record this as deliberate: *"C-H4-50 procedural bullet-ray
reticle remains the only H4 reticle presentation. The exact fail-closed branch is
not changed."*

The result in the headset is exactly what the user reported for weeks: Halo 4's
own crosshair disappears (the hide works) and the VR crosshair never carries the
real reticle art (the capture is switched off), leaving the procedural marker.

## The change

At RVA `0x03089B`, six bytes:

```
0x030890  4883ec28        sub  rsp, 0x28
0x030894  e8f43a2c00      call 0x2F438D        ; Stage 3X H4 heartbeat wrapper
0x030899  3c04            cmp  al, 4           ; GameTitle::Halo4
0x03089B  0f84a2000000    je   0x030943        ; -> return false   <-- REMOVED
          909090909090                         ; six NOPs
0x0308A1  803d97db270000  cmp  byte [g_reticleContainsAuthored], 0
```

Halo 4 now falls into the same cadence logic every other title uses. That code
was already written for it: the switch below decodes `GameTitle::Halo4` and
drives `g_config.crosshair_animation_frames` through the identical clamp Halo 3
uses (6..60, default 30 when animation is off).

## Scope

The whole file differs from Stage 3AL by exactly six bytes. Nothing else is
touched: no section is added or resized, the PE checksum stays unset as the whole
lineage ships it, and the Stage 3X title-query heartbeat immediately above the
patch still runs.

No other title can be affected. The removed instruction is the Halo 4 comparison
itself; every instruction below it is shared code that is byte-identical, and
Halo 3, ODST, Reach and both Halo 2 modes reach it exactly as before.

## Expected headset result

In Halo 4, the VR crosshair carries Halo 4's own reticle art, and it follows the
weapon rather than the head. `HaloMCCVR.log` should show the C-H4-48 line
reporting authored captures, and the reticle-upload line should report a non-zero
`art` value instead of `blankHeld`.

## Reproduction

```text
py -3 tools/build_stage3ap_h4_authored_reticle.py \
    built/Stage3AL-HaloMCCVR.dll built/Stage3AP-HaloMCCVR.dll
```

The builder refuses any input but the exact Stage 3AL SHA-256, guards the
function prologue, the `cmp al, 4`, the `g_reticleContainsAuthored` test and the
return-false epilogue before writing, and refuses to change the file size.
