# Stage 3U — Reach shell handoff + re-entry lifetime

Status: **headset test candidate**, built on the exact Stage 3T loader-safe DLL.

## Headset evidence / root cause

The 2026-08-27 Stage 3T headset log proves the loader regression is fixed and
Reach itself installs, arms, and submits stereo normally. On Save & Quit the
failure begins when the real Reach camera path stops but the pinned
`haloreach.dll` remains resident:

- the OpenXR compositor falls to `layers=0` while the session is still focused;
- the runtime briefly reports `gameplay -> loading -> gameplay` even though the
  player is leaving the title;
- `REACHCINE` reports that the owned camera path has stalled;
- there is no `Reach camera core removed` line anywhere in the run;
- later the raw module selector sees the still-pinned `haloreach.dll` and calls
  Reach active/loading again.

Stage 3T correctly fixed MinHook status-10 teardown by retaining a real loader
reference while Reach hooks exist. The missing second half was a *real* title
liveness signal: `AutoVrTick` was refreshing `g_reachLastCamCopyMs` from shared
MCC `Present`, so the new loader pin made stale Reach ownership immortal.

This is also directly consistent with an older known-good headset lifecycle
(2026-08-19): after leaving Reach, the old build disabled stereo, successfully
removed the Reach camera core, re-armed the level gate, and returned to
`layers=1`. That path only stopped working after teardown began failing against
an already-unmapped title DLL, which Stage 3T fixed.

## Stage 3U behavioral change

Stage 3U combines those two known-good properties instead of changing the
renderer:

1. The admitted `ReachMainRenderViewDetour` records
   `g_reachLastCamCopyMs = GetTickCount64()` from the **real Reach camera
   callback**.
2. `AutoVrTick` no longer manufactures a new timestamp from MCC `Present`.
   It publishes the last real camera timestamp to the shared title-runtime
   resolver.
3. While Reach is armed and **not in its native pause**, if that real camera
   timestamp is more than 1000 ms stale, Stage 3U sets the existing
   `teardownRequested` flag.
4. The existing worker then runs the normal verified `RemoveReachCameraCore()`
   path. Stage 3T's loader pin keeps every MinHook target mapped through that
   teardown, then releases the pin only after hooks/state are clean and the
   Reach level-load gate is re-armed.
5. Native pause remains exempt so Y+B / ordinary pause still uses the accepted
   head-locked 2D presentation without tearing down the title core.

There is deliberately **no compositor fallback hack** and no edit to `vr.cpp`.
That avoids reintroducing the earlier flat-frame/flicker class of regressions.
Once the stale core is removed, the existing stock-screen path naturally owns
the MCC shell again.

## Binary implementation

Exact Stage 3T base SHA-256:
`86919573435dd19ab68c36e78c84ac698208a7350f7b117c709d2bfc3b8eef7c`

Stage 3U adds two guarded post-link redirects only:

- RVA `0x72D43`: replace the original Reach outer-detour `lock inc` with a call
  to a helper that replays the increment and records the real camera timestamp.
- RVA `0x435BD`: replace Present's synthetic `GetTickCount64()+store` with a
  helper that returns the real timestamp and requests teardown after >1000 ms
  stale while unpaused.

The helper lives at RVA `0x2F1B60` in an already-zero executable cave inside
`.s3ic`. Stage 3T's helper at `0x2F1B00` is untouched.

The loader-hotfix PE geometry remains byte-for-byte identical:

- 12 sections
- `.s3ic` RVA `0x2F1000`, VirtualSize `0x16F0`
- `.s3qd` RVA `0x2F3000`
- `SizeOfImage = 0x2F4000`
- no import-table, section-table, architecture, launcher, or config changes

## Headset test order

1. Enter Reach and verify stereo/6DOF/aim/hands/HUD still behave normally.
2. Pause/unpause once with Y+B; it must remain head-locked 2D while paused and
   return to stereo without tearing Reach down.
3. Pause Reach and choose **Save & Quit**.
4. The MCC shell/main menu must return instead of remaining black. A short
   transition while verified teardown completes is acceptable; it must settle
   on the normal stock menu layer.
5. Without restarting MCC, enter Reach again. The next Reach generation must
   pass the level gate, reinstall, arm, and submit stereo again.
6. If that works, optionally do Reach -> Halo 3 -> Reach as the original Stage
   3T cross-title re-entry test.
7. Save `HaloMCCVR.log`.

Expected proof after Save & Quit includes:

- `Reach camera core removed; stock Reach owns the title`
- a transition away from Reach ownership
- `status ... layers=1` in the MCC shell
- on re-entry, a new Reach level-gate proof + camera-core install/arm.
