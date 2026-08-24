# Shared title-runtime ownership and Reach controller-input candidate

## 2026-08-24: C-RUNTIME-1 unifies active-load evidence retention

The C-H2-72 Steam run proves its Halo 2 -> Halo 4 repair: Halo 4 installed,
armed, and submitted complete pairs. On the next Halo 4 -> Reach transition,
the adapter selected Reach uniquely at 08:00:44.387, but for the remaining 24
seconds no Reach level-gate progress, cold preflight, or camera-core install was
logged. Presentation therefore stayed stock with stereo off.

This was not another module-registration omission. `ReachCameraCore_Poll`
called `g_reachLevelLoadGate.Rearm()` whenever the core was uninstalled and the
gate had not yet proven `levelRunning`. Since the worker polls every 50 ms, the
active Reach title's frozen/ticking samples were erased immediately after each
sample. The gate could never open on first entry, after another title, or after
a same-title level teardown. Halo 3, ODST, Halo 4 and Halo 2 already preserve
their active loading evidence and rearm on title exit/core retirement.

C-RUNTIME-1 makes the lifecycle rule explicit in
`ResolveTitleLevelGateAction`: an installed core retires when its title or level
closes; an inactive uninstalled title rearms for a future entry; every active
uninstalled title holds its accumulated gate evidence while loading. Reach now
uses this shared decision. Core tests enumerate all eight combinations of
active/installed/running so another title cannot silently invert the rule.
Camera/render implementations and all per-title engine bindings are unchanged.

The user explicitly promoted source
`1939eabc21c1607ef93ccaec97de004271d70091` as the new baseline on 2026-08-24.
Its installed DLL SHA-256 is
`D6332F6CE4F25B2277A071D12D80A93977913643BE89677855EA8C259F80A1D4` in
both Steam and Microsoft Store MCC. The supported-title regression matrix for
later shared lifecycle changes remains one continuous MCC session that enters
Halo 3, ODST, Reach, Halo 4 and Halo 2, switches among them, and loads two
consecutive levels inside each title. Logs should show each old core retiring
and each new/same-title core re-earning its own level gate and installing.

## 2026-08-24: C-H2-72 restores Halo 4 to the shared handoff contract

The first C-H2-71 Steam transition from Halo 2 to Halo 4 provides a precise
negative result. Halo 2's observer and both renderer cores retired cleanly, but
the shared manager then described the selected Halo 4 module as `adapter not
implemented` and changed the runtime mode from gameplay to unsupported. While
the Halo 4 level-load gate correctly remained closed on frozen initialization
state, MCC terminated in NVIDIA's user-mode D3D driver (`nvwgf2umx.dll`, access
violation `0xC0000005`, offset `0x15467D5`).

Source inspection identifies the lifecycle mismatch rather than a Halo 2
culling callback: the permanent Halo 4 camera core was absent from both
`TitleRegistry_HookPlan` and `RetainedRuntimeTitle`. Every other supported VR
title already occupied both tables. C-H2-72 adds `Halo4CameraCore` only when
that core is compiled and publishes `g_halo4Camera.generation` into the common
unique-generation retention decision. Raw resident-module generations remain
ineligible; zero or multiple installed camera-core generations still fail
closed. This preserves the established rule that module presence means only
availability while making Halo 4 participate in teardown/pending ownership on
the same terms as Halo 3, ODST, Reach and Halo 2.

This is an unaccepted cross-title lifecycle candidate. Required headset proof
starts with the observed Halo 2 -> Halo 4 transition and then exercises entry
to the other supported titles without restarting MCC. The Halo 2 C-H2-71
visibility cover remains enabled and unchanged.

Status: **desk-tested, headset-untested, unaccepted**.

The accepted baseline remains public release `MCC_VR_ALPHA_0.2.2`, runtime
source `3a2a11bfc66b36e70f60282e91c9d5436f2e18d1`. This candidate does not
advance `docs/CURRENT-STATE.md` and must not be installed or launched without
explicit approval for its exact packaged DLL hash.

## Exact parity behavior being preserved

Halo 3 and ODST keep the accepted 0.2.2 camera ownership, controls, stereo,
native CHUD, first-person weapon, capture, pause, fallback, and teardown paths.
This candidate does not extract, reorder, or add work inside either title's
two-eye render transaction.

Its shared-runtime behavior change is title selection when MCC keeps more than
one game DLL resident. Module presence now means only **available**. A title is
the runtime owner only when its currently installed, non-teardown lifecycle
generation publishes the one unique fresh camera heartbeat. Zero or multiple
qualifying titles expose no owner and no runtime capabilities.

The private Reach-ON preset adds one separately bounded behavior: when module
resolution identifies explicit Reach, shared virtual-controller admission is
enabled. This is a static input-admission policy, not Reach runtime ownership
or a Reach runtime capability publication.

## Rejected first package and retained fix

The exact `38c480a` package (`halo3xr.dll` SHA-256
`C6B49BD0F94E2F2366FDEBDC71D8359123A4FBA2035B381A5BF3478B9952B290`)
was rejected during its first Halo 3 headset regression. Gameplay controller
input, stereo, 6DOF, native HUD/weapon presentation, pause, resume, and title
exit all ran, but ordinary VR-pad input stopped after Save & Quit while MCC
held a multi-resident `Unknown` module set in the frontend.

The failure did not invalidate generation-tagged ownership. The resolver
correctly returned zero owner and zero capabilities after the module-set epoch
changed; the controller admission call site incorrectly treated that as a
reason to suppress title-independent frontend input. The retained fix restores
the accepted 0.2.2 rule only for ordinary controller transport in `None` or
multi-resident `Unknown` frontend/transition states. A unique owner may also
publish `ControllerInput`. CE, H2, and H4 states remain stock. Explicit Reach
also remains stock in the Reach-OFF preset; the Reach-ON preset grants it only
shared virtual-controller admission. Stereo, aim, movement transforms, HUD,
IK, room scale, runtime modes, and haptics receive no Reach grant or fallback
capability. The fabricated XInput slot also remains connected across gated
haptic transitions; stale haptic amplitude is still cleared.

The installed test files were restored from the verified official 0.2.2 ZIP.
The source foundation was retained and corrected for a new uniquely hashed
replacement candidate.

## State and transition rules

- The fixed title table tracks the exact loaded-module mask and base address.
  A title's generation changes when that title loads, unloads, reloads, or is
  rebound at another base. A separate epoch changes whenever any mask/base
  member of the complete module set changes.
- Lifecycle, mode, and heartbeat publications carry the title generation.
  Stale or foreign generations are rejected. A heartbeat must be strictly
  newer than both the title-generation boundary and the complete module-set
  boundary.
- On the first transition from one title module to a multi-resident set, the
  already-hooked title gets a bounded 100 ms teardown-only pending interval to
  publish a post-transition heartbeat. Pending exposes no armed state,
  heartbeat, gameplay mode, or capabilities; it never installs a hook, admits
  a different title, or survives multiple qualifying owners.
- New H3 or ODST hooks are installed only when exactly one title module is
  available. This candidate deliberately does not solve entry into an unhooked
  new title while an old title DLL remains resident; that remains a later,
  separately tested resident-module lifecycle candidate.
- Halo 3 ownership uses the accepted strict `<500 ms` camera-fresh boundary.
  ODST preserves its accepted asymmetric lifecycle: `<500 ms` drives fresh
  camera debounce, an unready camera falls back after `>750 ms`, and a
  previously seen still-ready camera is retained through age 5000 ms and falls
  back at 5001 ms. Multi-resident ownership is always clamped to strict
  `<100 ms` for both titles.
- Unarmed owners can expose only capabilities that do not require the armed
  camera transaction. Stereo, aim, HUD, arm IK, room scale, and haptics are
  masked until armed. Ordinary controller input and runtime-mode reporting are
  separate capabilities.
- Reach-ON controller admission requires an explicitly resolved Reach title.
  A multi-resident `Unknown` frontend cannot claim the Reach-specific policy.
  The title-independent frontend controller-continuity fallback remains
  separate and does not establish Reach gameplay ownership.

## Publication sites and safety

- Halo 3 publishes only at the existing `CamCopyHook` camera-transform point.
- ODST publishes only after the original native camera copy, inside the
  existing proven slot-0/single-user/active-camera branch.
- The hot publication path is fixed-storage, bounded, atomic, lock-free,
  allocation-free, log-free, and does not scan or perform file I/O.
- ODST teardown still disables the outer renderer first, lets an in-flight
  two-eye transaction finish, verifies callback quiescence, removes every hook,
  restores native patches and variables, and only then clears installed state.
  Incomplete cleanup remains installed with teardown requested and zero exposed
  capabilities.
- Reach publishes no generation-tagged lifecycle, mode, heartbeat, owner, or
  runtime capability state. It remains `runtimeSupported=false` with runtime
  capabilities `TitleCapability_None`.
  `TitleHookPlan::None` and `ReachAdapter_RuntimeHooksPermitted()==false` remain
  unchanged in both presets. Reach-ON changes only explicit-title shared
  virtual-controller admission; camera, rendering, aim and movement
  transforms, HUD, arm IK, haptics, and lifecycle remain disabled.

## Desk validation and required headset regression

The deterministic core matrix covers generation rollover, exact epoch and
freshness boundaries, future/stale/foreign publications, zero/one/multiple
owners, pending grace, teardown, mode invalidation, heartbeat clearing,
capability masking, Reach's zero runtime capability mask, and the separate
explicit-Reach controller-admission policy. Routine Reach behavior candidates
must build and pass CTest with the cumulative Reach-ON preset: Halo 3 + ODST
plus Reach shared virtual-controller admission. Reach-OFF compile and regression
remain a milestone/promotion gate instead of blocking each Reach experiment.

Desk tests cannot accept this shared lifecycle change. The exact packaged DLL
hash still requires, at minimum, a Reach headset result and a Halo 3 regression.
Reach testing must confirm ordinary virtual-controller buttons and sticks only,
with stock camera/render presentation and no aim or movement transforms, HUD/IK
changes, haptics, lifecycle, or runtime hook activity. Halo 3 regression must
confirm controller continuity through gameplay, pause, Save & Quit, and the MCC
menu while preserving the accepted stereo, aim, movement, HUD, weapon, and
rumble behavior. ODST and Reach-OFF regression remain milestone/promotion gates
instead of blocking this routine Reach experiment. Until the required exact-hash
results are recorded, 0.2.2 remains the only accepted build.
