# Stage 3G - Halo 2 Shader Draw Proof + Native Crosshair Handoff

## Purpose

Stage 3G keeps the Stage 3F Halo 2 HUD shader-draw ownership proof unchanged and
adds one narrowly-scoped crosshair experiment requested after Stage 3F.

The proven Halo 2 crosshair pixel-shader role is now handled at the same **actual
Draw/DrawIndexed boundary** used by Stage 3F, with the rejected Stage 3E CHUD-TLS
conjunction still bypassed.

## Crosshair behavior

When the exact registered Halo 2 crosshair shader is bound on the live draw:

1. `VR_BeginAuthoredReticleCapture()` redirects the authored draw into the
   existing reticle capture target.
2. The native draw executes normally through the existing Draw/DrawIndexed hook.
3. `VR_EndAuthoredReticleCapture()` restores render state and marks this Halo 2
   module generation as having completed a native crosshair capture.
4. Existing `Game_TitleCapturesAuthoredCrosshair()` ownership then makes the old
   procedural VR reticle opacity `0.0f`; the native weapon-specific art owns the
   same OpenXR aim quad instead.

Crucially, the procedural marker is **not hidden before a real native crosshair
shader draw completes**. This preserves a visible fail-open fallback during load,
death, or if the native crosshair path never arrives.

## HUD behavior retained from Stage 3F

Known Halo 2 gameplay-HUD shader draws continue to receive the deliberate 50%
centered viewport proof transform on their actual D3D11 context, with exact
viewport restoration immediately after each draw.

This remains an ownership/proof transform, not final HUD slider behavior.

## Binary reproduction

`tools/build_stage3g_h2_native_crosshair.py` takes the exact Stage 3F DLL and
re-enables the already-compiled native-crosshair capture call at RVA `0xBE78`.
No other Stage 3F byte is changed.

## Headset acceptance for this test

- Stage 3F HUD ownership telemetry remains active.
- Once a native Halo 2 crosshair draw occurs, the generic/procedural VR marker
  should disappear and the active weapon's authored Halo 2 crosshair should be
  shown on the existing controller-aim OpenXR quad.
- Weapon switching should change authored crosshair art through the existing
  bounded Halo 2 refresh path.
- If native capture does not occur, the procedural reticle must remain visible.
- Pause/resume, stereo, 6DOF, hands, weapon aim, renderer switching, and other
  titles must remain unchanged.
