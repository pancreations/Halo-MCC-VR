# Stage 3BD — Halo 4 authored capture 4x centre zoom (H4EK-derived)

Status: **HEADSET PENDING**.

- Input Stage 3BB SHA-256: `10E39CF6…722A8192`
- Output Stage 3BD SHA-256:
  `2E19F93AF12F8538F37BB7375A5FAA8BF0210B74697A87CCCC5D29C637068422`
- 2,923,520 bytes, 12 PE sections, **8 bytes changed**.

## Measured from the official H4EK tags

`tools/export_h4_kit.ps1` (18/18 exports valid) produced
`out/h4-kit-source/canonical/assault_rifle.cui_screen.xml`. Its reticle
subtree is `reticule_offset_container > reticule_art_container >
reticule_art_color`, whose eight bitmap leaves are authored as:

| widget | left | top | size | bitmap |
|---|---:|---:|---|---|
| quarter_circle_top_left | -37 | -37 | 32x32 | `ui\hud\reticles\unsc_assaultrifle\ar_corner` |
| quarter_circle_top_right | +5 | -37 | 32x32 | same |
| quarter_circle_bottom_left | -37 | +5 | 32x32 | same |
| quarter_circle_bottom_right | +5 | +5 | 32x32 | same |
| reticule_tick_top / bottom | -1 | -9 / +1 | 2.2x8.2 | `ui\cui\common\white_pix` |
| reticule_tick_left / right | -9 / +1 | -1 | 8.2x2.2 | same |

**The reticle spans -37..+37 — about 74 authored units, centred on its
container's origin.** Sibling HUD widgets sit near (516, 275), so the virtual
screen is roughly 1030x550 units.

## The defect

Read out of the shipped binary at `0x11C77`, inside the `cmp al,4 / jne`
Halo 4 branch:

```
movdqa xmm1, [0x1BF0C0]      ; {512.0, 512.0, 0.0, 1.0}
mulss  xmm1, xmm4            ; Width  *= authoredCaptureScale (1.0)
subss  xmm0, xmm1            ; 512 - Width
mulss  xmm0, 0.5             ; TopLeftX = (512 - Width) / 2
```

For Halo 4 this yields viewport `{0, 0, 512, 512}` — offset zero, exactly the
size of the capture texture. **A viewport maps the full NDC range onto its
rect, so this crops nothing; it scales Halo 4's entire HUD into the 512x512
reticle texture.** That is the reported symptom exactly, and it is why
selecting a different CUI container (Stage 3BC) could never change the
captured content.

Halo 3 and ODST zoom instead — an oversized viewport with a matching negative
offset, so only the centre lands in the texture. `src/dll/vr.cpp` documents
the same intent for Halo 4 ("Halo 4 now uses that SAME proven 4x ratio"); the
shipped bytes never implemented it.

## The change

The constant at `0x1BF0C0` has **two** referents (`0x11C7B` here and
`0x152C1F` elsewhere), so it must not be edited in place. Instead a private
`{2048, 2048, 0, 1}` constant is written to the free, 16-byte-aligned tail of
`.s3qd` at `0x2FB800`, and only the Halo 4 branch's rip-relative displacement
at `0x11C7B` is retargeted. Elements 2 and 3 stay `0.0`/`1.0` because the same
`movups` seeds the viewport's MinDepth/MaxDepth.

Resulting Halo 4 capture viewport: `{-768, -768, 2048, 2048}` — a 4x centre
zoom, identical in form to Halo 3/ODST. The 74-unit reticle then covers
`74 / 550 * 2048 ≈ 276` of the texture's 512 pixels, the same occupancy those
titles already ship.

Also restores the Stage 3AX capture edge at `0x53634` (`EB`→`74`), which
Stage 3BB had forced to the normal pass; without it no capture runs at all.

**Not included:** Stage 3BC's payload-X container discriminator, which was
shown to be irrelevant to the captured content.

## Verification

- `tools/test_stage3bd_h4_capture_zoom.py`: PASS — asserts the Halo 4 branch
  guard, the retargeted movdqa and its 16-byte alignment, the 4x ratio and
  -768 offset, the untouched shared constant *and its other referent*, the
  restored capture edge, CREDIT/ODST bytes, and that the non-Halo-4 framing
  path (`0x11C8D`) and shared scale math (`0x11D0A`) are byte-identical.
- `tools/test_stage3bb_h4_native_hide_no_replay.py`: PASS.

## Deployment

Installed to both editions 2026-08-28 with no MCC process running; both
verify to `2E19F93A…`. Prior Stage 3BB preserved at
`out/deploy-backups/2026-08-28-pre-3BC/` and in `built/`.

## Headset oracle

1. **Halo 4:** the VR crosshair shows Halo 4's own reticle art at a sensible
   size, not the whole HUD shrunk down.
2. If it is now recognisably the reticle but too large/small or off-centre,
   that is a *ratio* adjustment (the 4x constant) — a much smaller change.
3. Halo 3 / ODST / Reach: unchanged, byte-identical framing paths.
