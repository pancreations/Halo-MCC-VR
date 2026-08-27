# Stage 3AK — Halo 2 Classic current-user first-person particle gate

Status: **headset candidate, not accepted until the user confirms the Classic muzzle flash is gone.**

## Why this differs from failed Stage 3AJ

The Stage 3AJ headset log reached Halo 2 Classic twice but contained no Stage 3AJ ACTIVE,
REFUSED, restored, or hit marker. Stage 3AJ's behavior therefore never armed and its visual
failure did **not** disprove the particle renderer target.

Stage 3AK discards Stage 3AJ's generic active-title wiring and starts again from the exact
accepted Stage 3AI DLL (SHA-256 `36232bc077d1ca4f5080bf514f93fb5b70f746ec3baaba5e75c46a66e3a2d0a8`).
It installs at Stage 3AI's proven successful Halo 2 module-reference publication instruction,
`HaloMCCVR+0x8F66B`, immediately after the exact retained `halo2.dll` base is available.

## Exact retail target

Pinned Steam retail `halo2.dll`:

`DE65B4F4FDBF3F0A5EAB7431FE530DA17DD815599182DFD6AE9B7E21CF171946`

The particle-system renderer is retail RVA `+0x76DC90`, with stock first 15 bytes:

`48 8B C4 88 50 10 89 48 08 55 53 56 57 41 55`

Three retail callers reach it:

- `+0x721B32` — passes `EDX=0`, therefore Stage 3AK leaves it stock.
- `+0x76E1A5` — passes `EDX=SIL`.
- `+0x76E360` — passes `EDX=SIL` after the traversal resolves effect ownership against the
  current output user and sets `SIL=1` for current-user first-person ownership.

Stage 3AK installs one guarded function-entry jump and suppresses a dispatch only when:

1. `DL != 0` (the engine's current-user/first-person classification), and
2. `halo2.dll+0xE70CF8 == 0` (live renderer is Classic).

Anniversary (`+0xE70CF8 == 1`) always executes the exact stock particle renderer path, even
though the hook remains installed. There is no Classic/Anniversary install/uninstall churn.

## Why particle suppression is the right subsystem

The user-supplied complete Halo 2 Classic firing-effect archive contains 126 firing-effect
component rows across 17 stock weapon families. Every row is classified as `particle`.
The rows span all four authored camera modes (0/1/2/3), which is why a mode-1-only filter
would be incomplete. Battle Rifle/Magnum muzzle flash rows and Covenant energy flash rows
are included in `evidence/STAGE3AK-H2-CLASSIC-MUZZLE-EFFECT-CHAIN.csv` and summarized in
`evidence/STAGE3AK-H2-MUZZLE-TAG-SUMMARY.txt`.

## Lifecycle / regression containment

- Only three existing Stage 3AI H2 lifecycle instructions are redirected:
  - `+0x8F66B`: exact H2 module-reference store -> Stage 3AK store+install wrapper.
  - `+0x90AC4`: Stage 3I restore/pin -> Stage 3AK restore chain.
  - `+0x90CE2`: Stage 3I release/clear -> Stage 3AK guarded release chain.
- The wrappers replay/call the original Stage 3I behavior.
- The hook is restored to the exact retail 15-byte prologue before the retained H2 module pin
  can be released. An unknown byte state fails closed and retains cleanup ownership.
- New code/data is appended at RVAs `+0x2F6000/+0x2F7000`. Stage 3AI's original `.s3qd`
  0x3000 bytes, including its live state region at the beginning, are not overwritten.
- No Halo 4, Reach, Halo 3, ODST, HUD, reticle, two-hand, pause, or OpenXR logic is rewritten.

## Runtime markers

A successful install must produce:

`Halo 2 Classic muzzle/effects Stage 3AK ACTIVE: ...`

The first actual suppressed Classic current-user FP particle must produce exactly one:

`Halo 2 Classic muzzle/effects Stage 3AK HIT: ...`

Cleanup logs the total suppressed dispatch count. These markers make the headset run
conclusive even if the visual result is unexpected.
