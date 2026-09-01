# Stage 3BU - Halo 4 scene-target write-back (desktop mirror + damage black)

**Cumulative DLL:** `built/Stage3BU-HaloMCCVR.dll`
**SHA-256:** `5905afce6cc06996dd0c379d9cb1c6731091fe2d7f7a74051f6ae82004f3ac03`
**Chain:** 3BT `4a197073` -> 3BU. One behavioral change.
**User report (2026-08-30):** in Halo 4 the desktop mirror is black, and the
screen goes black when taking damage; "these bugs are very closely related."

## Root cause (one starvation, two symptoms)

Halo 4 stereo redirects every OM bind of the learned scene-colour RTV
(`g_sceneColorRtv`, the engine's final composited 3786x2730 fmt-28 target)
into the mod's per-eye caches (`VR_RedirectRenderTargets`, pointer-only
steady state). On every claimed frame the engine's REAL scene texture
receives nothing:

1. **Black desktop mirror** - the engine's present pass samples the real
   scene texture into the backbuffer; it samples black. (Pause frames are
   unclaimed/stock, which is why the pause menu still shows - and why the
   17:00 log's only "stock window" spikes are the user's own pauses.)
2. **Black screen on damage** - the damage screen-effect chain READS the
   real scene texture as its input and composites its output back into the
   (redirected) scene target: vignette over black = a black eye image for
   the duration of the effect. Identical mechanism, read side.

Evidence: 16:57 Steam session - `game swapchain: 3786x2730 fmt 28`;
`SCENEPROBE`/`M2 RASTER` learning lines (learned RTV 3786x2730 fmt=28
viewfmt=28, same size/format family as the backbuffer); zero mid-gameplay
stock windows or XR drops (so the damage black is inside the submitted eye
images, not a presentation fallback).

## The change

The steady-state branch `jne 0x2C3E5` at **0x2C2BC** inside `VR_EndRasterEye`
(taken exactly when eye >= 0 AND title == Halo 4 AND the scene RTV is
latched) is re-pointed to `s3bu_writeback` (129 bytes at 0x2F9D10, limit
0x2F9E10):

- `eye = g_rasterEye [0x2420A8]` (defensive `<= 1` check),
- `src = g_eyeCache[eye] [0x2AE808]`, `rtv = g_sceneColorRtv [0x2AEAF0]`,
- `rtv->GetResource(&dst)` (vtbl+0x38), `g_context [0x2AE298]->
  CopyResource(dst, src)` (vtbl+0x178), `dst->Release()` (vtbl+0x10),
- `jmp 0x2C3E5` (the original destination).

The learning path (RTV not yet latched) falls through unchanged; every
null pointer skips the copy. Stack-only writes, no allocation, no logging -
one GPU CopyResource per eye (~41 MB, ~10 GB/s at 120 fps, far below the
H3 ghosting fix's per-frame copies). COM slots verified against the mod's
own hook indices (contextVtbl[47] = CopyResource, contextVtbl[33] =
OMSetRenderTargets); globals disassembly-verified in the 3BT image
(g_context additionally confirmed by 18 references inside Blit).

## Expected result

- The desktop window shows live Halo 4 gameplay (the last-rendered eye).
- Taking damage shows the proper red damage vignette over the world instead
  of a black flash. (During the flash the effect's input can be one eye
  stale - at worst a brief softening, never black.)
- Other titles byte-identical; H4 pause/menu frames (unclaimed) untouched;
  the 3BI/3BP reticle capture runs inside the eye, before the write-back.

## Test

`python tools/test_stage3bu_h4_scene_writeback.py` - PASS: byte identity
vs 3BT outside the 6-byte splice + payload; splice decodes to the thunk;
thunk reads ONLY the four globals, three COM calls in order
(0x38/0x178/0x10), `cmp eax,1`/`ja` guard, stack-only writes, resumes at
0x2C3E5; learning path and 3BQ..3BT artifacts intact; 3BJ absent.

## Deployment

Installed 2026-08-30 into both editions (MCC confirmed closed), all three
hashes `5905afce...`; Stage 3BT preserved with the 16:57 log under
`out/deploy-backups/2026-08-30-pre-3BU/{steam,xbox}`.
