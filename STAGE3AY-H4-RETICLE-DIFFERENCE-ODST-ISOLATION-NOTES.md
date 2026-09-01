# Stage 3AY — Halo 4 authored-reticle difference and ODST isolation

Status: **HEADSET PENDING**. Do not advance `docs/CURRENT-STATE.md` until the
user explicitly accepts the headset result.

## Halo 3 reference behavior

The game's authored weapon reticle is hidden from the flat HUD and reproduced
at the VR gun-ray crosshair. No visor decoration or unrelated HUD art belongs
on that crosshair.

## Halo 4 change

Stage 3AX proved that the captured object was a visor/HUD fragment, not the
reticle. Stage 3AY disables AX's guessed spatial crop. At the same verified
H4EK type-0x28 capture marker, it replays the same CUI command stream twice in
one frame:

1. an unchanged authored-HUD capture;
2. a discard capture using the already-proven native-reticle suppression.

The existing VR-crosshair upload then uses the absolute GPU difference between
the two captures. Visor and other identical HUD pixels cancel, leaving only the
native Halo 4 reticle pixels. This is content separation, not a coordinate or
crop guess. The discard render target is explicitly cleared, and pixel-shader
resource slot 1 is saved and restored around the differential draw.

The headset-accepted Stage 3AW pause payload at RVA `0x2FA890` is byte-identical.

## ODST change

The rejected Stage 3AX forced-readiness edge is restored to the original proven
conditional. The rejected Steam log stops immediately after the ODST preflight
profile and before the next log line; the first call at that boundary is the
optional `LocateCinematicState` scan. Stage 3AY skips only that optional
cutscene-facing scan and logs the isolation, allowing the camera-core install,
theatre/vehicle discovery, and hook installation to continue. Failure of this
optional feature cannot gate or tear down the working ODST VR path.

## Verification

- Stage 3AY static acceptance: pass
- Stage 3AW pause static acceptance: pass
- Stage 3AL ODST pin preservation: pass
- Stage 3AX guarded-input acceptance: pass
- Reach consistency check: pass
- Candidate SHA-256: `44850c02a97e284b35f00585479d9b0391bc190c758f15efd88bce733c2e50ad`
- Rejected Stage 3AX Steam log SHA-256: `ab1ee7cff7491a9d2d7e80dedbb6cfb8b0acffe9f8d9a5c42140b65181189907`

Required headset checks are Halo 4's actual weapon reticle on the VR crosshair,
no visor fragment, pause remains fixed, and ODST hooks and presents in 3D.
