# Halo 2 signature evidence

Status: **C-H2-1 is headset-accepted on Steam as of 2026-08-20. Its required
Halo 3 shared-code regression also passed, advancing the accepted development
pointer to `f8928bb`. C-H2-2's alternating-eye implementation was rejected
before headset testing and is compile-disabled. C-H2-3 same-frame stereo +
6DOF was headset-rejected for a zero-layer black screen and is now also
compile-disabled; it did not advance that pointer. C-H2-4 added a stock-screen
fail-open for every ordinary unclaimed missing exact-current pair, but a fresh
static audit found persistent post-Claim and inherited-pause black paths; it is
also compile-disabled. C-H2-5 is the unaccepted, release-enabled black-safe
successor awaiting its own headset run and Halo 3 regression.** C-H2-1 remains
the accepted read-only behavior. The machine-readable subset is
`docs/HALO2-EVIDENCE-MANIFEST.json`.

The Halo 3 player experience is the eventual target: native stereo geometry,
headset-owned orientation and position, controller aim, head-relative movement,
HUD/crosshair behavior, haptics, and failure isolation. Halo 2 is a different
engine branch. This ledger records its own route rather than copying an address,
layout, or hook boundary from another title.

## Scope and evidence rules

- Scope is the campaign/classic module `halo2.dll`, which is the repository's
  existing `GameTitle::Halo2` descriptor. H2A multiplayer uses
  `groundhog.dll`; it is a separate module/engine surface and is not covered.
- Discover semantics from the official H2EK. Retail `halo2.dll` is used only
  to match and verify the H2EK-explained system.
- A kit RVA is never a retail RVA. Cross-architecture matching requires
  semantics, layout, call topology, and unique retail bytes to agree.
- Zero/multiple runtime matches or a moved decode fail the affected stage
  closed. C-H2-1 installs no hook either way; C-H2-3/C-H2-4/C-H2-5 withhold
  their camera core.
- Accepted C-H2-1 writes no H2 engine field. Rejected C-H2-2 admitted only the
  render and raster cameras' 12-byte position spans. C-H2-3/C-H2-4/C-H2-5 own
  six 12-byte position/forward/up spans and two 4-byte vertical-FOV spans,
  restore every owned span, and never write z-far or the asymmetric/pixel-offset
  fields. The shared `render_far_clip_distance` scanner/writer is not a Halo 2
  binding and remains hard-denied.

## Pinned identities (measured 2026-08-19)

| Artifact | Identity |
| --- | --- |
| Steam `halo2.dll` | 15,807,960 B; SHA-256 `DE65B4F4FDBF3F0A5EAB7431FE530DA17DD815599182DFD6AE9B7E21CF171946` |
| Store `halo2.dll` | 15,807,960 B; SHA-256 `81E5F41A7F8409D27A5454A28BFBECB8CD273E389366FB9865DD1D01E6BE689D` |
| Shared retail PE identity | PE32+, timestamp `0x68A0F0F2`, SizeOfImage `0x02A38000`, image base `0x180000000`, version 1.3528.0.0 |
| Shared retail `.text` SHA-256 | `973245E6898940B98BECC0F16BAB116B4A544B43DFAB041DB378279B8504C0DA` |
| H2EK build | `2023.06.20.176294.1-Release`, Steam app 1613450/build 11518694 |
| H2EK `halo2_tag_test.exe` | 12,364,016 B; SHA-256 `D0B71186D3948C48DDD02E2CCB88FA13E77E25A3D8F7FA60922F23A2A0073E36`; PE timestamp `0x6491EF8A`; SizeOfImage `0x01A5E000` |

The Steam and Store retail files differ in 835 bytes/16 clusters. After zeroing
the PE checksum and Authenticode certificate blob, the complete files are
identical. Every executable-section byte is identical. See
`docs/MCC-EDITIONS-EVIDENCE.md`.

## E-H2-1: official level-liveness source

The H2EK `game_time` lifecycle row identifies one 0x2C-byte
`game_time_globals` object behind a global pointer slot:

| H2EK fact | RVA / layout |
| --- | --- |
| Pointer slot | `0xC05458` (4-byte x86 pointer) |
| Allocator | `0xCF650`; allocates and zeroes exactly `0x2C` |
| Level initialize | `0xCF680`; zeroes object, writes tick timing, sets `+0x00 = 1` |
| Level dispose | `0xCF4A0`; clears initialized `+0x00` |
| Tick getter | `0xCF4C0`; reads uint32 `+0x08` |
| Tick increment | `0xCF190`; increments uint32 `+0x08` |
| `game_update` | `0x6B680`; sole caller invokes increment at `0x6BB64` only when update-result bit 0 is set |

The official `game_update` entry also requires active map/BSP state. Therefore
a coherent change of `current_tick` after initialization is evidence of a
completed active-map simulation update, not merely module allocation or an
intended map load.

Proven object fields used by C-H2-1:

| Offset | Meaning |
| --- | --- |
| `+0x00` | exact boolean `initialized` |
| `+0x02` | int16 tick rate |
| `+0x04` | float seconds per tick |
| `+0x08` | uint32 current game tick |

Other bytes remain unnamed. Save restoration, whole-object initialization, and
uint32 wrap can change the tick by something other than +1, so the gate tests
`tick != previous`, not monotonicity or an exact delta.

### Retail homolog

H2EK `game_update 0x6B680` to retail `0x6A72A0` is a strong BSim match
(similarity 0.53274, significance 144.38). The retail tail is structurally
identical: it tests update-result bit 0 and calls the tick increment only when
set. The incrementer has one direct code caller, at `0x6A765D`.

Retail preserves the 0x2C object and uses an 8-byte pointer slot at RVA
`0x15FE008`. Two independent unique anchors decode that same slot:

| Anchor | Retail RVA | AOB | Disp32 | Steam mapped/raw | Store mapped/raw |
| --- | ---: | --- | ---: | ---: | ---: |
| tick increment | `0x7067F0` | `48 8B 05 ?? ?? ?? ?? FF 40 08 C3` | `+3`, next `+7` | 1 / 1 | 1 / 1 |
| level initialize | `0x706910` | `48 83 EC 28 48 8B 05 ?? ?? ?? ?? 33 C9 48 89 08 48 89 48 08 48 89 48 10 48 89 48 18 48 89 48 20 89 48 28 E8 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 0F BF 48 08 66 89 4A 02 C7 42 0C 00 00 80 3F C6 02 01` | `+7`, next `+0x0B` | 1 / 1 | 1 / 1 |

The raw 11-byte tick getter at `0x706860` has two matches and is explicitly not
a resolver. Its ambiguity is a useful negative result: semantic resemblance is
not uniqueness.

### C-H2-1 gate protocol

Before liveness, the worker reads only the two anchors at their exact pinned
RVAs, decodes both to the same in-image writable pointer slot, and samples that
slot. It does not scan the module or pin it during loading. A mismatch withholds
the observation loudly; there is no RVA fallback.

Each 50 ms sample is coherent: read slot pointer, initialized, tick,
initialized again, then slot pointer again. The sample is admitted only if both
pointers and both initialized bytes agree and initialized is exactly 0 or 1.
The object must be committed/readable through `+0x0B`. A null, unreadable, or
racy sample closes any open liveness latch, clears the consecutive-change run,
and forces a new baseline; it does not manufacture a frozen sample. Genuine
`initialized == 0` evidence is retained.

- `initialized == 0` is genuine frozen/disposed evidence.
- The first initialized value after that is only a new baseline.
- The next different tick opens the frozen-then-ticking path.
- If observation begins in a running level, 120 consecutive changed samples
  (six seconds at 50 ms) open the already-running path.
- There is no timeout or unconditional open.

Only after the gate opens does the worker take a short refcount pin and scan the
loaded image once. The result is tagged to the module generation. The bounded
clock sampling continues because MCC can keep `halo2.dll` resident through Save
& Quit and another load: a later explicit uninitialized/disposed sample closes
level liveness without repeating the image scan.

The runtime scanner uses fixed stack storage, walks committed readable image
regions, and guards reads with SEH; it neither allocates nor dereferences
inaccessible image gaps. All six function anchors are well inside retail
`.text`. Complete offline mapped-image and raw-file scans independently prove
there is no second copy elsewhere in either pinned edition.

## E-H2-2: render and camera transaction

The official H2EK explains this path:

```text
observer_update_all 0x448B0
  -> per-user observer update 0x433E0
       -> observer result derivation 0x44390
main_render 0x51B930
  -> wrapper 0x51BC50
  -> setup 0x51BF00
  -> window constructor 0x51C090
       builds camera at window+0x80
       copies exactly 0x74 bytes to window+0x0C
  -> render_frame 0x29E060
       -> render_player_window 0x29EAD0
            -> asymmetric-frustum helper 0x2A7E10
            -> render_view 0x2A0160
```

Retail preserves the transaction semantics and layout, with the setup loop
inlined into the active render path at `0x960230` before its callsite at
`0x96040E`. The standalone `0x960680` setup homolog remains an independent
semantic/layout cross-check; it is not claimed as the wrapper on that active
call edge:

| Meaning | H2EK RVA | Retail RVA | Retail AOB matches per edition |
| --- | ---: | ---: | ---: |
| render frame | `0x29E060` | `0x7E1600` | 1 |
| player-window transaction | `0x29EAD0` | `0x7E2130` | 1 |
| render view | `0x2A0160` | `0x7E30D0` | 1 |
| asymmetric-frustum helper | `0x2A7E10` | `0x7DFCD0` | 1 |
| setup | `0x51BF00` | `0x960680` | topology/layout cross-check |
| window constructor | `0x51C090` | `0x960780` | topology/layout cross-check |
| camera builder | `0x2A5FB0` | `0x7DF5A0` | topology/layout cross-check |

The exact retail entry AOBs are the four render anchors in
`src/common/halo2_render_logic.h`. C-H2-1 verifies all six lifecycle/render
anchors are unique at their pinned RVAs in the loaded image.

Retail call edges independently decode as follows:

| Callsite | Target |
| ---: | ---: |
| `0x96040E` | render frame `0x7E1600` |
| `0x7E1706` | player window `0x7E2130` |
| `0x7E2315` | frustum helper `0x7DFCD0` |
| `0x7E2412` | sole player-path render view `0x7E30D0` |

### Hook ABI and exact caller boundary

Official H2EK `render_player_window 0x29EAD0` has the logical ABI
`void(window*, byte flag)`. Its retail homolog at `0x7E2130` therefore has the
Win64 ABI:

```cpp
void __fastcall render_player_window(void* window, uint8_t flag);
// RCX = window, DL = flag
```

The existing 23-byte entry AOB
`48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 02 00 00`
matches once in both complete mapped images and once in both raw retail files.
The ordinary render-frame call is `0x7E1706`, returning to `0x7E170B`; its
H2EK source edge is `0x29E133`. Retail also calls the function from
`0x7F0A60`. That second stock caller is an explicit exclusion: C-H2-2 and the
C-H2-3 outer hook own only the exact `base+0x7E170B` return edge, and only the
primary player (`window+0x04 == 0`). Entry uniqueness alone does not identify
the transaction instance.

### Window and camera layout

Retail window stride is `0x120` (H2EK x86 is `0x118`). Retail window fields
are type `+0x00`, player index `+0x04`, output user `+0x08`, render camera
`+0x0C`, raster camera `+0x80`, and trailing view argument `+0xF8`. Each camera
is exactly `0x74` bytes.

H2EK camera builder `0x2A5FB0`, its retail homolog `0x7DF5A0`, and the
source-backed H2EK projection builder `0x2A7920` establish these fields:

| Camera offset | Proven meaning |
| ---: | --- |
| `+0x00` | position, three floats |
| `+0x0C` | forward, three floats |
| `+0x18` | up, three floats |
| `+0x28` | vertical field of view in radians |
| `+0x30` | viewport rectangle: int16 y0/x0/y1/x1 at `+0x30/+0x32/+0x34/+0x36` |
| `+0x38` | window rectangle in the same int16 order |
| `+0x40` | z-near |
| `+0x44` | z-far |
| `+0x58` | asymmetric-frustum enable byte |
| `+0x5C`, `+0x60` | normalized X/Y frustum center |
| `+0x64` | common half-extent scale |
| `+0x68` | alternate pixel-offset enable byte |
| `+0x6C`, `+0x70` | X/Y pixel offsets |

The player-window transaction legitimately writes only render-camera z-far at
`window+0x0C+0x44`. The selected position, basis, FOV, and asymmetric fields
on both cameras are read-only through that transaction. A position-disparity
experiment must write both camera positions; changing only one leaves the
render and raster consumers incoherent.

### Title-specific metric scale

Halo 2 uses 3.048 metres per world unit, hence
`1 / 3.048 = 0.3280839895` world units per metre. This is independently
established for Halo 2 rather than inherited from another title:

- H2EK's unique `3.048f` is at RVA `0x7AD4F8`. Its xref at `0x31A5B4` is
  inside source-backed function `0x31A513`, which formats the converted
  distance as `%.1fm` or `%.1fkm`.
- Both retail editions contain the unique homologous `3.048f` at raw offset
  `0xB13AF4` / RVA `0xB14CF4`. Retail homolog `0x8365D0` references it at
  `0x836D42` and preserves the metre/kilometre branch.
- The unique reciprocal at raw `0xB5B754` / RVA `0xB5C954`, referenced by the
  retail cloth homolog, is corroboration rather than the primary semantic
  proof.

For an OpenXR-space metre displacement `(x,y,z)`, the evidence-backed basis
mapping is:

```text
delta_world =
    (cross(forward, up) * x + up * y - forward * z) / 3.048
```

This establishes units and axes. It does not by itself authorize applying
tracked center-head translation.

### Exact `render_view` call contract

H2EK `render_view 0x2A0160` maps to the unique retail entry `0x7E30D0`.
The existing entry AOB was measured once in each mapped/raw pinned edition by
C-H2-1. The exact logical widths and 19-argument pass-through contract are:

```cpp
void render_view(
    uint32_t, camera*, camera*, uint8_t,
    uint32_t, uint32_t, uint8_t, float (*)[4],
    uint16_t, int32_t, uint32_t, uint8_t, uint32_t,
    void* /* 0x120 local */, uint8_t, int16_t,
    void*, uint8_t, void* /* 0x18 local */);
```

At player callsite `0x7E2412`, arguments 1-4 are passed in
`RCX/RDX/R8/R9B` as `[window+0x04]`, `window+0x0C`,
`window+0x80`, and the incoming flag. Arguments 5-19 at
`[rsp+0x20..0x90]` are, in order:

```text
EDI, EBP, SIL, &frustum, 0, [window+0x08], R12D, R15B, local32,
&local0x120, 0, -1, window+0xF8, 0, &local0x18
```

The widths, locations, and pass-through values are exact. Names and semantics
for the opaque non-camera arguments are not proven and must remain unnamed.
This prototype documents the existing transaction. Static evidence alone did
not authorize replay; C-H2-3 isolates the one exact player-path invocation and
its headset-pending inner hook calls the original with the same 19 arguments
twice, once for each current eye.

### Native asymmetric projection route

H2EK and retail both implement the title-native off-center projection controls
listed in the camera table above.

The builder clears `+0x58`, so eventual eye substitution must occur after the
builder. The player transaction reads the render camera through the helper once
before render view. Those fields are read-only through the transaction, which
supports restoring only the eye-overwritten fields afterward.

A blind 0x74-byte restore is unsafe: the player transaction may legitimately
update render-camera `z_far` at camera `+0x44`. C-H2-3 therefore never replays
the entire player transaction: the outer original runs once. Its inner hook
intercepts the one exact player-path `render_view` invocation and calls that
original twice with unchanged opaque arguments while substituting only the
scoped camera fields. `render_view` also runs visibility, scene, interface/HUD,
and debug lifecycle work, so this outer-once/inner-twice behavior remains a
headset-pending runtime candidate rather than an accepted inference from static
evidence.

### Static final-swapchain-backbuffer chain

The official H2EK establishes three distinct D3D11 targets rather than the
Halo 3 target shape:

| Meaning | H2EK evidence | Retail homolog / slot |
| --- | --- | --- |
| target initialization | `0x50D760` | `0x952780` (BSim 0.61232, significance 236.93) |
| swapchain backbuffer RTV | created from `IDXGISwapChain::GetBuffer` | pointer slot RVA `0x197EE58` |
| primary scene RTV | texture bind flags `0x28` | pointer slot RVA `0x197EE60` |
| resolved scene RTV | texture bind flags `0x28` | pointer slot RVA `0x197EE88` |
| postprocess scene | `0x50CCA0` | `0x951EC0` (BSim 0.46370, significance 132.65) |
| final-output helper | `0x544AC0` | `0x975230` (BSim 0.45259, significance 66.95) |

`0x28` is `D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE`; neither
Halo 2 scene texture has the UAV bit. Postprocess first obtains the primary and
resolved resources and copies primary to resolved, runs the post effects, then
calls the final-output helper. In its normal path the H2EK helper selects
`global_d3d_surface_backbuffer`; an alternate screenshot-target condition also
exists. Retail preserves that choice. The C-H2-3 final-output resolver is:

| Fact | Exact value |
| --- | --- |
| Runtime AOB | `48 8B 1D ?? ?? ?? ?? 48 89 B4 24 B0 00 00 00 48 8B CB 0F 29 B4 24 90 00 00 00 0F 29 BC 24 80 00 00 00` |
| Pinned match RVA | `0x975297` |
| Steam complete mapped-image matches | 1 |
| Store complete mapped-image matches | 1 |
| Decode | signed disp32 at `+3`, based at RIP `+7` |
| Required decoded slot RVA | `0x197EE58` |

The fixed instruction bytes at the match begin
`48 8B 1D BA 9B 00 01`; the decode lands on the exact swapchain-backbuffer RTV
slot established independently from H2EK semantics.

Retail target binder `0x965610` calls the D3D11 context vtable at `+0x108`,
the `ID3D11DeviceContext::OMSetRenderTargets` slot. The retail player path is:

```text
render_view 0x7E30D0
  -> 0x952F50
       -> 0x7E2460
            -> postprocess_scene 0x951EC0
                 -> final-output helper 0x975230
```

After postprocess returns, `render_view` binds that same backbuffer RTV twice
more for late/interface work before returning to `render_player_window`.
Therefore, on the ordinary non-screenshot path, the swapchain backbuffer holds
the postprocessed scene plus late/interface output when the original
player-window call returns. C-H2-2 used that fact for its rejected Present-side
temporal copy. C-H2-3 instead proves the unique resolver above at install time,
borrows the exact final backbuffer RTV, and redirects each inner eye render into
its preallocated eye cache. This is not proof that an internal scene target can
be redirected safely, and C-H2-3 does not attempt one.

#### Explicit Halo 3 shape exclusion

Halo 3's learned scene-target rule requires a full-size
`DXGI_FORMAT_R8G8B8A8_TYPELESS` texture with bind flags
`RTV | SRV | UAV = 0xA8`. That rule is Halo 3 evidence only. Halo 2's proven
primary and resolved textures use `0x28` and have no UAV binding, so they do
not match it. Halo 2 must never fall through the generic Halo 3 exact-shape
discovery path. C-H2-2 and C-H2-3 both avoid scene-target discovery and
redirection entirely.

## Rejected C-H2-2 implementation: temporal stereo

C-H2-2 was implemented but rejected before headset testing because alternating
eyes divided the per-eye image update rate in half. It is compile-disabled,
retained only as dormant evidence, and is not accepted or part of the
current-state pointer. Its transaction was:

1. Admit only after the existing Halo 2 liveness and exact anchor proofs.
   Hook the unique `render_player_window` entry, but claim only the ordinary
   `base+0x7E170B` return edge and primary-player `window+0x04 == 0`.
2. Select the eye from the exact prepared OpenXR frame serial's parity, never
   from callback count. Apply only that eye's center-relative stereo
   displacement, at `1/3.048` scale, to the 12-byte position at both
   `window+0x0C+0x00` and `window+0x80+0x00`.
3. Call the original exactly once. Restore only those two position triples,
   preserving render-camera z-far `+0x44` and every other byte. Do not write
   forward/up, FOV, asymmetric fields, or tracked center-head translation.
4. Publish only a POD/atomic completion marker from the player hook. The
   established Present-side path consumes that marker and copies the completed
   current swapchain backbuffer into the preallocated cache for the selected
   eye. No D3D/COM call, allocation, lock, logging, file I/O, or signature scan
   belongs in the player hook.
5. Form a pair only from the current and immediately previous alternating eye
   serials in the same title/session/resource generation. Duplicate primary
   calls, split-player input, an invalid camera or prepared build, an exception,
   resize, title/session transition, or missing adjacent eye revokes that
   serial. The XR frame is dropped; the engine transaction stays armed and
   stock behavior continues.

Texture caches and resource-generation state must be created outside the hot
hook. The first frame, incomplete pair, or resize miss cannot be claimed and
must fail open without redirecting or modifying the game's D3D targets.

This produced one eye sample per game frame and never duplicated
`render_view` or the player-window transaction. At 90 rendered game frames per
second, each eye therefore updates at only 45 Hz, and the two images are
separated by one game frame. Moving objects and camera motion can show temporal
mismatch, judder, or shimmer. It is a proof-stage temporal stereo design, not
simultaneous stereo and not Halo 3 experience parity.

C-H2-2 deliberately does **not** provide 6DOF: it omits tracked center-head
translation and does not write camera orientation. It also does not yet claim
head rotation, native per-eye FOV/asymmetric projection, controller aim,
head-relative movement, HUD/crosshair parity, haptics, or a scene-target
redirect. Each is a later isolated feature transaction. Only a target-title
headset result can accept even the narrow temporal-stereo claim.

This rejected proof candidate started presentation automatically whenever the
exact Halo 2 hook owned a live level; it did not honor `auto_vr` or the manual
presentation veto.

## Rejected C-H2-3 candidate: same-frame stereo + 6DOF

**C-H2-3 was headset-rejected and compile-disabled by standalone revert
`eda5762`. It is not accepted, and the accepted pointer remains C-H2-1 at
`f8928bb`.** That revert set `HALOMCCVR_HALO2_STEREO6DOF=OFF`; C-H2-4 later
reused the gate for its no-pair fallback and was itself disabled after audit.
C-H2-5 now reuses the gate only with the black-safe contract below. The rejected
temporal gate remains OFF. The registry advertises exactly
`Stereo | RoomScale | RuntimeModes`, uses hook plan `Halo2StereoCore`, and
retains zero controller admission.

### Two-hook outer-once/inner-twice transaction

C-H2-3 installs exactly two MinHook targets only after C-H2-1 liveness and all
runtime evidence checks succeed:

| Hook | Entry RVA | Claimed return edge | Runtime role |
| --- | ---: | ---: | --- |
| outer `render_player_window` | `0x7E2130` | `0x7E170B` after call `0x7E1706` | validate primary-player window/current snapshot and call the outer original exactly once |
| inner `render_view` | `0x7E30D0` | `0x7E2417` after call `0x7E2412` | intercept the outer transaction's one exact player render and call the inner original exactly twice, once per current eye |

The inner hook additionally requires argument 1/player index 0, argument 4/flag
0, and the exact render/raster camera pointers from the live outer window.
Foreign callers and nested player transactions stay stock. A second exact inner
invocation inside the same outer attempt invalidates the attempt rather than
creating a third render.

The outer scope borrows only the exact swapchain-backbuffer RTV resolved by the
unique final-output AOB above. Preallocated per-eye RTVs receive the two final
inner outputs. The code does not discover or redirect either Halo 2 scene
texture and never uses Halo 3's incompatible `0xA8` target-shape rule.

### Mechanical same-frame/current-serial contract

One published pair must satisfy all of the following at once:

- The module generation, OpenXR resource epoch, outer-attempt token, and current
  prepared serial remain unchanged through the transaction.
- The left and right **render serials both equal the current prepared serial**.
- The left and right **capture serials both equal the current prepared serial**.
- Eye render count is exactly 2, fresh-eye count is exactly 2, and the completed
  eye mask is exactly left+right. Missing or duplicate eyes are rejected.
- Both inner originals return, both redirected outputs complete, and all owned
  camera spans are restored before pair publication.
- An N-1 eye, future eye, mixed generation/resource epoch/attempt, incomplete
  pair, stale snapshot, or repeated prepared serial drops only that frame.

Temporal pairing is forbidden: `temporal_previous_eye_allowed=false`, the eye
gap is zero, and no previous-frame eye cache can satisfy the pair. There is no
intentional cadence division. Both fresh eyes are rendered within every claimed
game frame, so each eye's intended render cadence equals the game-frame cadence
at 72, 80, 90, 120, and 144 Hz (`intentional_cadence_divisor=1`). This is the
same player-visible same-frame rule used by Halo 3, ODST, and Reach, implemented
through Halo 2's own two-hook transaction.

### Headset pose and exact engine-write scope

C-H2-3 owns full headset rotation and translation. A generation/reset-stable
head reference converts the current head orientation and displacement plus the
current eye orientation/offset into each Halo 2 render and raster camera. The
title-specific mapping remains:

```text
delta_world =
    (cross(forward, up) * x + up * y - forward * z) / 3.048
```

All quaternion, translation, eye-offset, basis, and bounds checks fail closed
for that frame. The only engine fields C-H2-3 may write are:

| Camera | Field | Window-relative offset | Bytes |
| --- | --- | ---: | ---: |
| render (`window+0x0C`) | position | `0x0C+0x00` | 12 |
| render | forward | `0x0C+0x0C` | 12 |
| render | up | `0x0C+0x18` | 12 |
| render | vertical FOV | `0x0C+0x28` | 4 |
| raster (`window+0x80`) | position | `0x80+0x00` | 12 |
| raster | forward | `0x80+0x0C` | 12 |
| raster | up | `0x80+0x18` | 12 |
| raster | vertical FOV | `0x80+0x28` | 4 |

Thus the write scope is exactly six independent 12-byte pose spans plus two
independent 4-byte vertical-FOV spans. Each is marked dirty before its guarded
write and restored independently. A whole-camera restore is forbidden because
it could overwrite the engine's legitimate render-camera z-far update.

The FOV transaction derives a finite symmetric binocular cover from the widest
left/right/up/down runtime-eye tangents, each camera's proven stock tangent
aspect, and a small finite margin. It writes only full vertical FOV `+0x28`.
This is a title-native **symmetric cover**, not exact native asymmetric per-eye
projection: `+0x58/+0x5C/+0x60/+0x64` and pixel-offset
`+0x68/+0x6C/+0x70` remain untouched, as does z-far `+0x44`.

### Headset rejection (2026-08-20)

| Item | Measured result |
| --- | --- |
| Source/package | `d82c6844072a72f12dc116f8eeb52a0045cb811d` / `d82c684-halo2-c3-stereo6dof-20260820-120451679Z` |
| Installed DLL | `7600D9135B3EE759EC612EC5B7F13F5BDDCD37C1C062594751704C58103606DB` in both Steam and Store |
| Headset run | Steam, SteamVR/OpenXR 2.17.7, `SteamVR/OpenXR : oculus`, 90 Hz |
| Preserved log | `out/test-runs/d82c684-halo2-c3-black-20260820-0855/HaloMCCVR.log`, SHA-256 `FACD98366720250CAA53FE05C718FAF675E1F136C094373EB51F8A1446D4EEA9` |
| User result | black screen in the headset |

The H2 gate passed and both hooks reported installed, but all core callback
counters remained zero. Fifteen two-second reports recorded zero submitted
pairs and zero post-claim drops; no outer callback, inner callback, eye capture,
or upload occurred. Once C-H2-3 enabled stereo presentation, ten status reports
showed `layers=0`. The OpenXR session remained focused and the final cadence was
90 fps / 90 Hz, so this was not temporal or half-rate stereo. The direct cause
was fail-closed presentation: an H2 frame without a complete pair could enter
the stereo branch, suppress the stock screen quad, and submit no world layer.
The selected player-window path may be scene-specific; this run does not prove
that it is globally dead.

Any successor must keep stock flat presentation for every ordinary unclaimed
no-pair frame, including loading and cinematics, while still forbidding N-1 eye
reuse and dropping a partially claimed failed-eye transaction. It must log the
first outer/inner callback and cannot claim stereo active until a full
current-serial pair reaches `xrEndFrame`.

### Explicit nonclaims and acceptance debt

C-H2-3 attempted stereo, headset rotation, and headset translation/room scale. It
does **not** claim controller input/admission, controller aim, head-relative
movement input, HUD/crosshair behavior, haptics, arm IK, cutscene theatre,
scene-target redirection, native asymmetric per-eye projection, or generic
draw-distance writes. Before a pair is claimed, failure calls stock once. After
scoped writes begin, failure restores every dirty span, invalidates/drops only
the current pair, and keeps the camera core and OpenXR session armed.

Any successor still requires a Halo 2 headset run recording edition, runtime,
headset, refresh rate, source/package identity, installed DLL SHA-256, and logs
that prove two current-serial eye renders/captures per game frame with no
half-rate cadence. The same exact DLL then requires a Halo 3 headset regression.
Until both results pass, no accepted-build pointer or C-H2-1 acceptance field
may advance.

## Audit-rejected C-H2-4 successor: no-pair stock-screen fail-open

**C-H2-4 is static-audit-rejected and compile-disabled. It is not accepted, and
the accepted pointer remains C-H2-1 at `f8928bb`.** It retained C-H2-3's two proven
hook targets, exact write/restore allow-list, headset pose mapping, and
same-frame pair validator. It adds no Halo 2 binding, engine write, controller
feature, or temporal eye cache.

The only player-visible delta is the ordinary missing-pair presentation choice:

- A complete pair still requires exactly two fresh renders and captures whose
  serials both equal the current prepared OpenXR serial in one game frame.
- If no H2 eye original was claimed and the complete pair is absent, the frame
  takes the stock screen-quad path:
  `unclaimed_no_pair_presentation=stock-screen`.
- That ordinary unclaimed decision does not intentionally suppress both world
  paths: `unclaimed_no_pair_intentional_zero_layer=false`. Loading, cinematics,
  and a scene that never calls the selected player-window hook therefore retain
  flat presentation while the shared screen chain is healthy.
- The stock screen is a flat fail-open presentation, not an eye cache. It never
  counts as a left or right eye, never publishes a stereo pair or Gameplay
  heartbeat, and cannot admit an N-1/current or all-N-1 pair.
- Immediately before the first original eye render, C-H2-4 publishes a
  resource-free generation- and process-monotonic-serial-tagged `Claimed`
  disposition; a finished exact pair upgrades it to `Complete`. The durable
  stamp survives a late resource reset, while every RTV/cache/token is still
  revoked. Without a live complete pair, either exact disposition remains
  `drop-frame`: it cannot borrow the unclaimed flat fallback or promote a prior
  eye cache into stereo.
- The audit found two blockers before a headset run: an inherited foreign pause
  presentation could repeatedly force complete H2 frames to Drop, and a
  systematic failure after the first eye original was claimed could repeat the
  claimed-frame Drop indefinitely. A successor must let the touched frame drop,
  quarantine only H2 stereo for that module generation, and return later
  untouched frames to the stock screen. It must also prevent H2 claims until
  inherited pause target/current state has cleared.

The simultaneous path remains refresh-invariant: two fresh eyes per game frame,
`temporal_previous_eye_allowed=false`, `temporal_eye_gap_frames=0`, and
`intentional_cadence_divisor=1` at 72, 80, 90, 120, and 144 Hz. Full headset
rotation and translation remain enabled only for that exact current pair.

Packaging advances in lockstep to manifest schema 12 and slug
`halo2-c4-no-pair-fail-open`. The producer and installer both require
`unclaimed_no_pair_presentation=stock-screen`,
`unclaimed_no_pair_intentional_zero_layer=false`,
`claimed_partial_pair_presentation=drop-frame`, the
same-frame/two-fresh-eye/current-serial fields above, no temporal reuse, divisor
1, full 6DOF, and the exact refresh list. The runtime claim line is:

```text
Halo 2 C-H2-4 simultaneous stereo + 6DOF active
```

No C-H2-4 headset result is accepted. Its successor must pass the fallback,
same-frame stereo, full headset rotation/translation, and full-rate headset
checks, followed by the Halo 3 shared-code regression.

## Unaccepted C-H2-5 successor: black-safe stereo + 6DOF

**C-H2-5 is release-enabled for a target-title headset run, but is not
accepted. The accepted pointer remains C-H2-1 at `f8928bb`.** It changes no
signature, caller edge, camera layout, engine-write span, pose mapping, or pair
identity rule from C-H2-3/C-H2-4. It retains two fresh left/right renders and
captures from one game frame, requires both eye serials to equal the current
prepared OpenXR serial, forbids any temporal/N-1 eye, and keeps the intentional
cadence divisor at 1. Headset rotation and translation remain part of both eyes'
restored camera transactions.

### No claim while inherited pause or unsupported app cadence is active

C-H2-5 clears any pause/head-lock presentation target inherited from another
title before allowing an H2 hook to claim. The render core's admission atomic
remains false until both the pause target and current pause presentation are
clear, so the comfort-fade interval itself stays unclaimed and uses the stock
screen path.

H2 stereo admission requires two timing witnesses from the current prepared
serial. Both the current `xrWaitFrame.predictedDisplayPeriod` target and the
delta between that serial's `predictedDisplayTime` and its predecessor must be
inside the exact integer-nanosecond interval `6,944,444..13,888,889`, the nearest
integer representations of nominal inclusive 144–72 Hz. The only rounding is
the sub-nanosecond representation inherent in those endpoints; there is no
±0.5 Hz tolerance. This second witness detects half-rate delivery even when the
runtime continues to advertise a 90 Hz target:

| C-H2-5 app-cadence contract | Value |
| --- | --- |
| Sources | current `xrWaitFrame.predictedDisplayPeriod` **and** same prepared serial's `predictedDisplayTime` delta |
| Nominal minimum / maximum | 72 Hz / 144 Hz |
| Accepted period bounds | `6,944,444..13,888,889 ns` for both witnesses |
| 90 Hz target + `22,222,222 ns` delivery delta | unclaimed stock-screen before either eye renders |
| 45 Hz or 60 Hz | unclaimed stock-screen before either eye renders |
| unknown, zero, or outside either bound | unclaimed stock-screen before either eye renders |

This gate prevents the mod from intentionally selecting, or accepting a runtime
target for, below-72-Hz H2 stereo. It is not a static promise that the GPU will
finish every frame at the target cadence. The headset result must prove the
actual observed application cadence is 72–144 Hz and that both eyes update on
every admitted game frame.

After the first completed pair, cadence continuity is also mechanical: every
attempted pair's prepared serial must equal the previous completed serial plus
one. A duplicate, skipped, missing, or wrapped serial publishes the named
`CorePreparedSerialGap` generation quarantine before an original eye render;
that gap frame remains unclaimed and uses the stock screen. This prevents an
otherwise valid later pair from establishing a half-rate sequence by silently
skipping prepared frames.

### One touched drop, then generation-scoped quarantine

The first structural failure after H2 has published `Claimed` still restores
every dirty span and drops that touched frame. It then publishes a quarantine
for that exact `halo2.dll` module generation before a later callback can claim.
The worker removes only the H2 stereo core, while the durable disposition keeps
the already touched failure from masquerading as an unclaimed frame. Subsequent
untouched frames therefore use the stock screen-quad fallback instead of
repeating a claimed-frame drop. A new module generation may perform a fresh
evidence and hook-install attempt.

This is feature isolation: structural H2 failure does not detach or end the
OpenXR session and does not disable another title's path. OpenXR remains
available. The maximum number of claimed failed frames before quarantine is 1;
the touched failure frame is `drop`, and subsequent untouched frames are
`stock-screen`.

### Strict unclaimed stock-screen transaction

Every active-H2 unclaimed fallback, including detached/quarantined H2 frames,
uses a title-specific strict screen transaction. Swapchain acquire, wait, and
release must each return exact `XR_SUCCESS`; `XR_SUCCEEDED` is insufficient.
The acquired index, image resource, and RTV must be valid, and the backbuffer
`Blit` must return success before the quad can be queued.

Any failure enters the named OpenXR session-recovery path
`EnterFrameWaitFatalDrain`. The begun frame is paired with an empty
`xrEndFrame`, no world layer is submitted, and the same session does not retry a
possibly poisoned acquire/wait/release transaction. This OpenXR-transaction
recovery is distinct from an H2 structural-core failure: the latter quarantines
only H2 stereo and leaves a healthy OpenXR session available.

### Package and acceptance contract

Packaging advances in lockstep to manifest schema 14 and slug
`halo2-c5-black-safe-stereo6dof`. Producer and installer require the exact build
identity `Halo2=SAME_FRAME_6DOF_FAIL_OPEN`, all C-H2-4 exact-pair/no-temporal/
divisor-1/full-6DOF fields, and these additional typed fields:

```text
foreign_pause_cleared_before_claim=true
app_cadence_gate_hz.min=72
app_cadence_gate_hz.max=144
app_cadence_gate_hz.source=current xrWaitFrame predictedDisplayPeriod and same prepared serial predictedDisplayTime delta
app_cadence_gate_hz.target_period_source=current xrWaitFrame predictedDisplayPeriod
app_cadence_gate_hz.delivered_delta_source=same prepared serial predictedDisplayTime delta
app_cadence_gate_hz.period_ns_min=6944444
app_cadence_gate_hz.period_ns_max=13888889
app_cadence_gate_hz.both_witnesses_required=true
app_cadence_gate_hz.hz_tolerance=0
app_cadence_gate_hz.at_45_hz=unclaimed-stock-screen-before-eye-render
app_cadence_gate_hz.at_60_hz=unclaimed-stock-screen-before-eye-render
app_cadence_gate_hz.unknown_or_outside=unclaimed-stock-screen-before-eye-render
app_cadence_gate_hz.target_90_hz_delta_22222222_ns=unclaimed-stock-screen-before-eye-render
post_first_complete_serial_policy=previous-completed-serial-plus-one
serial_gap_quarantine_reason=CorePreparedSerialGap
serial_gap_quarantines_before_eye_render=true
serial_gap_frame_presentation=unclaimed-stock-screen
post_claim_failure_quarantine=true
quarantine_scope=module-generation
max_claimed_failed_frames_before_quarantine=1
touched_failure_frame=drop
subsequent_untouched_frames=stock-screen
openxr_remains_available_for_structural_halo2_failure=true
strict_unclaimed_stock_screen_transaction.acquire_result=XR_SUCCESS
strict_unclaimed_stock_screen_transaction.wait_result=XR_SUCCESS
strict_unclaimed_stock_screen_transaction.release_result=XR_SUCCESS
strict_unclaimed_stock_screen_transaction.valid_resource_and_rtv_required=true
strict_unclaimed_stock_screen_transaction.blit_success_required=true
strict_unclaimed_stock_screen_transaction.named_session_recovery=EnterFrameWaitFatalDrain
strict_unclaimed_stock_screen_transaction.repeated_same_session_retry=false
```

The exact active line is:

```text
Halo 2 C-H2-5 simultaneous stereo + 6DOF active
```

It may be emitted only after a complete exact-current pair survives
`xrEndFrame`; install/build success alone cannot produce the player-facing
claim. The headset run must record edition, runtime, headset, configured panel
rate, measured app cadence, source/package identity, installed DLL SHA-256, and
the preserved log. It must demonstrate visible unclaimed fallback, true
same-frame stereo, full headset rotation/translation, no 45/60-Hz H2 stereo,
and actual 72–144 Hz. The same exact DLL then requires a Halo 3 headset
regression before any accepted pointer advances.

## Accepted C-H2-1 runtime contract

- Existing registry row/slot only; no new title descriptor or alias. These are
  the accepted C-H2-1 behavior settings; C-H2-2 through C-H2-5 change only
  their separately gated hook/capability/heartbeat fields described above.
- `runtimeSupported=false`, runtime capabilities none, admission capabilities
  none, hook plan none, heartbeat window zero.
- Shared controller merge remains denied.
- No MinHook call and no game function call.
- No engine write. The generic draw-distance scan/write is specifically denied
  while Halo 2 is active.
- Before level proof: two bounded exact-RVA code comparisons plus coherent
  reads of one official engine clock object.
- After level proof: one generation-tagged module pin/full-image scan. Only the
  bounded clock sample continues; the same module instance never scans twice.
- Failure changes only the log and keeps Halo 2 stock.

Expected successful log sequence:

```text
Title adapter: detected Halo 2 Anniversary (halo2.dll); C-H2-1 read-only cold observation is armed ...
Halo 2 level-load gate armed: ... game_time_globals pointer slot RVA 0x15FE008 ...
Halo 2 level-load gate: ... level running
Halo 2 cold observation PASS (C-H2-1): ...
```

The headset run must record edition, OpenXR runtime, headset, refresh rate,
source commit, and installed DLL SHA-256. A PASS advances only to designing the
stereo candidate; it does not itself constitute stereo or 6DOF acceptance.

## C-H2-1 target-title headset/log PASS (2026-08-20)

| Accepted claim identity | Value |
| --- | --- |
| Source | `f8928bbc25ee3ad90195cb32a1d9d41d767e4ed1` |
| Package | `f8928bb-halo2-c1-cold-observation-20260820-035247738Z` |
| `HaloMCCVR.dll` | `A9F384F26FE2E313AA7037A3A8C250839AE0D9950FD3A1D4414268225EAD5CF9` |
| `HaloMCCVRLauncher.exe` | `DC18587C6A7CBC6FF9A274703057C299AB2334CFD0E22C5CF7702D66AF9813BC` |
| Steam `HaloMCCVR.log` | `FDEEC3D8A2C68C05BCFEE07D54E0E4B41EE7AED72E99575DD7FF109F1EC09896` |
| Run | Steam; SteamVR/OpenXR 2.17.7; `SteamVR/OpenXR : oculus`; 90 Hz |
| Store edition | same artifact installed and hash-verified; not headset-run |

The run exercised two independent Halo 2 module generations. Generation 1
PASSed at `00:39:38.356`; generation 3 PASSed at `00:41:14.816`. Each proved
the pinned PE timestamp and `SizeOfImage`, six of six unique anchors at their
pinned RVAs, and two agreeing game-time decodes to pointer-slot RVA
`0x15FE008`. The log contains two PASSes and zero FAIL, WITHHELD, zero-match,
multiple-match, moved-anchor, bad-decode, pin-failure, or scan-failure lines.

Four explicit disposed/uninitialized observations closed level liveness. Two
same-generation reopens earned a fresh baseline and tick without repeating the
one-shot full-image scan. One initial incoherent clock read in each title
generation recovered as designed. Visible `1.829 s` and `1.391 s` game-load
stalls ended before either gate opened and before either full-image scan. The
first generation mostly reported 45 Hz and the later generation sustained
approximately 88–90 Hz after loading, but C-H2-1 owns no Halo 2 render path and
cannot establish a render-performance cause.

This accepts the Halo 2 read-only observation claim and clears its evidence for
stereo design. That C-H2-1 result did not accept C-H2-3's rejected hook/write or
stereo/6DOF runtime claim, C-H2-4's audit-rejected fallback successor, or the
C-H2-5 black-safe headset candidate. C-H2-5 still needs its own headset result
and Halo 3 regression.

### Required Halo 3 shared-code regression PASS

The same installed source and DLL were subsequently exercised in Halo 3 in a
Steam run whose complete log SHA-256 is
`9F4DD986C09DA3CFA8A3691E1D2B770BDFEF4003A9E0BDCE2C922955C58B0813`
(177,832 bytes, `00:54:33`–`00:58:22` local). The build identity remained
source `f8928bbc25ee3ad90195cb32a1d9d41d767e4ed1`, DLL SHA-256
`A9F384F26FE2E313AA7037A3A8C250839AE0D9950FD3A1D4414268225EAD5CF9`,
SteamVR/OpenXR 2.17.7, `SteamVR/OpenXR : oculus`, and 90 Hz.

Halo 3 became the unique title at `00:54:50.280`. Its proven level gate opened,
all camera/FP/render hooks installed, Runtime mode entered gameplay, positional
6DOF and stereo armed at `00:54:58.758`, and the exact-shape Halo 3 scene target
was learned. From `00:55:03` through title teardown the log repeatedly reports
90 fps / 90 Hz stereo, 89.9–90.0 HMD pose samples per second, zero duplicate
frames, zero frame-order failures, and zero stalls. Stereo disarmed normally on
the title transition at `00:56:00.146`. The user reported that Halo 3 worked
fine. This closes the shared title-worker/lifecycle regression debt and advances
the accepted development pointer to `f8928bb`.

## Offline verification method

Both retail PE files were parsed into complete mapped images: headers and each
section's raw bytes were copied to `VirtualAddress`, virtual gaps were zeroed
through `SizeOfImage`, and every possible RVA was scanned. Raw files were also
scanned independently, including certificate/overlay bytes. Function semantics
and call topology were inspected in offline Ghidra projects under ignored
`out/ghidra`; no MCC process was launched or accessed.
