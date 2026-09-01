# Stage 3BE — pin the Halo 4 capture framing at every captured draw

Status: **installed to both editions 2026-08-28, awaiting headset result.**

- Input Stage 3BD SHA-256:
  `2E19F93AF12F8538F37BB7375A5FAA8BF0210B74697A87CCCC5D29C637068422`
- Output Stage 3BE SHA-256:
  `741A0E69E848B2CC49113E4A21E555BFD6CAC78F7A482B623E213EE72D48EC8A`
- Size unchanged at 2,923,520 bytes; 12 PE sections; changed bytes: two 4-byte
  call displacements (0xD0C6, 0xD22A) + 370-byte payload at 0x2FB810.
- Previous install preserved under `out/deploy-backups/2026-08-28-pre-3BE/`
  (both editions, both hashing `2E19F93A…`).

## The evidence that forced this design

Stage 3BD quadrupled the capture viewport (512 → 2048, centred). The headset
result: **"some other random element … IT DIDNT SHRINK."** A 4× framing change
that produces zero size change on the captured art is proof that the framing
this mod sets is *not* the framing the captured draws actually execute with.

The shipped binary already re-asserts viewport+scissor after every mid-capture
scene-target rebind (`VR_RedirectRenderTargets`, 0x2FFDE–0x30010; measured
"9 exact capture OM reroutes / 3 captures") — and the art is *still* framed
wrong. Therefore the engine sets its own viewport AFTER that re-assert, in the
gap before the batched CUI draws flush. SCENEPROBE had already measured
exactly this: the same learned scene RTV bound with a 947×683 viewport at one
moment and a full-raster viewport at another. Halo 3/ODST never hit this
because their capture brackets one widget-scoped draw — there is no gap.

This also explains the entire "random element" series: with the engine's
raster-sized viewport live, the little 512×512 capture texture receives
whatever corner/slice of the HUD the engine's framing maps onto it —
one arbitrary element, varying run to run, immune to every container
selection and framing constant we shipped.

## The change

The only point the engine cannot race is the draw itself. The DLL already
owns production MinHook detours on `ID3D11DeviceContext::Draw`
(`Halo2DrawCensusHook`, 0xD0A0) and `::DrawIndexed`
(`Halo2DrawIndexedCensusHook`, 0xD200). Both begin with a 5-byte
`call TitleAdapter_GetActiveTitle` (0x879C0) after saving the context in rbx.

Stage 3BE retargets those two calls to `stage3be_draw_gate` (0x2FB818), which
tail-jumps into the real title adapter (so the Halo 2 census logic sees an
unchanged world) and, before the draw runs, checks in order:

1. `g_reticleCaptureState.active` (0x2AE770) — else 1-compare bail-out;
2. `framingCaptured` (0x2AE79C);
3. active title byte == 4 (Halo 4 only, 0x2BA6C8);
4. the context is the game's immediate context (0x2AE298);
5. `OMGetRenderTargets` says the **currently bound RTV is the capture
   target** (authored 0x2AE448 / discard 0x2AE458 per `publishesAuthored`).

Only when all five hold, it re-asserts `RSSetViewports(1, &captureViewport)`
and `RSSetScissorRects(1, &captureScissor)` — the exact saved values Begin
stored at 0x2AE774/0x2AE78C, which under Stage 3BD are the centred 4× framing
{-768, -768, 2048, 2048}. Nothing can intervene between this and the draw, so
**every captured draw executes with the intended centred 4× zoom**, no matter
what the engine set in between. Draws to the engine's own sub-targets
(atlases etc.) are untouched by check 5.

Every referenced global and vtable offset was read out of the Stage 3BD
disassembly this session (Begin at 0x11B60–0x11E50, reroute at 0x2FF38–0x30010,
census hooks at 0xD0A0/0xD200, installer at 0xD61F–0xD70x) — nothing copied
from source or another title.

## Cost and fail-safety

- Idle cost, every title, every draw: one byte compare + branch.
  Deterministic, allocation-free. The OMGet/Release pair runs only while a
  Halo 4 capture is live (bounded sample frames) and adds only ref-count
  traffic.
- H3/ODST/Reach: title byte ≠ 4 → their captures behave byte-identically.
- Halo 2: the census hooks' own logic is unchanged (the gate returns the
  title in al via the tail jump exactly as before).
- If the capture target is never bound at a draw, behaviour is exactly
  Stage 3BD. Reverting the whole stage = restoring the two call
  displacements to 0x879C0.
- One-time log line `S3BE: H4 captured draw reached with the capture target
  bound…` proves the gate actually fired.

## Verification

- `tools/test_stage3be_h4_capture_viewport_pin.py`: PASS — decodes the welded
  gate from the output bytes (every guard target, both vtable call offsets,
  the tail jump), asserts both splices, the intact Stage 3BD identity
  (private 2048 constant, retargeted movdqa, capture edge, untouched shared
  constant), CREDIT/ODST bytes, and that **no byte outside the three intended
  regions changed**.
- `tools/test_stage3bd_h4_capture_zoom.py`: PASS (unchanged).
- Toolchain: `tools/postlink.py` + conda-cache clang 23.1.0
  (`HALOMCCVR_CLANG=…\anaconda3\pkgs\clang-23-23.1.0-…\clang-23.exe`).

## Headset oracle

1. **Halo 4:** the VR crosshair should now show Halo 4's own reticle art —
   the four corner arcs + four ticks of the real reticle — at a sensible
   size, centred, instead of an arbitrary HUD element.
2. If it is now recognisably the reticle but too large/small or off-centre,
   that is a ratio/offset adjustment of the 0x2FB800 constant — small change.
3. If it is the reticle **plus** other centre-screen art layered on it, the
   framing is fixed and the remaining problem is isolation of centre-screen
   HUD elements — a different, known follow-up.
4. If it is STILL an arbitrary element of unchanged size, the log's `S3BE`
   line decides: absent → the gate never saw the capture target bound at a
   draw (the draws land some other way — next step is the BMP dump stage);
   present → the framing is pinned and the content itself is wrong.
5. Halo 3 / ODST / Reach / Halo 2: must be unchanged (regression check).
