# Stage 3AS — pause-resume freeze fix, visor filter v2, reticle ink probe

Input: Stage 3AR, SHA-256
`bd6a8eb33bbb9ca82686b41730c2949a5b1e5fbccef0ad3a8441e698aff15496`.

Output: 2,919,424 bytes (unchanged geometry), SHA-256
`311036c85a491fb9b65f3fe4ad17e1b331d03bb2bf7450e641c3142d5ffd1058`.

## What the 2026-08-28 07:38 Steam headset session proved

1. **Stage 3AQ worked.** `C-H4-48` reported **496 exact capture OM reroutes**
   (previously 0). The capture reroute fires.
2. **The captures still measure blank** — `art 0, blankHeld 199`. `art` is the
   summed **alpha** of an **8×8 mip** of the 512×512 capture
   (`MeasureAuthoredReticleCoverage`, probe size 8, alpha channel only). A
   thin reticle's alpha can average to zero at that mip even when present, and
   Halo 4's HUD blend may not write destination alpha at all. Which of the
   three (no draws / no alpha / mip-averaged away) is unknown — this build
   measures it.
3. **The pause-resume "black screen" is a game freeze**, on record:
   `Runtime mode: gameplay -> loading` at 07:38:28.646, then
   `STALL: the game has not presented for 1000ms`. Cause found in the
   disassembly: the Stage 3AQ admit path re-enters `VR_RedirectRenderTargets`
   with the eye unvalidated. The body re-checks `g_reticleCaptureState.active`;
   a teardown racing that check (exactly what the mode transition does) falls
   through to the stock path, which reloads the eye spill `[rsp+0x44]`,
   `cdqe`-sign-extends it and indexes `g_eyeCacheRtvs` — eye `-1` reads the
   qword before the array and binds garbage as a render target.
4. **The Stage 3AR visor filter ran and dropped nothing visible, and the visor
   still drew.** The page bytes and both splices verify instruction-perfect;
   `.s3qd` is RWX; the gameplay histogram carried `26/27/20`. Conclusion: the
   visor is NOT command `0x20` inside a parallax bracket of the hooked
   gameplay root. Supporting evidence: this session showed **three** brackets
   per gameplay pass (`26x1125` against 376 pass-equivalents), not the two the
   visor theory predicted, and `docs/HALO4-CUI-EVIDENCE.md` shows every weapon
   reticle screen carries its own `parallax_container` — brackets are not
   visor-only. The visor also does not follow the HUD sliders, which transform
   only the gameplay root — so it very likely draws in another CUI root
   (overlay/auxiliary), which the old filter deliberately never touched.

## The three changes

**1. `0x2F9000` page rewritten** (`stage3as_eye_gate.S`): the admitted Halo 4
capture path now clamps an out-of-range eye to 0 in both `r9d` and the spill
slot `[rsp+0x44]` before continuing. In the normal admitted flow the capture
block returns before the eye is used; in the teardown race the clamp turns a
garbage RTV bind into one benign eye-0 cache bind. All other flows execute the
exact original `cmp r9d,1 / ja` pair.

**2. `0x2FA000` page rewritten** (`stage3as_page.S`): the visor drop now also
covers `0x20` inside `0x26/0x27` brackets of **non-gameplay** CUI roots, with
a separate bracket depth per context, and counts what it sees:

```
S3AS ngp CUI: c26 %u c20 %u c20br %u drops %u gpDrops %u c22 %u
```

logged every 262,144 dispatched commands (~4 s in gameplay). The gameplay-root
behavior is unchanged from Stage 3AR. `halo4_visor = 1` still disables every
drop. Config key parsing and both splice entry offsets (+0x10, +0x88) are
unchanged. If Halo 4's own menus lose art with the visor hidden, set
`halo4_visor = 1` — that restores fully stock dispatch.

**3. New 5-byte splice at `0x1DD8B`** redirects the probe's 8×8 pixel loop
into the page. It computes the same alpha ink (returned in `edi`, `r8` zeroed
for the Unmap tail, behavior identical) **and** the RGB sum, and logs

```
S3AS reticle probe: alpha %u rgb %u n %u
```

every 64th measure. Next session's log then decides the reticle plan:

* `alpha 0, rgb 0` — the rerouted draws never land; suspect the reroute
  window or the learned scene target (`g_sceneColorRtv`, 0x2AEAF0).
* `alpha 0, rgb > 0` — draws land, Halo 4's HUD blend writes no destination
  alpha; the fix is forcing an alpha-writing blend during capture (or a blit
  that derives alpha), then lifting the C-H4-50 procedural-fallback gates.
* `alpha > 0` — the 8×8 alpha-only measure was the blocker; re-point the
  guard and lift the fallback gates.

## Scope

Halo 4 only, as before: the eye-gate page is behind `GameTitle::Halo4` +
capture-active; the CUI filter lives in the Halo 4-only detour; the probe
helper changes no behavior for any title (identical alpha sum, identical
register contract — the shared function only gains a diagnostic log line).

## Reproduction

```text
py -3 tools/build_stage3as_h4_fixes.py \
    built/Stage3AR-HaloMCCVR.dll built/Stage3AS-HaloMCCVR.dll
```

The builder refuses any input but the exact Stage 3AR SHA-256, guards nine
byte sequences (the probe splice and its resume tail, the 3AQ splice, the eye
spill/reload/`cdqe` sites, both page entry splices), asserts the page entry
offsets, and decodes both emitted payloads with capstone, asserting every
external rip/branch target. Payloads are assembled by `tools/postlink.py`
(clang integrated assembler; `clang-23` from the conda pkgs cache works —
add its `Library/bin` and `libllvm23`'s to PATH).

## Deployed

2026-08-28, both editions, SHA-256
`311036c85a491fb9b65f3fe4ad17e1b331d03bb2bf7450e641c3142d5ffd1058`; previous
DLL/cfg preserved under `out/deploy-backups/before-stage3as-20260828-081353`.
Both `halomccvr.cfg` files gained a documented `halo4_visor = 0` block
(appended; F1-menu saves will drop it — default stays hidden).
