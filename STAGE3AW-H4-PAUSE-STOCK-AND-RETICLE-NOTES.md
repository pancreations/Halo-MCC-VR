# Stage 3AW — Halo 4 pause backbuffer ownership + selected-copy reticle framing

Status: **HEADSET PENDING**. Do not advance `docs/CURRENT-STATE.md` until the
user accepts this exact artifact in the headset.

Input: rejected Stage 3AV, 2,919,424 bytes, SHA-256
`2BAF0A3E7D654A0CDA701399D672CEC7C582202E138537F8193E34E5D72ACA16`.

Output: Stage 3AW, 2,919,424 bytes, SHA-256
`AD0C6BBCA337F2436A258CB4A0CB9DA5884B20270BC2F9DADF7A06DABA1ED676`.

This is a forward-only post-link layer over the exact Stage 3AV DLL. It keeps
the AP-through-AV authored-reticle work and all earlier title behavior. The
source snapshot predates those layers and has no `.git`, so rebuilding the
snapshot would discard shipped behavior; it is not the reproduction path for
this candidate.

## Pause black screen

The complete rejected Stage 3AV log is preserved at
`out/deploy-backups/2026-08-28-stage3av-rejected-complete/`. It proves a
producer mismatch:

- presentation changes to the one-layer head-locked stock-backbuffer quad;
- Halo 4 nevertheless completes 219, 241 and 51 private stereo pairs below it;
- every report says `0 stock windows`.

`Halo4WrapperBody` was still consuming the native wrapper into private eye
targets after the compositor selected the stock backbuffer. The pause quad was
therefore valid but black.

Stage 3AW splices the H4-only armed load at mod RVA `0x58BED`. While the pause
presentation target is active it routes the wrapper through the existing stock
tail at `0x58D5D`. The camera core remains armed; this feature does not end the
OpenXR session or record a false `NotArmed` rejection.

Resume is semantic, not a controller guess. Official H4EK identifies
`c_start_menu_pause_component` as the owner that pauses the game while it
exists. It sets pause reason 3 and its destructor clears reason 3. The matched
retail getter is:

```text
bool __fastcall game_is_paused_for_reason(int32_t reason)
halo4.dll + 0xA0AE4, reason = 3

8B 15 ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00
41 B8 90 00 00 00 48 8B 04 D0 49 8B 14 00
32 C0 38 02 74 0F B8 01 00 00 00 66 D3 E0
66 85 42 02 0F 95 C0 C3
```

The signature has one `.text` hit at `0xA0AE4` in both pinned retail editions.
The 51-byte body is identical, SHA-256
`4A7BA392BAAF1AC6FC0AAD577BA8ECBA13D43D875602A17195A65459E868B5A2`.
Stage 3AW checks every fixed signature byte at that site before calling it,
wildcarding only the RIP displacement. A mismatch stays on the visible stock
pause path and never calls or disarms anything. A latched reason-3 true-to-false
edge requests stereo and stamps Stage 3X's existing resume grace at `0x2F31C8`.
The established B/Y+B escape remains untouched.

Retail identities verified before packaging:

| Edition | `halo4.dll` SHA-256 |
|---|---|
| Steam | `7C53E7D5BC9848545A1B70E2768242479336FBA1B7630D7AB955F7FD0C34FA84` |
| Microsoft Store | `5767CD564C1E8E8D012D002A8DE8E92960A3DE46442399ED054E3C4EF44AA496` |

Both are version `1.3528.0.0`, PE timestamp `0x68A0E7BF`, image size
`0x04A3F000`.

## Authored reticle

Stage 3AV proves the capture, exact scene-target reroutes and framing reasserts
execute, but every gameplay probe remains `alpha 0 rgb 0 art 0` with zero write
failures. The capture was started on the first unrelated readable CUI command,
and its fixed `{7572, 4258.068}` viewport described only one of the runtime CUI
resolution/class copies.

H4EK and retail prove that the exact `0x28/0x0C` command pushes the one
`0x34`-byte reticle transform used by its descendants. Stage 3AW:

1. Delays capture binding until that exact marker. The payload-readable byte is
   caller `[rsp+0x21]`; `[rsp+0x20]` is only the adjacent header-readable flag.
2. Uses the already-safe copied top matrix after the original marker command.
3. Validates all 13 floats and the selected copy's finite half-extents at
   matrix offsets `+0x28/+0x2C`.
4. Publishes a symmetric viewport into the existing mod-owned capture state:

```text
Width  = 4 * abs(baseX)
Height = 4 * abs(baseY)
Left   = 256 - 2 * abs(baseX)
Top    = 256 - 2 * abs(baseY)
Depth  = 0..1
```

This is exactly 2× authored-art magnification for both measured runtime copies
(`-1033.578/+336.549` and `-1893/+1064.517`). The initial H4-only viewport is
neutral `512×512`; the already-proven exact OM reroutes reassert the selected
viewport before delayed descendant batches. No COM call, allocation, logging,
lock, file I/O or signature scan was added to the command hot hook.

Malformed matrix data refuses only authored capture and increments its existing
feature counter. The existing blank-art guard keeps the procedural VR reticle
eligible, and the normal-pass hide path is unchanged.

## Binary surface and checks

Payload: mod RVA `0x2FA790`, 513 bytes, SHA-256
`243CFF272992484A73670172525615474A361D5852CB83BC845009518BF09661`.

Exactly 470 bytes differ from Stage 3AV, all within five guarded regions:

| Region | RVA | Reserved bytes |
|---|---:|---:|
| exact-marker begin splice | `0x538D3` | 7 |
| selected-copy framing call | `0x539C0` | 10 |
| H4 pause stock gate | `0x58BED` | 8 |
| H4 initial viewport | `0x1BF0C0` | 16 |
| Stage 3AW payload | `0x2FA790` | 513 |

Passed:

- `tools/test_stage3aw_h4_pause_stock_and_reticle.py`
- `tools/verify_stage3aw_h4_pause_binding.py` against both installed editions
- `tools/test_stage3al_odst_teardown_pin.py` on its pinned historical pair
- `tools/check-reach-fp-parity.ps1`
- two independent disassembly/ABI/diff audits

The general CMake suite could not configure in this reopened snapshot because
`out/deps/openxr-src` is absent. That suite builds the older source snapshot,
not the AP-through-AW post-link artifact, so no substitute source DLL is used.

## Headset oracle

1. In Halo 4 gameplay, the actual weapon reticle/hit/spread art should occupy
   the existing gun-ray VR quad, with no flat duplicate. The reticle probe must
   become nonzero and `blankHeld` should stop growing once art arrives.
2. Y+B pause should show the native pause menu instead of black. During pause,
   completed private pairs should fall to zero and stock-window counts should
   become nonzero while XR remains one layer.
3. Selecting Resume with A should restore stereo through the native reason-3
   clear edge. B/Y+B remains the fallback exit.

Only that headset result can accept Stage 3AW.

## Deployment

Installed on 2026-08-28 after confirming MCC and the launcher were closed.
Both live DLLs were independently hash-verified after copying:

| Edition | Installed path | SHA-256 |
|---|---|---|
| Steam | `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR\HaloMCCVR.dll` | `AD0C6BBCA337F2436A258CB4A0CB9DA5884B20270BC2F9DADF7A06DABA1ED676` |
| Microsoft Store | `N:\XBOX\Halo- The Master Chief Collection\Content\Halo_MCC_VR\HaloMCCVR.dll` | `AD0C6BBCA337F2436A258CB4A0CB9DA5884B20270BC2F9DADF7A06DABA1ED676` |

The previous Stage 3AV DLL, launcher, config and complete log from each edition
are preserved under `out/deploy-backups/2026-08-28-pre-3AW/`. Only
`HaloMCCVR.dll` was replaced. Neither config nor launcher changed, and MCC was
not launched.
