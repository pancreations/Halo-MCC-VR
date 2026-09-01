# Stage 3BQ - reticle swapchain RTV creation works on typeless images

**DLL:** `built/Stage3BQ-HaloMCCVR.dll`
**SHA-256:** `c9d95aa9aabeb2ed07c232d292c3ab4bbcd91300c2d67edd6921aabef0078f5d`
**Base:** Stage 3BP `4298f4a7...`
**Headset result that motivated it (Steam, 2026-08-30 15:52, 3BP):** "the
procedural placeholder crosshair was flashing like crazy".

## Diagnosis, straight from the log

Every authored upload failed:

```
blit: missing shader-path resource (... rtv=0000000000000000)
authored reticle upload FAILED; the VR reticle is showing its procedural art
```

while the capture itself was perfect (all four raw dumps: the same stable
1485-texel AR reticle; `art 97, blankHeld 0`). `GetRtv` (vr.cpp 3263) creates
the swapchain image's render-target view lazily with a NULL desc; SteamVR's
OpenXR runtime hands out TYPELESS D3D11 swapchain textures, on which a
null-desc view creation fails - every attempt, forever. Nothing ever noticed
because no shipped path needed that view: eye and reticle copies take Blit's
FAST CopyResource path and the procedural crosshair paints via
`UpdateSubresource`. Stage 3BO's shader path is its first consumer. The
failed upload falls back to the procedural repaint, the next upload retries -
the two alternate, which is exactly the reported flashing (and the same
mechanism the EnsureReticleChain comments describe).

## The change

The inlined GetRtv create in the reticle upload (0x2A9DA: `mov rax,[rcx]` +
`call [rax+0x48]`, rcx=g_device, rdx=images[idx], r8=NULL desc, r9=&rtv)
becomes a call to `s3bq_create_rtv` (91 bytes at 0x2F9A30):

1. performs the exact original null-desc create; success returns unchanged;
2. on failure, reloads g_device (0x2AE290, null-guarded) and retries once
   with an explicit `D3D11_RENDER_TARGET_VIEW_DESC { Format =
   (DXGI_FORMAT)g_xrFormat (0x2AE2A0), ViewDimension = TEXTURE2D,
   Texture2D.MipSlice = 0 }` - valid on a typeless resource and on a
   concrete-format one alike.

The call site ignores the HRESULT and re-reads `rtvs[idx]`, so the retried
view flows through with no other change. Every title, both passes, the fade /
menu / screen chains: untouched (their GetRtv copies never mattered to date;
fixing them is a separate decision).

## Test

`python tools/test_stage3bq_h4_reticle_rtv_typeless.py` - PASS: byte identity
outside the 6-byte splice and payload, splice decode, first call exactly the
displaced pair with r8 untouched, jns, guarded reload, desc field-by-field,
rdx/r9 restore, same vtable slot retried, stack-only writes, no non-volatile
register writes, 3BP/3BO/3BN artifacts, 3BJ absent.

## Expected headset result

The flashing stops; the Halo 4 weapon reticle (AR: four arcs, blue, red over
enemies) rides the VR crosshair, alone. If it STILL fails, the log line
`blit: missing shader-path resource` reappearing means the retry also failed -
report the log; the capture side is already proven good.

## Deployment

Installed 2026-08-30 into both editions, hash-verified `c9d95aa9...`; 3BP
preserved under `out/deploy-backups/2026-08-30-pre-3BQ/{steam,xbox}` together
with the 3BP run's log and raw dumps.
