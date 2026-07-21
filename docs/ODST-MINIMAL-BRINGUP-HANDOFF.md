# ODST minimal camera/stereo/6DOF bring-up handoff

Prepared: 2026-07-21
Branch: `feature/odst-bringup`
Starting HEAD at handoff preparation: `543b4f0`

This is the boundary for the next chat. The signature and camera-layout evidence
gates have passed for ODST's single-user stock camera path. The next task is a
private, deliberately minimal camera/stereo/6DOF implementation—not general
ODST support.

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

Before deployment:

- configure the experimental build option explicitly;
- build Release and run CTest;
- verify the option-OFF build/tests still classify ODST unsupported and stock;
- deploy only through `deploy.bat auto` with MCC closed;
- byte-compare DLL and launcher and record their hashes/build stamp.

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

Stop immediately and revert the isolated experiment on any signature ambiguity,
partial hook state, black screen, crash, incorrect eye ownership, failed title
exit/re-entry, or Halo 3 regression. Do not stack fixes onto a failed headset
build.

## Paste-ready opening prompt for the next chat

```text
Continue the ODST port at the minimal camera/stereo/6DOF bring-up boundary.

Read docs/CURRENT-STATE.md, docs/CONTINUATION.md, docs/PLAN.md,
docs/ODST-SIGNATURE-EVIDENCE.md, docs/ODST-CAMERA-LAYOUT.md, and
docs/ODST-MINIMAL-BRINGUP-HANDOFF.md before changing code. Inspect the worktree
and preserve stash@{0}, which contains the user's README work.

The ODST signature and compact-camera/prepared-view layout gates have passed.
Implement only the private, build-gated ODST camera core described in the
handoff: title-specific unique signature preflight; ODST layout profile;
atomic all-or-stock camera hook installation; camera/stereo/6DOF plus the
minimum FP camera coherence; safe level unload, title exit, and reload. Keep
runtimeSupported=false and the experimental build option OFF by default. Do not
port controls, aim, reticle, HUD/VISR, scopes, pause, brightness, motion blur,
weapons, bones, arms, VRIK, or gameplay patches yet. Preserve the Halo 3 path
exactly. Build Release and run CTest, but do not deploy or launch until the code
and fallback behavior have been reviewed and I explicitly approve the private
headset build.
```
