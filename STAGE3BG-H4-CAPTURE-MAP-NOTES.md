# Stage 3BG — framing pin + telemetry + a picture of the actual capture

Status: **installed to both editions 2026-08-28, awaiting one Halo 4 run and
its log.**

- Input Stage 3BD SHA-256: `2E19F93A…` (pre-3BE base; whole payload rewritten)
- Output Stage 3BG SHA-256:
  `1FF5F99724F793CB3A51CC2551D017C72F2FABAE40CC0C5DFACF63E56B233355`
- 1517-byte payload at 0x2FB810 (gate 0x2FB850, tap 0x2FB9ED, dump 0x2FBA69);
  three call splices. Prior 3BF install preserved under
  `out/deploy-backups/2026-08-28-pre-3BG/`.

## Why counters had to stop being the instrument

The 3BE headset result ("health bar AND objective") plus the preserved 17:25
Steam log settle what is and isn't known:

- The pin fires: `S3BE: H4 captured draw reached with the capture target
  bound…` is in the log, and the capture path is fully healthy —
  `4–5 authored captures`, `0 write failures`, `art 1724…2489`
  (non-blank), `11–12 exact capture OM reroutes (11–12 framing reasserts)`
  per 2 s window.
- The content is now *consistent* rather than a per-run random element, so
  the window is deterministic — it is simply pointed at the wrong screen
  region.
- SCENEPROBE shows the raster is **3786x2730** and that the CUI-adjacent
  target is bound with a **947x683** viewport (exactly raster/4) while other
  passes use the full raster. So "centre of a 512-square" is not the centre
  of what these draws actually map onto.

No counter in the log can say *where* on the HUD the 512x512 window sits.
That is why the Stage 3BC post-mortem prescribed dumping the capture and
looking at it, and why this stage finally does it.

## The change

Two splices carry Stage 3BF behaviour unchanged (draw-time framing pin +
first-8 engine viewport/scissor telemetry). The third is new:

`0x2763C` — the `call LOG` that prints the `"%s reticle upload: …"` stats
line — is retargeted to `s3bg_upload_tap`, which forwards that call verbatim
(its five stack varargs copied into a fresh frame) and then, **once per
session on the third Halo 4 stats window**, copies the published capture
texture to a CPU staging texture and logs a 32x32 ASCII intensity map:

```
S3BG map: published capture 512x512 fmt=28 pitch=2048; 32x32 cells follow
S3BG 00 |................................|
S3BG 01 |.......4789887.................|
...
```

One texel sampled per 16x16 cell; `.` = blank, `0`-`9` = intensity
(max of R,G,B,A). That picture plus the H4EK authored layout makes the
correct window offset a measurement.

Reading it:
- **arcs/ticks visible somewhere** → pure offset fix; the cell coordinates
  give the exact translation to apply to the capture viewport.
- **bar/box shapes only** → confirms the health/objective region and, with
  the `S3BG vp:` lines, gives the transform to re-centre.
- **all blank** → the art the quad shows is coming from somewhere other than
  this texture, which redirects the whole investigation.

## Safety

- Idle cost unchanged: one byte compare + branch per draw.
- The dump runs once, on the render thread, on the same cold path the stats
  line already uses; it fails open at every step (null texture/device/context,
  failed CreateTexture2D or Map each log one line and change nothing).
- **MipLevels and ArraySize are deliberately copied from the source desc**,
  not forced to 1: `CopyResource` requires identical mip/array counts and the
  authored texture is mipped, so forcing 1 would make the copy silently do
  nothing. The test asserts those stores are absent from the payload.
- Halo 4 only; H3/ODST/Reach/H2 paths byte-identical (asserted).
- Revert = restore the three call displacements.

## Verification

`tools/test_stage3bg_h4_capture_pin_and_map.py`: PASS — decodes all three
splices, the whole gate, the tap's vararg forwarding and one-shot gating, and
every D3D vtable offset in the dump from the welded bytes; asserts the intact
Stage 3BD identity, CREDIT/ODST bytes, and that no byte changed outside the
three intended regions. `tools/test_stage3bd_h4_capture_zoom.py`: PASS.

## Test instructions (plain)

Start Halo 4, play ~15 seconds with the HUD visible, quit. The crosshair will
still look wrong this run — that is expected; this run is the measurement.
Then say so, and the log gets read off disk directly.
