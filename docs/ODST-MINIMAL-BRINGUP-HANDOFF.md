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
- The private build permits ordinary virtual-gamepad buttons and sticks only
  while it explicitly owns ODST, so MCC menus and ODST remain navigable.
  Motion-controller aim, head-relative movement transforms, reticle suppression,
  HUD/VISR changes, scopes, pause, brightness, motion blur, weapons, bones, arms,
  VRIK, and all gameplay patches remain off. The public option-OFF build and
  shared gameplay paths remain fail-closed outside their proven context.
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
  locally. The private Release hook wrappers also pass their unwind-metadata
  check.

## First private headset result and recovery

The first private candidate was deployed from source commit `bccf4c7` with DLL
SHA-256
`533CE571B6AD0E955F1722DFF1341EE77A02184C1705D2616630C577BF34B103`.
It failed the initial smoke test: MCC menu VR controls stopped merging, ODST
loaded only as a stock 2D image with no hook/tracking/stereo, and Halo 3
performance regressed considerably. MCC was closed and the dedicated restore
mode restored the exact headset-confirmed baseline DLL
`0BD0233CD28975CADFCE7E03F9B9CA353CD533CD37D257FDCA362983D00B11BA`.
The sealed backup at
`Halo_MCC_VR\pre-odst-private-backup` remains the recovery record and must not
be removed or overwritten. `stash@{0}` also remains untouched.

The failed-run log separated the symptoms:

- ODST passed static signature/layout preflight but remained at the camera
  readiness wait, so no ODST detours were installed and stereo stayed off. The
  gate had required the nested FP source pointer to be published already and
  every byte in three inactive camera objects to be zero. The next checkpoint
  accepts a not-yet-published null nested source, still rejects any unexpected
  non-null owner, and uses the authoritative single-user tail plus absence of
  another active compact camera instead of demanding zero constructor storage.
  A one-shot readiness dump now records the complete tail, compact mode/FOV/
  clip fields, nested-source ownership, and inactive-slot activity if the
  stricter compact-camera proof still waits.
- The input path used the shared gameplay policy for MCC frontend controller
  merging. A private-build-only frontend ambiguity permission now restores menu
  navigation while explicit ODST camera ownership, teardown, and explicitly
  unsupported titles remain fail-closed. The normal option-OFF build preserves
  its prior ambiguous-title policy.
- Halo 3 emitted 4,328 synchronous reticle-clear log records in a 4,883-line
  log and repeatedly repainted the reticle swapchain. It also reported about
  532 full first-person palette solves per second at roughly 89 FPS. The next
  checkpoint retains an authored reticle across a two-frame capture gap, logs
  only the actual authored-to-cleared transition, and activates an exact-input,
  per-stereo-pair palette cache. Any changed tag, slot, bone map, config,
  original palette, or unsafe transform falls through to the existing full
  solve; the cache never spans frames.

## Second private headset result and recovery

The retry from source commit `dcdf49e`, DLL
`28BEC371529D826B8F89544F77B92EB00D46B9319E6C100CD1CED3130B3870DE`,
restored MCC frontend controller merging and removed the Halo 3 regression.
The user reported Halo 3 was fine, but ODST did not change and its controls did
not respond. The log proved VR controller edges and frontend XInput merging
(`merged=33404`), then the explicit ODST camera-only policy stopped merging
those controls. ODST therefore never reached a campaign level. Its camera array
remained in the stock unloaded state: an all-zero tail, compact camera, nested
source, FOV, and clips. This is evidence of a blocked pre-level input path, not
evidence that the proven gameplay camera-array RVA is wrong.

MCC was closed and the dedicated restore mode byte-restored the exact baseline.
The sealed record is `Halo_MCC_VR\pre-odst-private-backup-3`. The next
single-hypothesis checkpoint permits ordinary virtual-gamepad buttons/sticks
only while the private build explicitly owns ODST. Motion-controller aim,
head-relative Halo 3 movement transforms, shared gameplay features, and public
option-OFF behavior remain blocked. That isolated policy change was committed
and became the third private headset test recorded below.

## Third private headset result and recovery

The controller-only retry from source commit
`6e37807cbc2f3d531c3c5da3c9159d470736c632`, DLL
`7E711A3AEF33080471E82BC6B447173CE81FF91372DEC0D7EC9A8F30C1AEDC79`,
made MCC and ODST controls work and preserved the corrected Halo 3 performance.
After a long initial wait, the ODST camera core installed and produced distinct
stereo eyes, rotational tracking, and positional 6DOF. The headset test exposed
a repeated 2D/3D presentation cycle during gameplay, plus visible stock motion
blur and perceived input lag.

The log proves the four ODST hooks stayed installed, the exact camera array
remained valid, and true distinct-eye frames continued to validate. The cycling
came from `Game_AutoVrTick`: any camera-copy heartbeat older than the 500 ms arm
freshness threshold immediately detached presentation. The camera commonly
resumed within the separate unload watchdog's grace period, so stereo rearmed
after another one-second debounce. This created the repeated OFF/ON sequence;
it was not hook loss or a title unload.

MCC was closed and the dedicated restore mode byte-restored the exact baseline.
The sealed record is `Halo_MCC_VR\pre-odst-private-backup-4`; preserve it. The
next isolated checkpoint keeps the 500 ms continuous-heartbeat requirement for
initial arming, but an already active presentation detaches only when the same
soft/hard heartbeat watchdog has actually proved an unload. It adds regression
coverage for a transient readiness flicker below the soft timeout. Motion blur
suppression remains a separate later headset checkpoint; first verify that this
single state-machine correction removes the gameplay hopping and associated
presentation latency.

## Fourth private headset result, live comparison, and recovery

The heartbeat-stabilization retry from source commit
`cab874cc4a69fc77f194b7be10b663c3357d78e2`, DLL
`AD1619740DB200C419965B4F7D105DC0AB0AD935FDE06E8484DE3E2693CA97D4`,
kept ODST stereo continuously active at about 90 FPS; the repeated 2D/3D
presentation hopping was gone. The user reported that activation still felt
slow and, more importantly, that the resulting depth was shallow and
visually disorienting compared with the headset-confirmed Halo 3 path.

The log separates activation from scanning: ODST was detected at `13:02:24`
while its complete camera array was still the stock unloaded zero state. Once
the ordinary gameplay camera appeared at `13:03:23.903`, exact preflight and
the four-hook transaction completed within about 70 ms. Stereo armed after the
required presentation detach and fresh-camera debounce. The long visible wait
therefore includes the title's unloaded-camera interval; signature resolution
was not the minute-long operation.

MCC was closed and the dedicated restore mode byte-restored baseline
`0BD0233CD28975CADFCE7E03F9B9CA353CD533CD37D257FDCA362983D00B11BA`.
The sealed record is `Halo_MCC_VR\pre-odst-private-backup-5`; preserve it.

A read-only stock ODST capture in the same `2912x2100` render mode measured an
orthonormal camera basis, active bounds `[0,0,2100,2912]`, vertical FOV
`56.111` degrees, reference FOV `50.117` degrees, and projection scales
`P0=1.35313725`, `P5=1.87635040`. A subsequent live known-good Halo 3 run on
the same headset measured IPD `61.6 mm`, compact-camera inputs
`1.8418/1.3290`, final projection scales `P0=0.54296`, `P5=0.75246`, and a
first stereo validation with 47.3 percent changed samples / 8.371 mean RGB
delta.

The rejected ODST path already wrote the same final headset projection scales,
so final compositor FOV was not the missing difference. It widened ODST's
world-FOV input but left compact `+0x2C`, the proven first-person FOV reference,
at its stock `0.8747` value. Halo 3 instead feeds both compact inputs from the
same headset pair before rebuilding and uploading the world/FP cameras. The
next single-hypothesis checkpoint makes ODST use that exact numeric pair and
logs both per-eye compact inputs and final projection scales. This is a private
headset experiment based on live same-machine evidence, not a public-support
change. Motion-blur suppression remains separate.

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
installed bytes rather than rebuild. The first sealed record is
`Halo_MCC_VR\pre-odst-private-backup`; later attempts preserve every terminal
record and select the first unused `pre-odst-private-backup-N` directory. A new
attempt is refused if any prior record still has a live recovery state. Each
record seals the source commit, baseline and candidate hashes, candidate
size/time, and exact manifest before overwrite. Its exclusive recovery states
advance `PREPARED -> DEPLOYING -> ACTIVE`; restore or
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

The restore mode requires exactly one live recovery state across all sealed
records, validates that record, restores only the DLL, and byte/hash-compares
it. Preserve every terminal backup directory as a test record; the next private
attempt automatically uses a new numbered directory instead of deleting or
overwriting an earlier record.

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
