# Stage 3BF — draw-time framing pin + engine-viewport telemetry

Status: **installed to both editions 2026-08-28, awaiting one gameplay run
and its log.**

- Input Stage 3BD SHA-256: `2E19F93A…` (same base as 3BE; whole payload
  region rewritten).
- Output Stage 3BF SHA-256:
  `A1D23FC8E67E077C7B72C53C33F7D12606DD15CBBE2A48DBCD0303A18BEE67F3`
- 652-byte payload at 0x2FB810 (gate 0x2FB820); same two census-hook call
  splices as 3BE (0xD0C5/0xD229). Previous 3BE install preserved under
  `out/deploy-backups/2026-08-28-pre-3BF/` (both `741A0E69…`).

## What Stage 3BE proved

Headset result: "now it's the health bar AND objective" — for the first time
the captured art is **consistent and identifiable** rather than a per-run
random element. The draw-time pin works; the capture window is deterministic.
It is simply aimed at the wrong part of the HUD: the CUI draws' NDC→screen
mapping is not the centred full-frame mapping the {-768,-768,2048,2048}
constant assumes (that constant would be exact only if the engine's own
viewport for these draws were the full square-ish frame).

## What 3BF adds (no framing change — no fourth guess)

The missing calibration is the engine's own viewport for these exact draws:
it defines where the screen centre (the reticle) sits in NDC. The gate now
logs, for the first 8 captured draws of a session, the live viewport and
scissor read BEFORE the override:

```
S3BF vp: engine viewport at captured draw tl=%d,%d wh=%dx%d
S3BF sc: engine scissor at captured draw %d,%d..%d,%d
```

plus the existing one-time `S3BF:` proof line. Everything else is
byte-identical 3BE behaviour (pin to the saved 0x2AE774/0x2AE78C framing at
every draw whose bound RTV is the H4 capture target; Halo 4 only; immediate
context only; idle cost one compare).

Reading the numbers: sample 1 arrives before our first override, so it is the
engine's genuine framing (or Begin's own, if nothing intervened). Later
samples read {-768,-768,2048x2048} where the engine left our pin alone, and
fresh engine values where it overrode — the mix tells us how many distinct
framings the engine uses inside one captured pass.

With V_e = the engine's viewport and R the raster, the final capture viewport
is arithmetic: the screen centre's NDC is V_e⁻¹(R/2), and the capture
viewport is V_e scaled by 2048/V_e.H and translated so that point lands at
texture pixel (256,256). If the engine uses ONE consistent V_e, this is a
16-byte constant change (plus custom TopLeft in the gate); if it uses
several, the gate must derive the transform per draw from the live V_e.

## Verification

- `tools/test_stage3bf_h4_capture_viewport_pin_telemetry.py`: PASS — decodes
  the welded gate (all guards, both RSGet telemetry calls, the budget cap of
  8, the pin, the tail jump), asserts both splices, the intact 3BD identity,
  CREDIT/ODST bytes, and no byte changed outside the intended regions.

## Test instructions (plain)

Start Halo 4, get into gameplay with the crosshair visible for ~15 seconds,
then quit. Nothing should look different from 3BE (still health bar +
objective on the crosshair — expected this run). The log next to the DLL
(`HaloMCCVR.log` in the edition's `Halo_MCC_VR` folder) then contains the
`S3BF vp:` lines that give the exact final numbers.
