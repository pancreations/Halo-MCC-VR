# Stage 2C — Halo 2 pause-screen delivery

## Why Stage 2C exists

The Stage 2B headset test proved that H2's Y+B pause request now survives title
ownership, but exposed a second compositor conflict. The runtime log showed:

- Y+B -> `target=head-locked 2D`
- transition completed -> `presentation switched to head-locked 2D`
- H2 then classified the prepared frame as `frame-local drop`
- the compositor reported `layers=0`

That is the black headset view reported by the tester.

## Source relationship

The Stage 2B source tree already contained the intended C-H2-72 rule in
`src/common/halo2_render_logic.h` and `src/dll/vr.cpp`:

- `Halo2PausePresentationOwnsStockScreen(activeHalo2, pausePresentation)`
- while true, H2 chooses `SharedDefault` instead of calling the synchronous
  selector, allowing the existing screen/backbuffer quad to own pause display.

The Stage 2B *DLL* did not contain that source-side C-H2-72 behavior; it only
contained the earlier two-byte title-entry foreign-pause fix. Stage 2C corrects
that mismatch. `tools/build_stage2c_h2pause_screen_trampoline.py` reproduces the
Stage 2C DLL from the exact Stage 2B DLL.

## Runtime scope

The new `.h2pm` trampoline checks the compiled `g_pausePresentation` byte and
the already-live `halo2Title` boolean. Only when both are true does it set the
H2 presentation decision to `SharedDefault`. It then replays the displaced
strict-stock-screen decision bytes exactly and returns to stock code.

This means:

- pending fade edge: unchanged
- normal H2 stereo: unchanged
- claimed/complete Drop safety outside pause: unchanged
- quarantine policy outside pause: unchanged
- H3/ODST/Reach/H4: unchanged
- Stage 2A `.h2aa` aim/melee trampoline: unchanged
- C50 Halo 4 reticle binary patch: unchanged

## Verification

Post-link verification on the produced DLL:

- `.rdata`: byte-identical to Stage 2B
- `.data`: byte-identical
- `.pdata`: byte-identical
- `.fptable`: byte-identical
- `.rsrc`: byte-identical
- `.reloc`: byte-identical
- `.h2aa`: byte-identical
- `.text`: exactly one contiguous 23-byte changed range
- new `.h2pm`: 53 bytes of code, 0x200 raw aligned section

Stage 2C DLL SHA-256:
`a2460f279783df5d9e30feac23cc9fb22a6b66a79487a707fb17ef9c05d05f0f`
