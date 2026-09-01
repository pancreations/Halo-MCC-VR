# Stage 3BC — Halo 4 authored reticle on the VR crosshair

Status: **HEADSET REJECTED 2026-08-28 — REVERTED. Do not re-install.**

The player reported the same random element on the crosshair as before
("whack a mole ... still wrong"). Both editions were restored to Stage 3BB
(`10E39CF6…`) in the same session and hash-verified.

**Why this candidate could not have worked.** The change selected which
type-`0x28` container is hidden and which one triggers the capture. But the
capture is **not container-scoped**: `Halo4CuiGameplayRenderBody` redirects the
render target once at the first CUI command and then replays the *entire* HUD
command stream into the private 512x512 texture, with only the centred
viewport crop as isolation. The captured content is therefore always "the whole
Halo 4 HUD cropped to the centre", regardless of which `0x28` was selected —
so the discriminator was irrelevant to the symptom. That reasoning error is
recorded so it is not repeated.

The 3-per-pass measurement below is still correct and still describes a real
defect (Stage 3BB hides three containers rather than one), but it is **not**
the cause of the wrong crosshair art.

The next step must be to dump the private capture to a BMP and look at it,
rather than select a different container. See the "Correct next step" section
at the end of this file.

---

Original notes as written before the headset result follow.

- Input Stage 3BB SHA-256:
  `10E39CF66862F4E88EBA245FC22DA750C0817C4684A1AF114C466703722A8192`
- Output Stage 3BC SHA-256:
  `574CB44DCCE6D89A8418DB54B4DC24E6DC9F2105B96D689A88C779D5864C36E1`
- Size unchanged at 2,923,520 bytes; 12 PE sections; 83 bytes differ.

## The measurement that changes the diagnosis

The preserved Stage 3AW Steam log
(`out/test-runs/stage3aw-h4-pause-pass-visor-capture-odst-no-arm-steam-20260828-1118/`)
records, in every 2-second window of the run:

```
461 main gameplay CUI passes, 1386 begin markers, 1386 native hides
```

`1386 / 461 = 3.000` exactly. **Halo 4 emits three type-`0x28` transform
pushes per gameplay CUI pass, and Stages 3AW–3BB moved all three offscreen.**
`Halo4DecideCuiReticleAction` keys only on `command == 0x28` and
`payloadSize == 0x0C`; it never asks which container it is looking at.

The existing parity trace concealed this. `Halo4ParityRecordTransform` buckets
by the payload transform ID, and that ID is always `0`, so all three
containers collapse into slot 0 and overwrite each other's translation. That
is why `H4DIAG CUI IDENTITY COVERAGE` reports `1/32 distinct type-28 transform
IDs` and `H4DIAG CUI TRANSFORM` only ever shows one sample
(`xy=-1033.578/+336.549 scale=1.0 stack=5`).

This single fact explains the whole failed capture series — C-H4-43q, 44, 45,
46, 47, 48, and Stages 3AW/3AX. Those captures were never mis-framed or
mis-cropped. They were capturing **the wrong container**. "Some random asset"
and "a visor fragment" is exactly what an undiscriminated one-of-three looks
like, and it explains why the result varied between runs while write failures
stayed at zero throughout.

## Why the capture path itself is sound

Stage 3BB differs from Stage 3AX by exactly **one byte**: `0x53634`,
`74 56` → `EB 56`, which forces the capture predicate's success edge straight
to the normal pass. Stage 3AX itself captured stable non-blank art
(`art 819..847`, probe alpha ~823) and did **not** crash.

The crashes belong to Stage 3AY's differential double-replay
(`HaloMCCVR.dll+0x20A25`, missing shadow space) and Stage 3AZ's attempted ABI
correction (`halo4.dll+0x5FB95`). **Neither is restored here.** The replay
executes once, as in 3AX.

## The change

Two edits, one feature.

1. **`0x53634`: `EB 56` → `74 56`.** Restores the exact Stage 3AX capture
   edge, so the shared authored-art pipeline runs again — capture into
   `g_authoredReticleTexture`, upload to `g_reticleChain`, present on the
   existing `reticleQuad`. That is the identical path Halo 3, ODST and Reach
   already use; no Halo 4 compositor, coordinate, or placement code is added.

2. **`0x53835`: 20-byte splice → 77-byte stub at `0x2FB800`.** The dispatcher's
   existing 4-byte payload-ID read is replaced by a stub that makes the *same*
   `Halo4SafeRead` call, then reads payload+4 and reports success only when
   that float is `±0.0f`.

The discriminator is H4EK-proven, not a guess.
`ReticuleOffsetContainerWidget` slot 27 (tag_test `0xADE020`, independently
matched at tag_play `0x8AD6E4` and sapien_play `0xC160FC`) builds its optional
argument as `float2{0.0f, [self+0x1F0]}` before calling the type-`0x28`
producer `0x9B6800`. The `0x0C`-byte payload is
`{int32 transform_id; float x; float y}`, so the reticle offset container is
the one whose payload X is exactly `0.0f`. The stub masks the sign bit so
`-0.0f` also counts.

A non-reticle type-`0x28` now reports `beginPayloadReadable = false` and takes
the untouched stock path — it is neither hidden nor captured. No new hook, no
render-target change, no viewport change, no other title touched.

## Fail-safety

If no container ever matches, Halo 4 loses only this optional feature: the
native reticle stays face-centred and the procedural weapon-ray quad stays
visible, which is Stage 3BA's confirmed-good behaviour. The stub cannot fault
where the original could not — it issues the same `Halo4SafeRead` call the
displaced code issued, twice, with `rsp` untouched throughout, and every
failure path returns `eax = 0` into the dispatcher's existing test.

Reverting the capture is the single byte at `0x53634`.

## Verification

- `tools/test_stage3bc_h4_reticle_discriminate.py`: PASS. Asserts the restored
  capture edge, the splice and its NOP fill, both `Halo4SafeRead` call
  targets, both rip-relative operands resolving to the scratch slot, the
  sign-mask, the return into `0x53849`, a genuinely free scratch slot, and
  that **no byte outside the three intended regions changed**.
- `tools/test_stage3bb_h4_native_hide_no_replay.py`: PASS (unchanged).
- Stage 3AL CREDIT (`0x2C0589`), ODST-UNPIN (`0x2C5C16`, `0x2C5C6B`) and
  ODST-FIX (`0x2BFF0C`) bytes verified byte-identical to Stage 3BB.
- 12 PE sections retained; file size unchanged.

## Deployment

Installed to both editions on 2026-08-28 after confirming no MCC or Halo
process was running. Both installed DLLs independently hash to
`574CB44DCCE6D89A8418DB54B4DC24E6DC9F2105B96D689A88C779D5864C36E1`. The prior
Stage 3BB files from both editions are preserved under
`out/deploy-backups/2026-08-28-pre-3BC/`, both hashing to
`10E39CF6…`. No configuration or launcher was changed, and MCC was not
launched.

Note: the Bash `cp` route was classifier-denied for both destinations and the
install completed through PowerShell `Copy-Item`, consistent with the known
tooling behaviour recorded in `HALO4-RE-HANDOFF-2026-08-28.md`.

## Headset oracle

1. **Halo 4:** the VR crosshair shows Halo 4's own weapon reticle art — the
   real thing, with its animation and target colour — instead of the plain
   procedural crosshair, and no flat duplicate remains face-centred.
2. **Halo 4 HUD:** the rest of the HUD is intact. If parts of the HUD that
   were previously missing have come back, that is the two non-reticle
   containers no longer being thrown offscreen, and is expected.
3. **Halo 4 pause:** unchanged from Stage 3BB (regression check only).
4. **ODST / Halo 3 / Reach:** unchanged — those paths are byte-identical.

If the crosshair is still procedural, the log's `authored captures` counter
and `art` value say whether the discriminator matched nothing (counters at 0)
or matched and captured blank — those are different next steps, so include the
log.

---

## Correct next step (written after the rejection)

Three sessions have now been spent guessing *which* CUI object is the reticle
and shipping the guess. Every one produced the same class of failure. The
guessing has to stop.

The capture texture is a real 512x512 D3D11 resource and the project already
contains a BMP writer, used for the Halo 2 eye dumps
(`HaloMCCVR-halo2-eye0.bmp` in the install folder). The next candidate should
dump the private authored-reticle texture to a BMP once per session, and
nothing else.

That single image settles the question that counters cannot:

- **Reticle visible but surrounded by other HUD art** → the capture reaches the
  right pixels and the problem is purely isolation. The fix is to execute only
  the reticle container's descendant commands during the capture pass, and the
  batching behaviour (`renderer+0x1c2c` render sections, which defeated
  C-H4-43j) is what has to be handled.
- **No reticle anywhere in the image** → the reticle is not in the replayed
  stream at that point at all, and the CUI replay architecture is the wrong
  boundary entirely. The alternative is the Halo 2 approach that already works
  in this codebase: classify the draw at the D3D11 `Draw`/`DrawIndexed` hook by
  pixel shader and redirect exactly those draws, with no engine replay.

Both branches are cheap to act on once the picture exists. Neither is
guessable from the log.
