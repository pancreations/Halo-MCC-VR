# Stage 3AV — Halo 4 menu-resume pause exit + capture depth-target fix

Input: Stage 3AU, SHA-256
`6365ac47ea7d2d7e934ba7f7bdd74bcb6c2ada1293697fb8fc2cfe52ec9d8d67`.

Output: 2,919,424 bytes (geometry unchanged), SHA-256
`2baf0a3e7d654a0cda701399d672cec7c582202e138537f8193e34e5d72aca16`.

Stage 3AU was hash-verified as installed on both editions before the 09:10
test, so the 09:10 log measures Stage 3AU exactly. Both of its changes were
wrong about the cause, and the log says why in both cases.

## 1. The pause black screen — Stage 3AU fixed the wrong pause

Stage 3AU assumed the trailing **Y** of the Y+B chord re-paused the game after
a B-edge exit. The 09:10 log shows the session never went down that path:

```
09:10:18.730  controller edge: Y
09:10:18.751  controller edge: B
09:10:18.751  pause fallback: injecting Start, target=head-locked 2D
09:10:18.956  pause transition: presentation switched to head-locked 2D
09:10:20.132  controller edge: A          <- "Resume Game" in the pause menu
```

There is no `Halo 4 pause exit` line anywhere in the file, and no second
Start injection either — so Stage 3AU's 400 ms grace was never even consulted.
The player entered pause with Y+B and left it by selecting **Resume Game with
A**, which produces no B edge at all. Stage 3X's raw B edge was the *only*
Halo 4 pause-exit detection in the build, so nothing ever asked for stereo
back. Presentation stayed head-locked 2D for the rest of the session while
`fps 112 (stereo on)` shows the mod still rendering stereo. That mismatch is
the black screen.

**Fix — modelled on the accepted C-H2-73 clock proof, not on button guessing.**
Halo 2 solved this identical problem by proving the engine's own clock froze
and resumed. `halo4.dll` carries a **`game_paused`** boolean in the same
debug-var table this mod already resolves `enable_first_person_squish` from
(offline dump: entry at `halo4+0xE83388`, type 5, exactly the type the squish
resolve requires). Stage 3AV:

* resolves it **by name**, once, through the DLL's own `FindDebugVarSlot`
  (`0x41E10` — proven by disassembly to be the function the C-H4-30 squish
  code calls, with the same `edx = 0x4A3F000` image size), while the Stage
  3V/3X loader-pinned `halo4.dll` reference is live; the pointer is cleared
  when that reference drops;
* on each **validated H4 camera callback** (the Stage 3X heartbeat edge),
  while the head-locked pause target is active: first requires the flag to
  actually read **nonzero**, which proves its polarity for this pause, then
  treats **8 consecutive zero reads** as the native resume;
* restores stereo exactly the way the Stage 3X B-edge wrapper does —
  `VR_RequestPausePresentation(false)` plus the same resume-grace stamp at
  `0x2F31C8` that the 3AU toggle gate and the 3X poll gate already honour.

If the debug var is not live in this build, one log line says so and the
behaviour is byte-identical to Stage 3AU. The B-edge path is untouched, so
leaving pause with B still works as before.

## 2. The crosshair — the capture had a depth target bound

Stage 3AU widened the capture viewport from `{2048,2048}` to
`{7572,4258.068}`. The log proves the new framing reached the engine —
`offscreen hide 7572.000/0.000` — and the ink did not move: the probe still
measured **`alpha 0 rgb 0`**, at both framings, with **0 write failures**.
A framing error cannot produce that. Nothing is landing in the texture at all.

**Cause.** `OMSetRenderTargetsHook` forwards the game's depth-stencil view
unchanged:

```cpp
g_origOMSetRenderTargets(context, count, redirected, dsv);   // d3d11_hook.cpp
```

During an active capture the reroute substitutes the private **512×512**
authored target for the learned **3786×2730** scene target — but if that same
engine bind carries its full-size DSV, D3D11 sees an RTV/DSV dimension
mismatch and **silently drops every draw**, with no failure the mod can see.
The capture's own opening bind uses a null DSV
(`BeginAuthoredReticleCaptureInternal`), which is why the one non-blank
capture ever recorded (`art 4263`) came from a pass that flushed before any
depth-carrying rebind — and it came from the pause menu, which is the "random
element on the VR crosshair" from the headset report.

**Fix:** the hook's argument-restore tail is spliced. The forwarded DSV
becomes null **only** when the reroute actually rewrote the bind, the capture
is active, and the active title is Halo 4. A one-time log line proves the case
occurs at all. Every other bind in every title forwards byte-identically.

## What the next log should show

* `S3AV: halo4 game_paused resolved at halo4+0x…` — the pause fix is armed.
  (If it says `not live in this build`, the pause behaviour is unchanged from
  3AU and that needs a different lever.)
* `S3AV: H4 capture rebind carried a full-size depth target` — confirms the
  depth mismatch was real.
* `S3AS reticle probe: alpha N rgb M` with **non-zero** values in gameplay.
* On resume from the pause menu with A:
  `Halo 4 pause presentation: native game_paused cleared; restoring stereo 3D`
  followed by `pause transition: … -> stereo 3D`.

## Scope

Halo 4 only. The pause monitor runs off the Halo 4 camera heartbeat and is
gated on the Halo 4 module reference; the DSV gate is gated on
`TitleAdapter_GetActiveTitle() == Halo4` **and** an active capture. Halo 3,
ODST, Reach and Halo 2 take byte-identical paths. The Stage 3AT
CREDIT + ODST-unpin bytes are inherited untouched.

## Reproduction

```text
py -3 tools/build_stage3av_h4_pause_flag_and_dsv.py \
    built/Stage3AU-HaloMCCVR.dll built/Stage3AV-HaloMCCVR.dll
```

The builder refuses any input but the exact Stage 3AU SHA-256; guards all
three splice sites plus six surrounding sequences (including the
`FindDebugVarSlot` prologue and the squish call site that proves its ABI);
**reads** both Stage 3X continuation addresses out of the input's own
displacements rather than hardcoding them, and range-checks them into the
Stage 3X helper page; asserts the payload lands in genuinely zeroed page tail
and that its state block assembles to zero bytes; and decodes the emitted code
with capstone, asserting all eleven external targets.

## Deployment

**INSTALLED 2026-08-28 10:13** to both editions, hash-verified after the copy:

| Edition | Path | SHA-256 |
|---|---|---|
| Steam | `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR\HaloMCCVR.dll` | `2baf0a3e…` |
| Microsoft Store | `N:\XBOX\Halo- The Master Chief Collection\Content\Halo_MCC_VR\HaloMCCVR.dll` | `2baf0a3e…` |

MCC was confirmed closed first. The previous Stage 3AU DLLs are preserved at
`out/deploy-backups/2026-08-28-pre-3AV/`. No `halomccvr.cfg` was touched and the
game was not launched.
