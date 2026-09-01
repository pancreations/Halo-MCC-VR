# Stage 3BI - capture only the hidden reticle container

**DLL:** `built/Stage3BI-HaloMCCVR.dll`
**SHA-256:** `dad8a373ed30f3e31f42350fa85b575e2f7b146b8ba83d7f4cdf7333da653d22`
**Base:** Stage 3BH `80573ed9...` (headset result: health bar still on the
crosshair - the framing centre was already right; content selection was the
remaining defect).

## The user's requirement, verbatim

> "you somehow were able to hide it from the hud? that hidden element is
> what needs to be projected on the cross hair the actual reticle"

## The defect

Two selectors in `Halo4CuiRenderCommandBody` disagreed:

- Visible pass (PROVEN by headset): each type-0x28/payload-0xC begin gets
  its pushed transform shifted +4*halfWidth in x -> the reticle disappears
  from the flat HUD.
- Capture replay: "owns the whole CUI command stream" - no selection.
  Every HUD element replayed onto the 512x512 texture; the viewport crop
  keeps the health bar/objective (3BH full-coverage map proved it).

## The fix

One splice at `0x53921` (the capture replay's per-command `original()`
call: `mov rcx,rsi` + `call [rsp+0x30]` -> `call 0x2FBE40` + 2 nops).
Payload `s3bi_capture_select` (292 bytes at 0x2FBE40):

- forwards the call through `g_halo4OrigCuiRenderCommand` (0x2B9B18,
  null-guarded), preserving the result byte;
- ONLY while `scope.redirectActive` ([r15+0x3BA], sampled before the
  call) - a failed Begin means the replay is the user's one stock pass
  and is left untouched;
- reads the 4-byte command header through the SEH `Halo4SafeRead` thunk
  (0x56C10); 0x28+0xC sets an inside flag, 0x29 clears it;
- then enforces on the transform-stack top entry
  (renderer+0x878+(count-1)*0x34, x at +0x28), idempotently vs the
  threshold 2*halfWidth (= [0x2FB800]*0.5):
  - inside the reticle container: x must be on target (subtract
    4*halfWidth if an inherited composition carried the offset in);
  - outside: x must be offscreen (add 4*halfWidth - the exact proven
    hide shift, same axis, same magnitude).

Transforms are baked into vertex data at command time, which is why this
survives the render-section batching boundary (renderer+0x1c2c) that
defeated draw-level scoping (C-H4-43j / Stage 3BC).

Result: the capture texture receives exactly what the visible pass hides.
What disappears from the flat HUD is what appears on the VR crosshair.

## Expected log signature

- `Halo 4 reticle upload: ... art N` should drop from ~1700-2500 to a
  reticle-sized count (roughly low hundreds).
- The one-shot `S3BH map:` (still armed, next launch) should show a small
  centred figure and NO full-width bands.

## Oracle for the headset test

- **Reticle on the crosshair** -> done; only size/polish remains.
- **Crosshair blank / procedural** with `art` near zero -> the 0x28
  containers' content genuinely isn't reaching the capture stream; the
  render-section batching boundary is the next target, and the map
  licenses that conclusion.
- **Still shows HUD bands** -> the 0x26 parallax containers push content
  that draws under non-top entries; next step scopes those too.

## Test

`python tools/test_stage3bi_h4_capture_reticle_only.py` - PASS. Verifies
splice decode, gate ordering (sampled before / tested after), SafeRead
usage (no raw [r13] deref), same-flag set/clear, enforcement maths,
branch idempotence, result preservation, no nonvolatile writes, byte
identity outside 2 regions, 3BH artifacts + CREDIT/ODST + 12 sections.

## Deployment

Installed 2026-08-28 into both editions, each independently verified as
`dad8a373ed30f3e31f42350fa85b575e2f7b146b8ba83d7f4cdf7333da653d22`:

- Steam `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR`
- MS Store `N:\XBOX\Halo- The Master Chief Collection\Content\Halo_MCC_VR`

The prior 3BH installs are preserved under
`out/deploy-backups/2026-08-28-pre-3BI/`. MCC was confirmed closed; the
game was never launched and no `halomccvr.cfg` was touched.
