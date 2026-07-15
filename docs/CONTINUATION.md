# Continuation / handoff guide

Operational summary + current frontier. Read `CLAUDE.md` first (hard rules, who the user is).
`docs/PLAN.md` = milestones. `docs/RE-notes.md` = reverse-engineering findings.

**The user is not a programmer.** Claude writes all code; the user runs the game, tests in the
headset, reports back. Give exact click-by-click instructions. Explain jargon.

**Process rules learned the hard way — violating these costs real play sessions:**
- **No fallbacks, no tradeoff options, no degraded-mode defaults.** Ship one correct path.
  (An fps-halving "safety net" default cost a session and real trust.)
- **Understand offline first. One decisive build per headset session.** Do not ship theories.
- **Never regress fps or controls.** 120 fps + working controls is the floor, not a goal.

---

## KNOWN MAJOR BUG — left-eye ghosting (open, deprioritized 2026-07-14)

**Symptom:** trailing after-images of bright pixels (sparkles/canopy) in the **first-rendered
eye** only. Present with the head perfectly still. Also visible on the desktop mirror.
Flipping `right_eye_first` moves it to the other lens — that is the reliable repro.

**Status: ACCEPTED AND MOVED ON FROM at the user's direction.** Playable. Do not spend another
session on it without new evidence. ~14 fixes across 5 categories have failed; the catalogue
below exists so nobody repeats them.

**The only proven remover is BANNED:** a discarded warm-up render
(`[firstEye discard, firstEye, otherEye]`) removes it completely — and halves fps to 60,
because 3 world renders cannot fit a 120 Hz budget at any resolution (Halo's per-render cost is
~4 ms of CPU/engine work, nearly independent of pixels — measured: 2 renders @ 12.2 MP = 117 fps,
3 renders @ 10.9 MP = 60 fps). SteamVR then paces to 60. There is no 80 fps on a 120 Hz headset.

**The empirical rule that fits every result:** *whichever render crosses the frame boundary
absorbs the poisoned state, regardless of eye identity or camera anchoring.* No ordering trick
dodges it; only a discarded render absorbs it.

**DO NOT REPEAT — all falsified in headset tests:**
| Category | Attempts | Result |
|---|---|---|
| Ordering | fixed order; alternating parity (v5); post-anchor to first eye (v7/v9) | fixed → steady first-eye ghost; alternating → ghost flickers between eyes; anchoring → unchanged |
| Buffer isolation | fp16 ≤half-res set; widened full-res; R10G10B10A2; cross-pass discovery (v2); frame-level blanking (v3) | 0 targets promoted in every discovery variant; widened full-res was actively harmful (radial smears) |
| Copy substitution | learned scene-snapshot pairs, dst-only then dst+src | ghost persisted both times |
| Shader constants | UpdateSubresource/Map/Unmap census; exact-match matrix matcher | all uploads funnel through ONE call site (`halo3+0x2AF8C1`, ~100k/s) so caller-RVA cannot discriminate; matcher scored **zero hits** — the engine does not reuse our built matrices verbatim |
| Effects | sun-shaft (R16G16 occlusion target) neutralization | A/B checkbox made no difference |

Four independent probes came back **empty** (in-pass RTV writes, in-pass PS SRV reads, UAV
binds, frame-level RTV writes). Conclusion: the poison is not reachable by any bind-time hook.

**The one lead never tested** (offline, no headset needed — see RE-notes for addresses):
Halo's view struct holds **two** camera+matrix slots: `{camera@+0x08, derived@+0x98}` and an
identical pair at `{+0x158, +0x1E8}`. That is the classic *current view / previous view* layout
used for temporal reprojection — the shape of this bug. `RenderViewHook` (game.cpp) currently
sets **prev := current for both eyes every pass**, which would zero any reprojection delta;
those two memcpys arrived unexplained in `bd2254e` and were never justified. **Open question:**
is `{+0x158,+0x1E8}` previous-frame state or a secondary view? Settle it by finding the
engine's own writer of `view+0x158` (a per-frame current→prev copy proves it). If it is
prev-frame state, the fix is giving each eye its own memory of its own last frame — memcpys,
two renders, no fps cost. Related unfollowed note: RE-notes records the world render camera as
*"a sliding array/ring (render history for temporal effects)"* around `~0x468xxxx`.

---

## Current state: M0, M1, M2, M3 all working

- **M0** — inject, launch EAC-off, D3D11 Present hook, OpenXR session, in-headset ImGui menu (F1).
- **M1** — head rotation + leaning drive the game camera; camera found by AOB signature.
- **M2** — true per-eye stereo at **120 fps**, full sharpness. Confirmed "3D is great".
- **M3** — full VR controls: hand aim, sticks, buttons, snap/smooth turn, D-pad gesture.

**Baseline to protect (commit `5485596` lineage): 120 fps + controls + stereo.**

**Known open issues:** the left-eye ghost above; mono screen mode runs ~62-67 fps while stereo
runs 117-118 (not investigated; only matters as the fallback view); the gun MODEL is still
camera-glued (only aim follows the hand); no hand-ray crosshair; HUD elements split between the
eyes (grenades+radar left, ammo right).

## Key paths / environment

- Game: `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection`
- Host: `MCC\Binaries\Win64\MCC-Win64-Shipping.exe` (build **1.3528.0.0**)
- Engine DLL (RE target): `halo3\halo3.dll` (loads only once you enter a Halo 3 level)
- Installed mod: `...\Halo The Master Chief Collection\halo3xr\` (dll, launcher, logs)
- Runtime: SteamVR/OpenXR, PSVR2 via Sony adapter. Dev GPU RTX 5070 Ti.
- CMake: `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- Python 3.13 + `capstone`. Reference: `reference/UEVR` (praydog) — study its pattern of giving
  each eye a complete private view state rather than chasing individual buffers.

## Build / deploy / test loop

```
cmake --build n:\dev\halo3-openxr\build --config Release --target halo3xr
# deploy: copy build\Release\halo3xr.dll into the game's halo3xr\ folder
#   (the game must be CLOSED — the DLL is locked while it runs)
```
Launch via the **"Halo 3 VR"** desktop shortcut. Logs: `halo3xr.log`, `halo3xr_launcher.log`
(`.prev` keeps the last run).

## Architecture (src/)

- `dll/dllmain.cpp` — init thread: log, config, MinHook, D3D11 hooks, OpenXR, `Game_Init`.
- `dll/d3d11_hook.cpp` — hooks Present(8)/Present1(22)/ResizeBuffers(13) and
  `OMSetRenderTargets` (context vtable 33). **The RTV redirect is what makes stereo work.**
  A comment block lists the hooks deliberately NOT installed and why — do not re-add them.
- `dll/vr.cpp` — OpenXR session on the game's device; per-eye raster (`VR_BeginRasterEye` /
  `VR_CaptureRenderedEye` / `VR_EndRasterEye`); projection layer submission; menu quad.
- `dll/game.cpp` — camera-copy hook (head look, aim capture), `RenderViewHook` (renders the
  world once per eye with per-eye camera/cant/projection), turn + move mapping.
- `dll/input.cpp` — XInput virtual gamepad; **claims the exe's IAT slots** (Steam Input patches
  MCC's import table and routes around MinHook detours) and re-asserts every 2 s.
- `dll/menu.cpp` — window proc hotkeys + focus spoofing; ImGui.
- `common/` — logging, `halo3xr.cfg`.

## RE toolkit (tools/)

The user's AV blocks Cheat Engine, so we built our own **read-only** tools.
- **`pedis.py` — OFFLINE static disassembler** of halo3.dll on disk (no game running):
  `fn <rva> [len]` disassemble · `sig "<AOB>"` → RVAs · `scan <disp>...` find `[reg+disp]`.
  **Gotcha:** capstone's `disasm()` stops at the first undecodable byte; a naive linear sweep
  silently skips almost everything (50 hits vs 405 for the same scan once resync was added).
- `disasm.py <rva_hex> <len>` — capstone disassembly of the **live** process only.
- `verify_sig.py` — confirm an AOB is unique on disk.
- `camscan` (build `--target camscan`, run from `build\Release`, game must be in a level):
  `attach` · `first`/`narrow` differential float scan · `clusters` · `watch` · `poke`/`spin`
  (write test) · `findwrite`/`xref` (hardware watchpoints) · `hex`.

## Hotkeys

F1 menu · F2 head-tracking · F3 recenter · F6 leaning · F8/F9 pitch trim · F10 screen-follow ·
F11 stereo · PgUp/PgDn lean strength · Home/End weapon+HUD size (stereo only).
Yaw/pitch/up calibration flips are **F1-menu-only** (a stray Alt+F4 from SteamVR's exit path
used to hit the old F4 binding and invert head-turn + hand-aim — it read as "controls completely
broken". Fixed: hotkeys act on `WM_KEYDOWN` only and Alt+F4 passes through to the game).

## Root causes worth remembering

- **Controls dying between sessions** = **Steam Input patches MCC's IAT**, routing XInput calls
  around our detours; whether our hook saw traffic was a per-session race. Fixed by claiming the
  IAT slots directly and re-asserting. MCC also gates ALL XInput polling on
  `XInputGetCapabilities` reporting a connected pad — we fabricate one unconditionally.
- **"Head tracking completely broken"** was the phantom Alt+F4 above, not a ghost fix.
- Writing camera position feeds the sim (leaning is clamped for this reason).
