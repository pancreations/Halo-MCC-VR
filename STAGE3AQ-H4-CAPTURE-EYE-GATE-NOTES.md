# Stage 3AQ — admit Halo 4's authored-reticle capture through the redirect gate

Input: Stage 3AP, SHA-256
`3f6cceddf290305886adb37129eb1ab9b3d04a19d91ecfca6c37b40d103858ac`
(Stage 3AL plus the six-byte C-H4-50 removal).

Output: 2,915,328 bytes, SHA-256
`e68b39b1e1f054d1b3db69bf9fa111943a40eaf2709f0395699d09f17d7f186a`.

## Headset evidence behind this pass

Stage 3AP turned the Halo 4 capture back on, and the capture ran — but produced
nothing. From the 2026-08-28 Steam log (raster 3786x2730, HUD sliders live):

```
483 main gameplay CUI passes, 1350 begin markers, 1350 native hides
4 completed actions (4 authored captures / 0 native hides→see below)
0 exact capture OM reroutes (0 framing reasserts)
Halo 4 reticle upload: ... pieces 0, held 0, art 0, blankHeld 4
```

The hide works and the capture opens, but **zero OM reroutes**. That counter is
the whole mechanism: Halo 4 rebinds its learned scene-colour target *inside* CUI
playback, so the private target bound when the capture opened is immediately
replaced by the engine's own bind. The only thing that puts Halo 4's CUI draws
into the authored texture is the reroute of that exact rebind, in
`VR_RedirectRenderTargets`. Zero reroutes means every CUI draw went to the eye,
not to the capture, so the texture stayed empty — exactly `pieces 0, art 0`.

The previous measurement of this path recorded "9 exact capture OM reroutes
against 3 captures", so the reroute is expected to fire several times per
capture. Zero is a refusal, not an absence of rebinds.

## Why it refuses

The reroute block sits below the function's entry refusal:

```c
if ((!scope && !nativeHud && (eye < 0 || eye > 1)) || !input || !output ||
    !g_gameSwapchain)
    return false;
```

compiled as:

```
0x02FF05  test cl, cl              ; scope
0x02FF07  jne  0x02FF18
0x02FF09  test r15b, r15b          ; nativeHud
0x02FF0C  jne  0x02FF18
0x02FF0E  cmp  r9d, 1              ; g_rasterEye
0x02FF12  ja   0x02FE92            ; -> return false
0x02FF18  test rbx, rbx            ; continue ...
0x02FF38  cmp  byte [0x2AE770], 0  ; g_reticleCaptureState.active
0x02FF52  cmp  al, 4               ; GameTitle::Halo4
```

The gameplay CUI replay that opens the capture is not guaranteed to run with a
published raster eye. When it does not, `ja` fires and the function returns
before the Halo 4 capture block is ever reached.

## The change

`0x02FF0E` (10 bytes) becomes `jmp 0x2F9000` plus five NOPs. The new page holds
seven instructions:

```
0x2F9000  cmp byte ptr [rip -> 0x2BA6C8], 4   ; active title == GameTitle::Halo4
0x2F9007  jne 0x2F9012                        ; other titles: stock test
0x2F9009  cmp byte ptr [rip -> 0x2AE770], 0   ; g_reticleCaptureState.active
0x2F9010  jne 0x2F901C                        ; Halo 4 capture open: admit
0x2F9012  cmp r9d, 1                          ; ORIGINAL instruction pair
0x2F9016  ja  0x2FE92                         ; ORIGINAL refusal
0x2F901C  jmp 0x2FF18                         ; continue edge
```

The active title is read as a byte rather than through
`TitleAdapter_GetActiveTitle`, because `rdx`, `r9d`, `r12d`, `r14`, `r15b` and
`rsi` are all live at this point and a call would clobber the volatile set. The
getter is `movzx eax, byte ptr [rip+X]; nop; ret`, so the byte at `0x2BA6C8` is
the same value the call would return.

## Scope

Strictly permissive and strictly Halo 4. Any title other than Halo 4, and Halo 4
with no capture open, executes the identical `cmp r9d, 1 / ja` it executed
before — the builder asserts those four bytes match the instruction pair being
replaced. Halo 3, ODST, Reach and both Halo 2 modes cannot reach the admitted
path at all, because the block it admits into begins by requiring
`GameTitle::Halo4`, and that block re-checks capture state and target validity
before touching a single bind.

## Expected headset result

`Halo 4 C-H4-48 ...` should report a non-zero **exact capture OM reroutes**
count, and `Halo 4 reticle upload:` should report a non-zero **art** value in
place of `blankHeld`. Visually, the VR crosshair carries Halo 4's own reticle and
follows the weapon.

If reroutes stay at zero, the refusal was not the cause and the next suspect is
the scene-target identity itself (`g_sceneColorRtv`, learned at `0x2AEAF0`) going
stale after the mid-session SCENEPROBE re-learn seen at 01:00:21.

## Reproduction

```text
py -3 tools/build_stage3aq_h4_capture_eye_gate.py \
    built/Stage3AP-HaloMCCVR.dll built/Stage3AQ-HaloMCCVR.dll
```

The builder refuses any input but the exact Stage 3AP SHA-256, guards six
surrounding instruction sequences plus the ten bytes it replaces, decodes its own
emitted payload with capstone and checks every resolved target against the
recorded RVAs before writing.
