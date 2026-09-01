# Stage 3BH — adopt the engine's own CUI framing; inspect every texel

Status: **installed to both editions 2026-08-28, awaiting headset result.**

- Input Stage 3BD `2E19F93A…`; output Stage 3BH
  `80573ED9FFFD3A557DEA96B80823E490ECDAEA7C0063EE6A5E1BACC4486D48C4`
- 1580-byte payload at 0x2FB810 (gate 0x2FB850, tap 0x2FB9ED, dump 0x2FBA69),
  three call splices, plus the 16-byte framing constant at 0x2FB800.
- Prior 3BG install preserved under `out/deploy-backups/2026-08-28-pre-3BH/`.

## What Stage 3BG measured (Steam log, 18:44)

```
S3BG vp: engine viewport at captured draw tl=-768,-768  wh=2048x2048   <- ours, from Begin
S3BG vp: engine viewport at captured draw tl=-1811,-417 wh=4134x1346   <- the ENGINE's, x7
```

Two facts follow, and they are the whole stage:

1. **The engine already centres its CUI on whatever target is bound.** That
   rect's centre is `(-1811+2067, -417+673) = (256,256)` — exactly the centre
   of the 512x512 capture texture. Its size is exactly 4x the live CUI
   half-extents the log reports (`4 x 1033.578 = 4134.312`,
   `4 x 336.549 = 1346.196`), i.e. precisely what
   `BeginAuthoredReticleCaptureInternal`'s own `(512-W)/2` centring produces
   when fed those numbers.
2. **It frames ±128 authored units square.** The H4EK reticle is ±37, so it
   occupies **28.9% of the texture in both axes** — the same occupancy Halo 3
   and ODST ship. The constant we had been shipping since 3BD ({2048,2048})
   frames **±258 x ±84**: wrong aspect (the CUI aspect is 3.07:1, ours was
   1:1), and it was overriding the engine's correct framing at every draw.

So every stage from 3BD onward was fighting the engine to install a worse
framing than the one the engine was already choosing.

## The change

- **Constant 0x2FB800: `{2048,2048,0,1}` → `{4134.312,1346.196,0,1}`.** Begin,
  the existing reroute re-assert, and the draw-time gate now all agree with
  the engine, and the pin becomes a stabiliser rather than an override.
- **The map now samples every texel.** Stage 3BG read ONE texel per 16x16 cell
  — 0.4% of the texture. The reticle's ticks are 2.2 authored units (~4 px)
  and its arcs are thin outlines, so 3BG's blank centre was **not evidence of
  absence**. The dump now takes the max over all 256 texels of each cell.

Everything else is Stage 3BG unchanged (guards, telemetry, one-shot gating,
fail-open behaviour, Halo 4 only).

## Reading the next log

`S3BH map:` + 32 rows, at true full coverage this time:

- **arcs/ticks visible** → if off-centre, the cell coordinates give the exact
  translation; if centred, this is the fix and only size remains.
- **still blank at centre with the HUD bands present** → the reticle's draws
  are genuinely not in the captured stream at that moment, which points at the
  known render-section batching boundary (`renderer+0x1c2c`, the thing that
  defeated C-H4-43j) rather than at framing. That is a different fix, and the
  map is what licenses it.

## Verification

`tools/test_stage3bh_h4_engine_framing_and_full_map.py`: PASS — decodes all
three splices, the gate, the tap, and the dump from the welded bytes; asserts
the framing constant reproduces the measured `tl=-1811,-417 wh=4134x1346` and
keeps the live CUI aspect; asserts the 16x16 dx/dy loops and the running max
(so the sampler cannot silently regress to point-sampling); CREDIT/ODST bytes;
12 sections; no byte changed outside the four intended regions.
`tools/test_stage3bd_h4_capture_zoom.py`: PASS.
