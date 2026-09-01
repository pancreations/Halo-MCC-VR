# Stages 3BR / 3BS / 3BT - size, animation, and rolling snapshots

**Cumulative DLL:** `built/Stage3BT-HaloMCCVR.dll`
**SHA-256:** `4a1970734c5266688d2c61490fc485f74cbd3d39a3ffc568d0832791ada2279a`
**Chain:** 3BQ `c9d95aa9` -> 3BR `767e8816` -> 3BS `51d1feb5` -> 3BT `4a197073`.
**Headset result that motivated them (Steam, 2026-08-30 16:20, Stage 3BQ):**
the Halo 4 reticle finally rides the VR crosshair (user screenshot), but it
is too small, "loses updates", and shows "other elements attached".

## 3BR - capture drawn 2.5x larger (one behavioral change)

The reticle occupied 73 px of the 512 quad (~14%). The gate call at 0x2FB992
now targets `s3br_scaled_viewport` (368 bytes at 0x2F9A90): exactly the 3BN
computation (raster-aspect Height, biased TopLeftY, 3BK fallback), then
`W *= 2.5; H *= 2.5; TL = 256 - 2.5*(256 - TL)` for both axes - scaling the
artwork about the texel the reticle centres on. Expected ~183 px (~36% of the
quad, ~2.6 deg at the configured 7.2 deg quad). The scale constant is
`s3br_kscale` (0x40200000 = 2.5f) in the payload data if a different size is
requested.

## 3BS - Halo 4 never freezes behind the half-ink hold (one change)

The 16:20 log: after firing, ink jumps ~97 -> ~1000; the guard then holds the
calm 97-ink reticle as "blank" (`required = goodInk/2`, `blankHeld 2..4`,
whole windows with 0 uploads) - the reported "losing updates". The bar
assumes the identity key changes on a weapon swap; Halo 4 publishes fixed
key 1, so `identityChanged` never fires and a widget whose ink legitimately
swings >10x (bloom, red = 45) is misjudged. The 13 bytes at 0x2A701
(`movzx esi,dl` + displaced global load + `movzx ebp,cl`) call
`s3bs_h4_identity`: for the active title Halo 4, esi (identityChanged) := 1 -
the exact treatment a weapon swap already receives (required = 0, holds
cleared). Blank captures are still refused by the >= 2 ink bar; other titles
bit-for-bit unchanged.

Note: the user's config has `crosshair_animation_frames = 60` (slowest legal;
default 6). The mod never edits an existing halomccvr.cfg; the user should
set 6 for smooth bloom animation.

## 3BT - the 3BM snapshots roll all session (instrument)

The 4 raw dumps fired only in the first ~8 s, so the "other elements" seen
later were never captured. The 9 bytes at 0x2F9066 in the 3BM payload
(`cmp eax,4; ja skip`) call `s3bt_wrap`: the counter wraps 4 -> 1 and the
tick never stops, so `HaloMCCVR-h4-reticle-1..4.raw` always hold the LAST
four snapshots (~8 s) of the session. Next run, whatever is on the crosshair
when the session ends is on disk.

## Test

`python tools/test_stage3bt_h4_rolling_dumps.py` - PASS: byte identity vs 3BQ
outside the five regions, all three thunks decoded and checked (private
viewport only, stored struct never written; esi/rax/ebp only; counter wrap),
artifacts intact, 3BJ absent.

## Deployment

Installed 2026-08-30 into both editions, hash-verified `4a197073...`; 3BQ
preserved under `out/deploy-backups/2026-08-30-pre-3BT/{steam,xbox}` with the
16:20 log and dumps.

## Open

The "other elements attached" are unidentified: the early dumps hold only the
clean reticle. The rolling dumps + the next headset session will show them;
candidates are `hit_indicator_art` (in the weapon container by design) and a
polyart that somehow precedes its container's 0x20/0x1F marker.
