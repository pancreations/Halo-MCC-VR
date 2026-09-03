# C-H4-58 - Halo 4 effect suppression survives reinstall

## Scope

This candidate repairs one Stage 3AI ownership asymmetry. The exact Halo 4
first-person muzzle/effect suppression already installs and works, but its
cleanup did not restore the trampoline cave. A later Halo 4 install therefore
refused that non-stock cave and left effects stock.

## Change

- Build the existing 0xF0-byte cave payload in one function for install and
  cleanup ownership checks.
- Restore all four routes into the cave first.
- Restore the cave to its verified all-zero stock bytes only when it still
  matches the mod-owned payload, then verify all routes and the cave are stock.
- Preserve feature-local failure isolation. Any ownership mismatch keeps the
  cleanup obligation loud and cannot disarm Halo 4 camera, stereo, HUD,
  reticle, helmet, pause handling, screen effects, or OpenXR.

## Evidence and acceptance

The supplied Steam log first reports `Halo 4 effects Installed` and
`effects=LIVE`, then reports the exact cave as non-stock on the later install
and remains `effects=StockFallback`. Source inspection identifies the omitted
cave restore as the complete cause of that refusal.

Build/tests prove only static integrity. Headset acceptance requires Halo 4
muzzle flashes and the previously targeted Promethean first-person effects to
remain hidden after ordinary title/level lifecycle transitions, with all prior
Halo 4 behavior unchanged.

## Supplied-log performance check

At the user's 4368x3150 backbuffer and 120 Hz panel, the log's low-rate Halo 4
windows are dominated by `renderWindow` (roughly 24-36 ms p95). Measured
two-eye publication remains about 0.60-0.73 ms/frame, DXGI Present about
0.2-0.34 ms, and frame-wait handoff about 0.01 ms. Similar low frame rates are
reported with stereo off. Nothing in the log identifies a runaway Halo 4 hook
or VR publication bottleneck; resolution and scene rendering are the supported
causes. This candidate therefore makes no unrelated performance change.
