# ODST minimal camera/stereo/6DOF bring-up handoff

Prepared: 2026-07-21
Branch: `feature/odst-bringup`
Historical starting HEAD at original evidence handoff: `543b4f0`
Reviewed camera-core checkpoint: `7c25a1a`

This document began as the implementation boundary after the signature and
camera-layout evidence gates passed for ODST's single-user stock path. The
current worktree now contains that private, deliberately minimal
camera/stereo/6DOF implementation—not general ODST support.

## Read first

In this order:

1. `docs/CURRENT-STATE.md`
2. `docs/CONTINUATION.md`
3. `docs/PLAN.md`
4. `docs/ODST-SIGNATURE-EVIDENCE.md`
5. `docs/ODST-CAMERA-LAYOUT.md`

The read-only live probe is `tools/odst_layout_probe.py`. It requests only
`PROCESS_QUERY_INFORMATION | PROCESS_VM_READ` and was exercised against stock
ODST launched with EAC off.

Preserve `stash@{0}` (`preserve user README before ODST bring-up`). It contains
the user's pre-existing README work and must not be applied, dropped, or folded
into an ODST commit without the user's explicit direction.

## Current implementation checkpoint

- CMake option `HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP` defaults to `OFF`. ODST
  remains `runtimeSupported=false` with capabilities `None`, and an OFF build
  gives it no hook plan. The public `deploy.bat` and `export-alpha.bat` flows
  demand an exact OFF cache entry and reject private ON caches.
- Explicitly configuring the private option ON enables only the isolated ODST
  camera-core installer. It requires the exact retail timestamp/image size,
  ten unique title-specific signatures, in-module/code-range checks, the
  derived four-slot array, and proven ODST layout/single-user invariants before
  hook creation. It does not invoke Halo 3's monolithic installer.
- The complete ODST detour set is exactly four hooks: compact-camera copy,
  inner prepared-view render, FP camera rebuild, and FP driver. It supplies
  stereo, rotational tracking, positional 6DOF, and minimum FP-camera
  coherence only.
- ODST controls/controller aim, reticle suppression, HUD/VISR changes, scopes,
  pause, brightness, motion blur, weapons, bones, arms, VRIK, and all gameplay
  patches remain off. Shared input/presentation paths fail closed to stock or
  physical-controller behavior outside the proven camera context.
- Install is an atomic all-or-stock transaction. The hooks remain disarmed
  until a continuously fresh camera passes the stability debounce. Fallback
  disables the outer renderer before dependencies, drains and verifies detour
  ingress, and retains the exact module/hook state for retry if safe teardown
  cannot yet be proved. Rearm requires a proven reload edge or title re-entry.
- Camera-array readiness is worker-published only while the exact module lease
  is retained, and the render detour revalidates every slot immediately before
  camera mutation. Presentation-detach generations are acknowledged on Present
  before XR early exits; a completed detach resets the camera debounce and
  cannot be followed by a same-Present stereo rearm.
- Active-title detection is polled every 50 ms and is not an atomic transition
  signal. MCC can retain multiple title modules and produce `Unknown`; shared
  Halo 3 behavior is then permitted only by a post-transition Halo 3 camera
  heartbeat less than 100 ms old. Explicit ODST/private camera-only ownership
  blocks it.
- Position currently uses a `1 / 3.048` game-unit-per-OpenXR-meter multiplier.
  This is a headset-calibration hypothesis, not accepted ODST scale evidence.
- Both option-OFF and private option-ON final Release builds and CTest pass
  locally. The private Release hook wrappers also passed their unwind-metadata
  check. No implementation binary has been deployed or launched, and no ODST
  headset test has occurred.

The next runtime step is the narrowly scoped private headset checkpoint below.
On 2026-07-21 the user asked to prepare the next chat so they can test ODST.
That authorizes the next chat to review and run the dedicated private procedure
documented here. It does not authorize public packaging/support, additional ODST
features, or an unattended game launch. Nothing was deployed or launched while
preparing this handoff.

## Proven evidence available to implementation

Retail ODST module used for the gate:

- SHA-256: `5BB20976EFDFD9E1CE59C589339804725FEC239021027C8D65B2733EAB94829A`
- PE timestamp: `0x68A0F232`
- loaded image size: `0x4797000`

The eight byte-identical production signatures and all twelve re-derived ODST
signatures are unique and land in their semantic counterparts. The patterns,
function comparisons, and rejection of lookalikes are in
`ODST-SIGNATURE-EVIDENCE.md`.

Camera-core ODST anchors:

| Role | ODST RVA |
|---|---:|
| compact-camera copy | `0x2CAAF0` |
| inner prepared-view renderer | `0x2AFB10` |
| prepare view | `0x1B4694` |
| viewport builder | `0x2CAC5C` |
| matrix builder | `0x2CB1C4` |
| FP camera rebuild | `0x2A6F5C` |
| FP camera uploader | `0x2A45DC` |
| FP driver | `0x2AC2E4` |
| FP driver guard callsite | `0x2AE8DE` |
| four-slot camera-array constructor | `0x00006B60` |
| camera array | `0x2D73590` |

RVAs are evidence/debug references, not runtime addresses. Runtime resolution
must use the documented title-specific unique AOBs and must reject zero or
multiple matches.

Proven ODST layout constants required by the camera core:

| Field | ODST value |
|---|---:|
| compact camera size | `0x90` |
| derived block size | `0xC0` |
| root current compact | `+0x008` |
| root current derived | `+0x098` |
| root secondary compact | `+0x158` |
| root secondary derived | `+0x1E8` |
| nested FP prepared-view base | `+0x6C8` |
| nested current compact | `+0x6D0` |
| nested current derived | `+0x760` |
| nested secondary compact | `+0x820` |
| nested secondary derived | `+0x8B0` |
| nested FP source-camera pointer | `+0x970` |
| normalized viewport floats | `+0x27E0..+0x27EC` |
| render user index | `+0x27FC` |
| final observed tail boolean | `+0x280C` |
| prepared/gun-camera stride | `0x2810` |

Do not translate Halo 3 tail offsets by a blanket `+8`. ODST's tail is
structurally different and shorter. Root `+0x2A8` is not a valid camera pointer
in the live object; the source-pointer meaning applies to the nested prepared
view, where nested `+0x2A8 == root+0x970` and live capture proved it points to
root compact `+0x008`.

Live capture covered movement/look, zoom, death/respawn, level unload/reload,
cutscene cameras, and vehicle entry/exit. In particular:

- compact `+0x2C` is the zoom/reference FOV used by FP normalization;
- compact `+0x30` is the FP-camera blend/eligibility weight (`1` on foot, `0`
  in vehicle/cutscene cameras, interpolated at transitions);
- compact `+0x34` is a vertical projection/observer aim-offset scalar;
- compact `+0x6C..+0x7B` is an optional oblique near-clip plane;
- `+0x40/+0x54` are the window/render title-safe rectangles;
- root near is normally `0.0078125`; the flagged nested FP rebuild commonly
  uses `0.01875`;
- only slot 0 was active; slots 1-3 retained constructed vtables and zero camera
  state;
- level unload zeroed the camera blocks and nested source pointer cleanly.

## Implementation boundary

The first ODST runtime checkpoint may touch only:

- title-specific camera signatures and layout data;
- camera copy / inner per-view stereo / viewport / matrix integration;
- the minimum FP camera rebuild/upload/driver integration required to keep the
  weapon/HUD camera coherent in stereo;
- hook ownership, all-or-stock fallback, level unload, title exit, and reload.

It must not enable or port controls, controller aim, reticle suppression, HUD or
VISR changes, weapon/arm IK, palette/bone hooks, sway patches, native weapon-IK
patches, pause hooks, scopes, brightness, motion blur, or gameplay patches.
Those remain later evidence and headset checkpoints.

Do not call the current monolithic Halo 3 `InstallHook` for ODST. It installs
many Halo 3-only hooks and byte patches. Split or add a camera-core ODST
installer so the existing Halo 3 path and its exact signatures/offsets remain
unchanged.

## Required implementation shape

1. Introduce a title-specific camera runtime profile containing patterns,
   layout sizes/offsets, module identity for logs, and only the function roles
   consumed by the minimal camera core. Do not weaken or replace Halo 3 AOBs.
2. Keep ODST `runtimeSupported=false` and capabilities at `None`. Add a clearly
   named **build-time experimental ODST bring-up option**, default `OFF`, so a
   normal/package build still leaves ODST stock. The private headset build may
   turn it on explicitly. Do not hide this behind an undocumented user config.
3. Perform a read-only preflight before creating any detour: resolve every
   required ODST signature, prove uniqueness, ensure each address lies inside
   `halo3odst.dll`, resolve the four-slot array with the `0x2810` constructor,
   and log all RVAs and the active layout profile.
4. Treat the camera-core installation as an atomic transaction. If any required
   signature, invariant, hook creation, or hook enable fails, remove anything
   created in that attempt, clear all ODST function pointers/state, and leave
   the stock title running. Never accept a partial stereo path.
5. Use ODST layout constants at every hook access. Do not reuse hardcoded Halo 3
   `+0x27F4` or `0x2820`. ODST derived camera storage/copies are `0xC0`.
6. Preserve the headset-confirmed Halo 3 implementation exactly. In particular,
   do not silently change Halo 3's existing `g_eyeDerivedBlock[0x90]` behavior
   while adding the separate ODST `0xC0` path; audit that separately later.
7. On level unload/title leave, disarm the ODST camera heartbeat, disable/remove
   only installed ODST hooks, clear cached module/function/view pointers, detach
   VR presentation, and tolerate the DLL being mapped again at the same base.
8. Do not set `runtimeSupported=true` in this checkpoint. That is a public
   acceptance decision after ODST headset validation and the full Halo 3 shared
   regression.

## First private headset checkpoint

Authorization status: the camera-core/fallback review is complete, and the user
asked for the next chat to prepare the private build so they can test ODST. The
next chat may deploy the reviewed private build with the dedicated opt-in script
below after repeating the desk gates. Do not launch MCC on the user's behalf;
report the deployed identities and let the user begin the headset session. The
public deployment/export scripts remain OFF-only and must not be weakened or
bypassed.

Before deployment:

- retain separate, explicit option-OFF and option-ON build trees;
- rerun both Release builds and both CTest suites;
- verify the option-OFF build still classifies ODST unsupported, advertises no
  capabilities, and has no ODST hook plan;
- verify HEAD is a clean descendant of reviewed core commit `7c25a1a`;
- use `deploy-odst-private.bat` with MCC closed; do not route an ON cache through
  `deploy.bat auto`;
- byte-compare the privately deployed DLL and record its hash/build stamp.

### Dedicated private deployment procedure

The separate script is intentionally inert without its opt-in token:

```powershell
.\deploy-odst-private.bat VERIFY-ODST-TEST auto
.\deploy-odst-private.bat I-APPROVE-ODST-TEST auto
```

`VERIFY-ODST-TEST` exercises every identity, configuration, build, and test
gate, then exits before creating a backup or touching the installed files.
`I-APPROVE-ODST-TEST` repeats those same gates before the private copy.

Before copying anything, it verifies the reviewed branch/commit ancestry, a
clean worktree, MCC and the launcher being closed, exact x64 OFF and ON caches,
fresh Release builds, and both named CTest runs. It also requires the evidenced
retail `halo3odst.dll` SHA-256
`5BB20976EFDFD9E1CE59C589339804725FEC239021027C8D65B2733EAB94829A`.

The currently installed headset-confirmed DLL is
`0BD0233CD28975CADFCE7E03F9B9CA353CD533CD37D257FDCA362983D00B11BA`.
A new OFF-tree build is not byte-identical, so recovery must restore those exact
installed bytes rather than rebuild. The script copies that DLL under
`Halo_MCC_VR\pre-odst-private-backup` and seals the source commit, baseline and
candidate hashes, candidate size/time, and exact manifest before overwrite. Its
exclusive recovery states advance `PREPARED -> DEPLOYING -> ACTIVE`; restore or
rollback enters `RESTORING` before copying the baseline and finishes as
`RESTORED` or `ROLLED_BACK`. An interrupted `DEPLOYING`/`RESTORING` copy can
therefore be retried even if the destination contains partial bytes, while a
normal `ACTIVE` state refuses to overwrite an unrelated later DLL. The script
stages and byte-checks the private DLL in that directory and deploys only that
DLL. The launcher is left untouched and must retain
`BDC0A20F56DF72CDDE68E5D0AB621321FBDE91DA427B6C24142B38336D33EA6D`.
It never starts MCC. A staging/copy/verification failure attempts to restore the
saved baseline and prints `DO NOT LAUNCH OR TEST`.

Record the script's complete success block before launch. On any failed test,
close MCC and restore immediately. After a successful ODST checklist, run the
protected Halo 3 regression while the private DLL is still installed, then close
MCC and restore the exact saved baseline:

```powershell
.\deploy-odst-private.bat RESTORE-ODST-BASELINE auto
```

The restore mode validates the exclusive recovery state and sealed record,
restores only the DLL, and byte/hash-compares it. Preserve the backup directory
as the test record; do not delete or overwrite it merely to silence the private
script's repeat-run guard.

### Expected scope

This is deliberately not finished ODST support. Expect stock physical-controller
input, not motion-controller weapon aiming. There is no ODST VR reticle, HUD/VISR
port, scope port, weapon/hand calibration, arm IK, or gameplay patch. The only
candidate behavior is stereo world rendering, rotational tracking, positional
6DOF, and minimum first-person camera coherence. The `1 / 3.048` position scale
may be wrong; report direction and perceived magnitude rather than treating it
as established calibration.

For the option-ON private ODST test, validate only:

1. ODST level load reaches stereo without a black screen or crash.
2. Head rotation and positional leaning produce correct 6DOF scale/direction.
3. Movement and looking remain stable; no doubled/mono world camera.
4. The first-person weapon/HUD camera is coherent in both eyes.
5. Zoom returns cleanly to the ordinary camera.
6. Death/respawn restores the camera.
7. Entering/exiting a vehicle and a cutscene never corrupts the view; stock
   fallback is acceptable for modes not enabled by the minimal checkpoint.
8. Exit to MCC, re-enter ODST, then close the title cleanly. No stale hooks,
   retained render target, or stranded VR presentation.
9. Re-run the protected Halo 3 camera/stereo/head-tracking regression before
   accepting the checkpoint.

Run items 1-4 first and report the smoke result before stressing transitions.
Only continue with items 5-9 if the initial image is stable. Include the level,
approximate test time, whether each eye rendered a coherent world, lean
direction/scale, weapon/HUD appearance, and the tail of `halo3xr.log`.

Stop immediately and revert the isolated experiment on any signature ambiguity,
partial hook state, black screen, crash, incorrect eye ownership, failed title
exit/re-entry, or Halo 3 regression. Do not stack fixes onto a failed headset
build.

## Paste-ready next-step prompt

```text
Prepare and deploy the first private ODST camera/stereo/6DOF headset test. I
explicitly approve this chat using the dedicated private deployment procedure,
but do not launch MCC for me.

Read docs/CURRENT-STATE.md, docs/CONTINUATION.md, docs/PLAN.md,
docs/ODST-SIGNATURE-EVIDENCE.md, docs/ODST-CAMERA-LAYOUT.md, and
docs/ODST-MINIMAL-BRINGUP-HANDOFF.md before changing code. Inspect the worktree
and preserve stash@{0}, which contains the user's README work. The reviewed
camera-core checkpoint is 7c25a1a; require a clean HEAD descended from it.

The private candidate now has exact build/signature/layout preflight, four
atomic camera-core hooks, fail-closed teardown/rearm, and isolated shared-feature
policy. Keep ODST runtimeSupported=false/capabilities None and keep the build
option OFF by default. Public deploy/export must continue to reject ON caches.
Controls, aim, reticle, HUD/VISR, scopes, pause, brightness, motion blur,
weapons, bones, arms, VRIK, and gameplay patches remain out of scope. Treat the
1/3.048 positional conversion as a headset calibration hypothesis, not accepted
evidence. Do not change runtime code before the first test unless a new desk-side
blocker is found.

Review deploy-odst-private.bat, confirm the recorded installed/retail hashes, and
run:

    .\deploy-odst-private.bat I-APPROVE-ODST-TEST auto

This is the only authorized private deployment path; do not weaken or bypass the
normal OFF-only deploy/export guards. The private script must preserve the
exact installed baseline, rebuild/test both OFF and ON, deploy and byte-verify
only the private DLL, leave the launcher untouched, and print all identities. Do
not launch MCC. Report the commit, DLL build time/SHA-256, verified retail and
launcher hashes, and backup path, then tell me the staged items 1-4 smoke
checklist so I can launch with anti-cheat disabled and test in the headset. On a
failure restore immediately; after a successful ODST checklist, test Halo 3
before restoring with:

    .\deploy-odst-private.bat RESTORE-ODST-BASELINE auto
```
