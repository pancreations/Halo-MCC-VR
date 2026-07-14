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

- **World render camera** lives around `~0x468xxxx` (position `≈176,5,-8`, orthonormal basis) but
  it's a **sliding array/ring** (render history for temporal effects), so no single fixed address
  is a stable write target there.

## Key lesson / next step

External poking can only ever *flash* a camera value because it races the game, which rewrites
these every frame right before rendering. **To hold the camera steady, the mod must write from
inside the game's frame** — i.e. hook the game's camera-update function (e.g. the one at
`0x2A86D2`) or the render-camera setup, and overwrite the orientation there each frame with the
head pose. That's the M1 mechanism to build next.

## Coordinate convention (from observed data)

Halo forward is a unit vector `(i, j, k)` with **k = vertical** (up/down look → k changes) and
`(i, j)` = horizontal heading. Engine appears **Z-up**. OpenXR head is Y-up, -Z forward, meters —
so mapping head→game forward needs an axis swap + a yaw recenter (to be calibrated in-headset).
