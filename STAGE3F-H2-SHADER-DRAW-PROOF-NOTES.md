# Stage 3F — Halo 2 actual shader-draw ownership proof

Stage 3F starts from rejected Stage 3E (`518aebe`) and changes only the Halo 2 HUD draw mutation boundary.

## Why Stage 3E was rejected

The headset result proved two signals independently but their intersection was zero:

- native gameplay CHUD scope at retail `halo2.dll+0x7FFD70` was live;
- known Toggle HUD / 3DMigoto gameplay-HUD and crosshair pixel-shader identities were created;
- **no registered shader draw occurred while the CPU CHUD TLS scope was active**.

Therefore Stage 3E's `Halo2NativeHud_GetRasterLayout()` gate was invalid as a D3D draw-ownership condition.

## Stage 3F behavior

For Halo 2 only, `BeginHalo2HudDraw` now:

1. executes on the real `ID3D11DeviceContext*` passed to the hooked `Draw` / `DrawIndexed` call;
2. asks that context for the bound pixel shader;
3. accepts only a shader object already classified by the Stage 3E exact gameplay-HUD hash registry;
4. saves that context's current viewports;
5. changes each viewport to **50% width/height, centered**;
6. lets the original draw execute;
7. relies on the already-existing `EndHalo2HudDraw` transaction to restore the exact original viewports and increment `rasterTransforms`.

This is intentionally a strong ownership proof, **not final HUD slider mapping**.

The native-crosshair capture is disabled for this pass because the Stage 3E capture helper uses the global immediate context and has not been proven safe when the actual HUD draw comes from a deferred/worker context. The procedural VR reticle remains the fallback.

## Binary provenance

The headset DLL is a deterministic post-link patch of the exact rejected Stage 3E DLL:

- Stage 3E input SHA-256: `95F3362DB179038DDBCD77FEA42AEA8A10D1DC086A40850D455301B17D786893`
- Stage 3F output SHA-256: `83867929BA3678DC302E36B1A92649C08AB44A59D1E85B040A6BE64D27D44C72`
- new RX section: `.h2sf`, RVA `0x002EE000`, virtual size `0xE8`

Three existing `.text` edits are made:

- `RVA 0x0000BDF6`: reject the old CHUD-TLS dependency by forcing the old boolean gate true;
- `RVA 0x0000BE78`: disable Stage 3E's context-unsafe native-crosshair capture for this proof;
- `RVA 0x0000BE97`: route the already-classified GameplayHud role to `.h2sf`, then return through the original mutation epilogue.

`tools/build_stage3f_h2_shader_draw_proof.py` reproduces the DLL from the exact Stage 3E input without a Windows compiler.

## Acceptance / interpretation

The existing log telemetry is deliberately reused.

- `rasterTransforms > 0` proves that a known gameplay-HUD shader reached the actual Draw/DrawIndexed hook and the proof viewport mutation executed.
- A visibly centered/shrunken HUD proves that these role draws directly own the visible HUD rasterization.
- `rasterTransforms > 0` with no headset HUD means the draws are late or target a surface not contained in the submitted eye caches; the next change must route/replay those exact draws into the two eye-cache RTVs on the same context.
- `rasterTransforms == 0` with `stateFailures > 0` means the role was recognized but viewport state could not be read on that context.
- `rasterTransforms == 0` and no new state failures means `PSGetShader` is not sufficient to recover the role at Draw time; next step is a `PSSetShader`-tracked per-context role, not another engine/TLS hypothesis.

Do not call this a HUD fix until headset acceptance.
