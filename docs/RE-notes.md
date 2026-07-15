# Reverse-engineering notes — halo3.dll camera (M1)

**Game build:** MCC `1.3528.0.0`, `halo3.dll` (~10.6 MB on disk, ~71 MB mapped image).
**All addresses below are RVAs (offset from halo3.dll's runtime base).** They are specific to
this build and WILL move on an MCC update — they must be turned into AOB signature scans before
shipping (see the `sig` module). They're recorded here so we don't have to re-derive them each
session.

## How these were found

Without Cheat Engine (user's AV blocks it), we built our own read-only external finder,
`tools/camscan.cpp`, plus `tools/disasm.py` (capstone). Workflow used:
`attach` → `first halo3` (baseline) → `narrow inc/dec/changed/unchanged` (user looks up/down on
cue) → `clusters` → `watch` (confirm it tracks the view) → `poke`/`spin` (write-test) →
`xref` / `findwrite` (hardware watchpoint) → `hex` + `disasm.py` (read the code).

## Findings

- **`halo3.dll+0xA84258` — a camera/observer struct.**
  - `+0x0C..+0x14`: position, roughly `(x≈176.9, y≈5.4, z≈-5.5)` on the test level.
  - `+0x18` (`0xA84270`): **forward unit vector** (i,j,k). Tracks the view perfectly when read.
  - **This forward is a computed READOUT, not an input:** writing/spinning it externally did
    nothing, and there are **no direct RIP-relative references** to it (the game reaches it via a
    pointer). So the render does not read this copy.

- **Writer of that forward:** `halo3.dll+0x2A86CD` `movdqu [rbx+0x18], xmm0` (rbx = struct base
  `0xA84258`). The enclosing function computes the forward into a stack local `[rdi]`, optionally
  negates it (hemisphere flip via a dot product), then copies it in. It reads engine globals
  `rdx/r10 = 0x2D2F680` and `r11 = 0xA67630`. (Disassembly around `0x2A8600–0x2A8720`.)

- **`halo3.dll+0x2D2F680` — a camera object** (vtable/ptr at `+0x00`):
  - `+0x08`: position `(≈-5.5, 176.9, 5.4)`
  - `+0x14` (`0x2D2F694`): **forward** unit vector
  - `+0x24` (`0x2D2F6A0`): **up** unit vector
  - `+0x30`: looks like projection/frustum params (`0.858, 0.874, 1.0`)
  - Spinning `+0x14` externally made **the first-person gun + HUD flash** (not the world) → this
    is (at least) the **weapon/overlay camera**, and external writes only flicker because they
    race the game's per-frame recompute.
  - It is the **first element of a 4-slot array** (0x2820 bytes per object). The builder function
    at `halo3.dll+0x68BC` does `lea rbx,[rip+disp] → 0x2D2F680; mov edi,4;` then loops the ctor
    advancing rbx by 0x2820. On-disk scan found 8 RIP-relative refs into `0x2D2F680..+0x40`
    (others at RVAs 0x152486/0x15249A/0x1524B0 read floats `+0x20/+0x24/+0x28`; 0x185745,
    0x24CA71, 0x24CC28, 0x2B7A12 reference the base). `kGunCamRefSig` in `game.cpp` matches the
    builder prologue uniquely (an otherwise identical builder for another array uses stride
    0x1630, so the stride bytes are part of the signature); the gun camera address is
    `match+17 + int32 at match+13`. In stereo, `RenderViewHook` rewrites `+0x30/+0x34` to the
    world-raster tangents (× user scale, Home/End) so the weapon/HUD aren't magnified ~2x by the
    widened world projection.

- **World render camera** lives around `~0x468xxxx` (position `≈176,5,-8`, orthonormal basis) but
  it's a **sliding array/ring** (render history for temporal effects), so no single fixed address
  is a stable write target there.

- **Projection values pass through the M1 camera-copy hook.** Live disassembly of the original
  function at `halo3.dll+0x2A628C` shows `src+0x68/+0x6C` copied (after a common scale factor) to
  compact render-camera `dst+0x28/+0x2C`. This is a much cleaner M2 entry point than chasing the
  sliding render-history array. A short startup diagnostic now logs the first 24 copies so we can
  identify whether this function receives world, weapon/HUD, or multiple camera passes.
  - Headset test confirmed a single stable destination and two alternating source buffers:
    `dst=...BEBEF688`, sources `...2544A4A8` / `...24F4A4A8`.
  - Both sources carried position `(-9.14, 175.69, 5.38)` and projection values
    `(1.091595, 1.114286)` during the test. Calls arrived at render cadence. This identifies the
    hook as the world-camera path and gives M2 a place to apply alternating left/right position
    offsets and, later, per-eye projection overrides.

## Key lesson / next step

### Critical stereo correction and successful raster redirect (2026-07-14)

The original F11 path did **not** prove two independent world images: calling the inner renderer
twice raised GPU work but reused shared targets. The corrected path now binds distinct final-scene
RTVs per eye, and the user confirmed the headset stereo image works beautifully. An objective
pixel-disparity capture remains useful validation, but the shared-backbuffer path is superseded.
One-time GPU readback subsequently measured mean RGB delta 6.262 and 48.8% changed samples
(11,631/23,842), objectively confirming the submitted eye textures are distinct.

Do not repeat: exaggerated OpenXR eye poses (double vision), modifying shader constant `0x270000`
(reconstruction warp/holes), or changing shared projection `view+0x98+0x78` (weapon/HUD damage).
See `CONTINUATION.md` for the mandatory proof sequence.

The camera-copy destination cannot be safely treated as weapon-only. A test that copied the original
stick-aim vectors to `dst` while leaving headlook in `src` disabled world head tracking. Therefore
weapon/HUD decoupling must be found after this shared camera copy, at the overlay render pass or a
weapon-specific transform; do not repeat the save/restore split in `CamCopyHook`.

The raster path hooks D3D11 `OMSetRenderTargets` at vtable index 33. Census showed Halo's inner pass
does not bind the game backbuffer. The correct final scene target is the unique full-resolution
`R8G8B8A8_TYPELESS` resource with RTV+SRV+UAV bind flags (`0xA8`), bound after the typed RGBA
intermediate. Redirect that final resource only. Redirecting the format-28 intermediate is black.
Fake post-render backbuffer copies remain disabled. Eye caches use the original RTV's typed view
format so Halo's sRGB conversion is preserved.

External poking can only ever *flash* a camera value because it races the game, which rewrites
these every frame right before rendering. **To hold the camera steady, the mod must write from
inside the game's frame** — i.e. hook the game's camera-update function (e.g. the one at
`0x2A86D2`) or the render-camera setup, and overwrite the orientation there each frame with the
head pose. That's the M1 mechanism to build next.

## Coordinate convention (from observed data)

Halo forward is a unit vector `(i, j, k)` with **k = vertical** (up/down look → k changes) and
`(i, j)` = horizontal heading. Engine appears **Z-up**. OpenXR head is Y-up, -Z forward, meters —
so mapping head→game forward needs an axis swap + a yaw recenter (to be calibrated in-headset).
