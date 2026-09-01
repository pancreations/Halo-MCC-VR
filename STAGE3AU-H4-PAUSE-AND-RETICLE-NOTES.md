# Stage 3AU — Halo 4 pause black screen + authored-reticle capture framing

Input: Stage 3AT, SHA-256
`250da86094b9eb35d295f73b850ff84165a900a77c249c4539477caccca77671`.

Output: 2,919,424 bytes (geometry unchanged), SHA-256
`6365ac47ea7d2d7e934ba7f7bdd74bcb6c2ada1293697fb8fc2cfe52ec9d8d67`.

## 1. The pause black screen — measured, not inferred

2026-08-28 08:38 Steam log:

```
33.591  controller edge: B
33.591  Halo 4 pause exit: B edge while head-locked pause target active
        -> stereo restore requested
33.599  pause transition: fade out -> stereo 3D
33.600  controller edge: Y                        <- 9 ms later
33.600  pause fallback: injecting Start, target=head-locked 2D
33.608  pause transition: fade out -> head-locked 2D
33.817  pause transition: presentation switched to head-locked 2D
```

Nothing after 33.817 ever switches back — the session ran to its end in
head-locked 2D presentation while `fps 119 (stereo on)` shows the mod still
rendering stereo. That mismatch is the black screen.

The pause chord is Y+B. Pressing **B to leave** the pause menu satisfies the
Halo 4 pause-exit edge (correctly restoring stereo) and the trailing **Y of the
same physical press re-completes the chord**, injecting a second Start. The
game leaves its menu; the mod goes back to 2D and stays.

**Fix:** `Input_RequestPauseToggle` (`0x86A10`) is spliced through a gate that
refuses for 400 ms after a Halo 4 pause-exit restore. Stage 3X already
timestamps exactly that event at `0x2F31C8`, so no new state exists. With a
zero timestamp (no restore has ever happened) the stock path runs unchanged.

## 2. The crosshair — why the capture was empty

Stage 3AS's probe settles it. Across **1792 consecutive samples** it measured
`alpha 0 rgb 0`. Not an alpha-write problem: **nothing was landing at all.**

The one non-blank capture in the whole session appeared during the pause
transition — `art 4263`, logged at 08:38:32.644, in the window that also
recorded 201 captures at a *different* transform base (`-1033.578/336.549`
instead of the gameplay `-1893.000/1064.517`). That is the pause-menu CUI being
captured and promoted — precisely the C-H4-50 false positive the source warns
about, and precisely the "random element in the place of the VR crosshair" from
the headset report.

**Cause of the blank gameplay capture.** The Halo 4 branch of
`BeginAuthoredReticleCaptureInternal` loads a 16-byte viewport constant at
`.rdata 0x1BF0C0` = `{2048, 2048, 0, 1}`; the code shufps-extracts lane 1 as
Height and derives `TopLeft = (512 - extent) * 0.5`. Halo 4's CUI draws in
raster-pixel space with measured half-extents **1893 × 1064.517**, so a
2048-wide capture viewport **minifies by 0.54×** — and this file's own C-H4-47
note records that minifying a thin/hollow reticle outline into the capture
produces a totally blank result.

**Fix:** the constant becomes `{7572, 4258.068, 0, 1}` — 4× the measured
half-extents, i.e. the full CUI extent at **2× magnification**. Lines get
thicker rather than thinner, the CUI centre still lands at the texture centre
(`TopLeft = (512 - 7572) * 0.5 = -3530` puts CUI x=0 at texture x=256), and the
authored reticle (nominal height 81.92 CUI units) occupies ~32% of the 512×512
capture — the same order as Halo 3/ODST's accepted 4× occupancy.

The builder asserts this constant has **exactly one reference in the image**
(`0x11C77`), so nothing else can be affected.

**Calibration caveat:** 7572/4258.068 are 4× the half-extents measured at this
session's 3786×2730 raster. At a different render resolution the framing will
be off and the constant needs recomputing from the `last native base` values
the `C-H4-48` log line prints every 2 s. Making this self-calibrating (read
`cuiReticleBaseX/Y` at capture entry) is the follow-up once the framing is
confirmed in a headset.

## 3. Probe throttle

The Stage 3AS reticle probe now logs every 16th measure instead of every 64th,
so ink movement shows up without waiting on the post-publish sampling cadence.

## What the next log should show

* `S3AS reticle probe: alpha N rgb M` with **non-zero** values during gameplay.
* `Halo 4 reticle upload: ... art <non-zero>` with a **falling** `blankHeld`.
* No `pause fallback: injecting Start` within 400 ms of a
  `Halo 4 pause exit` line, and the last `pause transition` after a resume
  reading `stereo 3D`.

## Reproduction

```text
py -3 tools/build_stage3au_h4_pause_and_reticle.py \
    built/Stage3AT-HaloMCCVR.dll built/Stage3AU-HaloMCCVR.dll
```

Guards six surrounding sequences, the constant, its load instruction, the
pause-toggle prologue/continue/refusal edges and the probe throttle; asserts
the sole-reference property of the constant and that the gate lands in
genuinely zeroed page tail; decodes the emitted gate with capstone and asserts
all five external targets.

## Deployed

2026-08-28, both editions, hash-verified. Previous DLLs preserved under
`out/deploy-backups/before-stage3au-20260828-090303`.
