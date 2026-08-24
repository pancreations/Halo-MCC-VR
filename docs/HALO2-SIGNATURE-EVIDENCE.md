# Halo 2 signature evidence

Status: **C-H2-1 is headset-accepted on Steam as of 2026-08-20. Its required
Halo 3 shared-code regression also passed, advancing the accepted development
pointer to `f8928bb`. C-H2-2's alternating-eye implementation was rejected
before headset testing and is compile-disabled. C-H2-3 same-frame stereo +
6DOF was headset-rejected for a zero-layer black screen and is now also
compile-disabled; it did not advance that pointer. C-H2-4 added a stock-screen
fail-open for every ordinary unclaimed missing exact-current pair, but a fresh
static audit found persistent post-Claim and inherited-pause black paths; it is
also compile-disabled. C-H2-5 was headset-rejected after its own pre-stereo
screen check terminated a healthy OpenXR session, and is compile-disabled.
C-H2-6 corrected that false RTV/session-fatal condition but was headset-rejected
because neither installed render detour executed, and is compile-disabled.** C-H2-1 remains the accepted read-only behavior. The
machine-readable subset is
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
  closed. C-H2-1 installs no hook either way; C-H2-3/C-H2-4/C-H2-5/C-H2-6 withhold
  their camera core.
- Accepted C-H2-1 writes no H2 engine field. Rejected C-H2-2 admitted only the
  render and raster cameras' 12-byte position spans. C-H2-3/C-H2-4/C-H2-5/C-H2-6 own
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

## Headset-rejected C-H2-5 successor: false pre-stereo RTV fatal

**C-H2-5 is headset-rejected and compile-disabled. It is not accepted. The
accepted pointer remains C-H2-1 at `f8928bb`.** It changed no
signature, caller edge, camera layout, engine-write span, pose mapping, or pair
identity rule from C-H2-3/C-H2-4. It retains two fresh left/right renders and
captures from one game frame, requires both eye serials to equal the current
prepared OpenXR serial, forbids any temporal/N-1 eye, and keeps the intentional
cadence divisor at 1. Headset rotation and translation remain part of both eyes'
restored camera transactions.

The Steam run used source `c5395ccf7a9972a1913da56df0775e8b8ec862a0`,
DLL SHA-256 `8F3FDE784A4B28F8C77C0930ABB3DC898099D10505292C83383E18AEAF140635`,
SteamVR/OpenXR 2.17.7, `SteamVR/OpenXR : oculus`, and a 90 Hz panel. The
session was focused and the headset remained connected. At the first Halo 2
loading frame, the screen copy had the already-proven equal-size,
equal-format-family `CopyResource` shape, whose fast path does not consume an
RTV. C-H2-5 nevertheless required a non-null RTV, entered
`EnterFrameWaitFatalDrain`, and requested `xrRequestExitSession`. The resulting
STOPPING/EXITING events produced the misleading headset-off/runtime-ended
popup. The H2 hooks installed only after the session had exited; all outer,
inner, eye, and pair counters remained zero, so this run cannot accept or
reject their gameplay behavior. The active claim line was never emitted.
The preserved log is
`out/test-runs/c5395cc-halo2-c5-false-rtv-fatal-20260820-1158/HaloMCCVR.log`,
SHA-256 `54668DFA806CF6E17C4B6DD8C63D0DFCBFE5B06591157AF5C367437AB0E07B56`.

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

## Headset-rejected C-H2-6 successor: session survived, render hooks stayed cold

**C-H2-6 is headset-rejected and compile-disabled. The accepted pointer remains
C-H2-1 at `f8928bb`.** It inherited C-H2-5's engine evidence,
two hook sites, caller edges, camera layout, eight restored write spans, pose
mapping, and pair/cadence/quarantine rules without change. Its intended
transaction had one outer game-frame scope produce exactly two fresh left/right
eye renders and captures. Both eyes had to use the exact current prepared OpenXR
serial; temporal, previous-frame, and N-1 eye reuse remain forbidden. Full
headset quaternion rotation and translation are applied independently to both
render and raster cameras. The intentional cadence divisor remains 1.

The Steam run used source `628c37bd9a5437ed35a7a1696e266fc8612452ab`,
DLL SHA-256 `86AE0540F60064E6C86F1551D71AF878F0CF62E2BD1FD5CD42C26425C8256E2C`,
SteamVR/OpenXR 2.17.7, `SteamVR/OpenXR : oculus`, and a 90 Hz panel. The C-H2-5
session-kill defect was fixed: OpenXR stayed focused and continued submitting a
screen layer. C-H2-1 cold observation passed and C-H2-6 installed its outer and
inner hooks twice, then armed. Nevertheless, every hook telemetry sample stayed
at `outer=0 inner=0 claimed=0 eyes=0 complete=0`. No pair or 6DOF transaction
began and the active line was never emitted. The user reported that nothing
hooked. The preserved log is
`out/test-runs/628c37b-halo2-c6-zero-hook-20260820-1638/HaloMCCVR.log`, SHA-256
`0D654E1071E7870068E72E9B615A1896EAAB67BB97C512151E1AA89BF431F498`.
This result rejects the selected runtime binding, not the headset or OpenXR
session. A successor must identify the live player render route before claiming
stereo ownership.

The two current-frame cadence witnesses remain exact: both
`xrWaitFrame.predictedDisplayPeriod` and that prepared serial's delta from the
prior `predictedDisplayTime` must fall within
`6,944,444..13,888,889 ns` (nominal inclusive 72–144 Hz). There is no
tolerance band. After the first complete pair, the prepared serial must equal
the previous completed serial plus one before either eye renders. Nothing in
C-H2-6 introduces alternating eyes, a stale-eye cache, or a below-72-Hz stereo
mode.

### Only behavioral delta: consume an RTV only on the path that needs it

The C-H2-5 run established an equal-size, equal-format-family, single-sample
screen copy. `Blit` selects its direct `CopyResource` branch for that shape and
never consumes the destination RTV. C-H2-5 incorrectly rejected the otherwise
valid transaction because `GetRtv` returned null. C-H2-6 makes the validation
match the selected D3D operation:

- the source and destination textures are required on both paths;
- the equal-size/equal-format-family/single-sample direct `CopyResource` path
  does not require a destination RTV;
- the shader-blit path requires a valid destination RTV;
- a local pre-stereo D3D validation failure drops only the current frame and
  keeps the OpenXR session alive, and the next frame may retry.

The OpenXR transaction contract itself is unchanged: acquire, wait, release,
and `xrEndFrame` must each return exact `XR_SUCCESS`, `Blit` must succeed, and
an actual XR transaction failure may enter `EnterFrameWaitFatalDrain` without
retrying that unresolved XR transaction in the same session. The local D3D rule
is failure isolation, not permission to ignore a failed XR operation.
The loading/shell screen presentation is not an eye, never publishes the H2
stereo or gameplay heartbeat, and cannot satisfy this candidate's acceptance.

### C-H2-6 package and acceptance contract

Packaging advances in lockstep to manifest schema 15, evidence schema 7, slug
`halo2-c6-stereo6dof-session-fix`, and exact embedded build identity
`Halo2=SAME_FRAME_6DOF_SESSION_FIX`. The inherited typed same-frame and cadence
fields remain mandatory. The path-specific delta is represented as:

```text
strict_unclaimed_stock_screen_transaction.source_texture_required=true
strict_unclaimed_stock_screen_transaction.destination_texture_required=true
strict_unclaimed_stock_screen_transaction.end_frame_result=XR_SUCCESS
strict_unclaimed_stock_screen_transaction.fast_copy_resource.eligibility=equal-size-equal-format-family-single-sample
strict_unclaimed_stock_screen_transaction.fast_copy_resource.target_rtv_required=false
strict_unclaimed_stock_screen_transaction.shader_blit.target_rtv_required=true
strict_unclaimed_stock_screen_transaction.local_d3d_validation_failure_terminates_openxr=false
strict_unclaimed_stock_screen_transaction.local_d3d_validation_failure_presentation=drop-current-frame-keep-session
strict_unclaimed_stock_screen_transaction.local_d3d_next_frame_retry_allowed=true
strict_unclaimed_stock_screen_transaction.unresolved_xr_transaction_repeated_same_session_retry=false
pre_stereo_screen_path_counts_as_eye=false
pre_stereo_screen_path_counts_as_stereo_success=false
pre_stereo_screen_path_publishes_stereo_or_gameplay_heartbeat=false
```

The exact active line is:

```text
Halo 2 C-H2-6 simultaneous stereo + 6DOF active
```

It may be emitted only after a complete exact-current two-eye pair survives
`xrEndFrame`. The target-title headset run must prove the OpenXR session stays
alive across the H2 transition, the active line is backed by fresh same-frame
left/right output, full headset rotation and translation work, and actual
application cadence remains within 72–144 Hz. The same DLL must then pass the
Halo 3 headset regression before any accepted pointer advances.

## E-H2-3: Halo 2 ships TWO renderers, and the classic tree is gated OFF

**This is the proven root cause of every zero-callback Halo 2 render hook,
including the headset-rejected C-H2-6 result.** It was established on
2026-08-20 by offline static analysis of the pinned retail module only. The
classic addresses recorded in E-H2-2 are all correct; the code they name is
simply not executed in the graphics mode the player was using.

### The gate

`0x95FEC0` is the classic per-frame render driver. Its second instruction is a
whole-tree bail-out:

```text
0095FEC6  80 3D 2B 0E 51 00 00   cmp byte ptr [rip+0x510E2B], 0   ; -> RVA 0xE70CF8
0095FECD  0F 85 48 01 00 00      jne 0x96001B                     ; skip the classic render
```

`0x95FECD + 0x510E2B = 0xE70CF8` exactly. When that byte is nonzero, control
never reaches `0x95FF09`, so the entire chain below it is dormant: setup
`0x960230`, `render_frame 0x7E1600`, `render_player_window 0x7E2130` and
`render_view 0x7E30D0`.

### The render-mode globals

The render-mode applier at `0x511E0` writes that byte. Decompilation shows:

```c
iVar6 = DAT_180e21278;                       /* requested mode  */
if (FUN_180821eb0() != 0) iVar6 = 0;
if (iVar6 != DAT_180e21280) {                /* applied mode    */
    DAT_180e70cf8 = iVar6 != 0;              /* classic-disabled flag */
    DAT_180e21280 = iVar6;
    ...
    local_res8[0] = DAT_180e21280 == 0;      /* argument named "isLegacy" */
    ... FUN_180077490(uVar5, local_res20, "isLegacy"); ...
}
```

The meaning of `+0xE21280` is not inferred: the same function passes
`DAT_0xE21280 == 0` as the Saber SSL argument literally named `isLegacy`.

| Global RVA | Width | Proven meaning |
| ---: | --- | --- |
| `0xE21278` | int32 | requested render mode; the in-game toggle performs `x = 1 - x` inside `0x515E0` |
| `0xE21280` | int32 | applied render mode. **0 = legacy/classic, nonzero = remastered/Anniversary** |
| `0xE70CF8` | byte | 1 when the classic Blam render tree is skipped at `0x95FECD` |

`0x515E0` also calls the SSL script function `switch_render_mode` and maintains
`0xE21288`/`0xE2127C`/`0xE21274` around the transition.

### The host relationship

`halo2.dll` embeds Saber's SSL (Saber Scripting Language) bridge
`ssl.halo_manager` / `HaloMng`. Its method-name strings sit contiguously in
`.rdata` at `0xBE2BC0`-`0xBE2E18` and name the two-renderer design outright:
`switch_render_mode` (`0xBE2EB0`), `OnRenderModeSwitched` (`0xBE2ED8`),
`isLegacy` (`0xBE2EC8`), `SetRenderModeSwitchEnabled` (`0xBE2C58`),
`IsRemastered` (`0xBE2CA8`), `StartedInRemasteredMode` (`0xBE2DE8`),
`ToggleForcedLegacyFading` (`0xBE2DC8`).

Saber's GroundHog engine is the **host**; the Blam classic engine is embedded
inside it. The classic renderer `0x95FEC0` is reached through a thunk at
`0x4DC70` (`jmp 0x95FEC0`) whose pointer lives in an SSL binding vtable at
`.rdata:0xBE3078`. A depth-5 direct-call reachability sweep from all 48 slots of
the exported game-engine vtable at `0xBE3480` (obtained from `CreateGameEngine`,
export ordinal 2, `0x54730`) reaches none of `0x95FEC0`, `0x960230`, `0x7E1600`,
`0x7E2130` or `0x7E30D0`. The classic render is dispatched indirectly, as a
registered callback.

### Scope correction

`groundhog.dll` is **not** the remastered campaign renderer and is not part of
this problem. It contains zero Halo 2 campaign level names
(`01a_tutorial`, `03a_oldmombasa`, `05a_deltaapproach` all absent) and zero
Saber renderer strings. The remastered renderer is inside `halo2.dll`, evidenced
by `C:/SaberTools/tools`, `SceneBrowser64.dll`, `s3dprs`, `Saber Integ`,
`UUI Saber Preload thread`, and a render-target name table near `0xBE76F4`
holding `Frame buffer 1`, `Lightshafts radial blur quarter`, `DOF blurred`,
`Postproc half FP16` and `Fog vol. backface depth`.

### Consequence for candidates

A Halo 2 candidate must read `+0xE21280` and bind to whichever renderer is
live. Zero render-hook callbacks is not evidence that a signature is wrong.
Forcing the mode is not an acceptable substitute for binding: it would change
the player's chosen graphics without being asked.

### Recovered Halo 2 symbol assets

Halo 2 previously had no symbol source outside H2EK. Two named tables were
decoded from retail `.rdata` and are kept under ignored `out/ghidra/`:

- `h2_named_fns.txt` - 934 named HaloScript engine functions with their
  implementation RVAs, from the script registration records (each record holds a
  name pointer plus `.text` pointers at `+0x10`/`+0x18`). Examples:
  `camera_set_field_of_view` -> `0x788F20`, `camera_set_first_person` ->
  `0x788EC0`, `pvs_set_camera` -> `0x789090`, `player_camera_control` ->
  `0x7891C0`, `texture_camera_off` -> `0x784CF0`.
- `h2_globals.txt` - 1082 named engine globals from a `{name, type, storage}`
  table of stride `0x18` spanning RVA `0xE08750`-`0xE0ECA8`; 87 carry live
  storage, including `camera_fov_scale` -> `.data:0xDFF4F8` and
  `texture_camera_scale` -> `.data:0xE13678`.

RTTI is not a discovery route for this module: only 186 type descriptors exist
and all are Havok `hk*` classes.

## E-H2-4: the shared camera root, and the Blam->Saber camera hand-off

Established 2026-08-20 by offline static analysis of the pinned retail module,
cross-checked against H2EK symbols and assert text. **This is the lever that
reaches both renderers.**

### The observer, and its result

The classic engine's camera originates in `camera/observer.cpp`. H2EK proves
the shape (observer array RVA `0xB552EC`, stride `0x35C`, 4 users; `result` at
observer `+0xBC`; `observer_update_all 0x448B0` -> per-user `0x433E0` ->
`0x44390`). Retail preserves it with a grown stride:

| Retail fact | Value |
| --- | --- |
| observer array base | RVA `0x15F28B8`, stride `0x368`, 4 users |
| header / trailer signature | `'!dar'` (`0x72616421`) at observer `+0x00` and `+0x360` |
| `observer_result` accessor | `0x6F0E60(user)` returns `0x15F297C + user * 0x368` |
| therefore `result` offset | observer `+0xC4` (H2EK `+0xBC`; the x64 stride grew by `0xC`) |

`0x6F0E60` is byte-proven: `48 69 C3 68 03 00 00` (`imul rax, rbx, 0x368`) then
`48 8D 0D F5 1A F0 00` (`lea rcx, [rip+0xF01AF5]`), and
`0x6F0E87 + 0xF01AF5 = 0x15F297C`.

The `observer_result` field layout is byte-proven on **retail** from the camera
builder `0x7DF5A0`, which copies it field by field into the `0x74`-byte camera:

| `observer_result` | Bytes | Copied to camera |
| ---: | ---: | --- |
| `+0x00` position | 12 | `+0x00` |
| `+0x20` forward | 12 | `+0x0C` |
| `+0x2C` up | 12 | `+0x18` |
| `+0x38` horizontal FOV | 4 | (consumed) |
| `+0x3C` aspect ratio | 4 | (consumed; default `0x3FAAAAAB` = 4/3) |
| `+0x4C` vertical FOV | 4 | `+0x28`, after a global FOV scale |
| `+0x50` FOV ratio | 4 | `+0x2C`, after the same scale |

The three global render FOV scales are at RVA `0xE13470`, `0xE13474` and
`0xE13478`; `0x7DF5A0` selects the third for a 2:1-aspect split-screen with more
than one player. This exactly matches H2EK `render_camera_build 0x2A5FB0`
(`render/render_cameras.cpp`), whose assert text names the camera fields.

### The classic window array is at a fixed address

`0x960230(char useAlternateEntry)` builds the windows and then branches:

```c
if (useAlternateEntry) { FUN_1807E1990(); return; }
FUN_1807E1600(3, count, index, flags, &DAT_1819976E0);   /* render_frame */
```

The window array is therefore **RVA `0x19976E0`, stride `0x120`**, and
`0x960780` writes `type +0x00`, `player index +0x04`, `output user +0x08`,
builds the rasterizer camera at `window+0x80` via `0x7DF5A0`, then copies
`0x74` bytes to the render camera at `window+0x0C`. `0x960230` is reached only
from `0x95FF09` (the classic driver, dead in remastered mode) and from
`0x695E0`, a nested render-to-texture path that temporarily swaps the RTV slots
`0x197EE58`/`0x197EE60`/`0x197EE70` - consistent with the classic/remastered
cross-fade behind `ToggleForcedLegacyFading`. **The window array must not be
assumed live while the remastered renderer owns the frame.**

### The Blam -> Saber camera hand-off (the shared lever)

`FUN_0x5F510(user_index, splitScreenFlag)` is the bridge. It reads
`observer_result` through `0x6F0E60(user)` - position `+0x00`, forward `+0x20`,
up `+0x2C`, vertical FOV `+0x4C`, horizontal FOV `+0x38` - and writes a full
4x4 row-major matrix plus a field of view into Saber's per-user camera object:

```text
saberScene = *(void**)(halo2 + 0x1E91210)
cameraList = *(float***)(saberScene + 0x100)
cameraCount = *(int*)(saberScene + 0x108)
camera     = cameraList[user_index]

camera[3] = camera[7] = camera[11] = 0.0f;  camera[15] = 1.0f
camera[12..14] = position * 3.048 + offsets   /* WORLD UNITS -> METRES */
basis rows built from forward/up with two components negated
FUN_0xBC2B0(camera)                    /* commit */
FUN_0xBC560(camera, fovRadians * 57.29578)   /* set FOV in DEGREES */
camera[0x53] = (bool)(fabs(oldFov - camera[0x55]) > 1e-6)
```

The literal `3.048` confirms Halo 2's metre-per-world-unit scale independently,
and shows Saber's renderer works in metres while Blam works in world units.

Its only caller is `FUN_0x51510`, the per-frame camera push: gated by the flag
`0x1E8CF94`, it calls `0x2CEFF0(*(void**)0x1E91260, split)`, then
`0x5F510(0, split)` and `0x5F510(1, split)` for split-screen, and finally
`SetEvent` - **the Saber scene renders on another thread.**

`0x51510`'s caller is `FUN_0x515E0`, the top-level frame driver, which also
calls the Blam frame `0x67A220` (at `0x51C89`) before pushing the camera (at
`0x51CC8`). `0x515E0` is the same function that calls the SSL script function
`switch_render_mode`.

**Consequence: `observer_result` is upstream of BOTH renderers.** The classic
path consumes it through `0x7DF5A0` into the window cameras; the remastered path
consumes it through `0x5F510` into the Saber camera matrix. A headset pose
written into `observer_result` therefore reaches whichever renderer is live.
This is a static-analysis conclusion; it has not yet been observed at runtime.

### The remastered renderer's own surface

| Fact | Value |
| --- | --- |
| Saber renderer code region | approx. RVA `0x1CB000` - `0x2E2000` |
| render-target name array | RVA `0xBE9010`, 98 `const char*` entries, indexed by RT id |
| render-target getter | `0xF87E0(?, rtIndex, ?, ?)`, ~70 call sites, referenced by `0xF87F1`/`0xF8802` |
| scene render entry | `0x2DC3D0` -> `0x2DEC00` -> `0x2DF190` (`0x2995` bytes, the pass list) |
| `0x2DC3D0` dispatch | indirect only; its pointer sits in a handler table at `.rdata:0xBFEDC0` |

Selected render-target ids: `[31] Color FP16`, `[32] GBuf pixel normals`,
`[34] Frame buffer 0`, `[35] Frame buffer 1`, `[70] Depth buffer`,
`[95] Frame after gamma correction`, `[96] FSR destination texture`. Entries
`[95]`/`[96]` are the candidate per-eye capture sources; this is **not** yet
proven and must be confirmed against the live bind order.

### The Saber camera object layout

Byte-proven from `0x5F510` (which fills it) and `0xBC560` (the FOV setter,
whose only non-data caller is `0x5F510`):

| Saber camera offset | Meaning |
| ---: | --- |
| `+0x00` .. `+0x3F` | 4x4 row-major matrix, 16 floats |
| `+0x0C`, `+0x1C`, `+0x2C` | homogeneous column: `0, 0, 0` |
| `+0x30`, `+0x34`, `+0x38` | translation, **in metres** (`worldUnits * 3.048`) |
| `+0x3C` | `1.0f` |
| `+0x14C` | bool: field of view changed this frame |
| `+0x150` | vertical field of view, **degrees** |
| `+0x154` | horizontal field of view, **degrees** |
| `+0x158` | aspect ratio |

`FUN_0xBC560(camera, horizontalFovDegrees)` writes `+0x154`, derives
`+0x150 = 2 * atan(tan(hfov/2) / aspect) * 180/pi`, and calls `0xBC380` to
rebuild the projection. Because the vertical FOV is *derived* from the
horizontal FOV and the aspect ratio, this entry point can only express a
**symmetric** frustum; native asymmetric per-eye projection would require
`0xBC380`'s internals and is not established.

`FUN_0xBC2B0(camera)` commits the matrix and has 20+ callers across the
remastered renderer, so it is the general "camera updated" path rather than
anything specific to the Blam bridge.

Reaching the object at runtime:

```text
saberScene  = *(void**)(halo2 + 0x1E91210)
cameraList  = *(void***)(saberScene + 0x100)
cameraCount =  *(int*) (saberScene + 0x108)
camera      = cameraList[user_index]        /* user_index < cameraCount */
```

### The remastered renderer is view-driven, and it carries temporal history

`0x2DEC00` does not render one scene. It walks a **view list** - count at
`*(void**)(halo2+0x1A250F8) + 0x148`, entries from `+0x150` - and calls the
scene render `0x2DF190(view)` once per view whose flag bits pass (bit 6, bit 3,
and a `0x4C` mask test at three separate sites). The engine therefore already
renders the scene several times per frame, which is direct evidence that
`0x2DF190` is re-entrant and that a per-eye second pass is architecturally
natural rather than a replay hack.

**Temporal warning, recorded before any candidate is written.** The
render-target table at `0xBE9010` contains `[66] IBR half prev. frame`,
`[67] IBR SSR` and `[68] IBR SSR alpha`. A previous-frame history buffer plus
screen-space reflections is exactly the construct that cost this project the
Reach effects/bloom eye-desync: a temporal pass blends the OTHER eye's history
and produces per-eye disagreement that looks like ghosting or shimmer rather
than like a camera bug. Any Halo 2 stereo candidate must decide up front
whether to give each eye its own history or to disable the IBR/SSR passes while
stereo is active. This must be settled before the first headset run, not
diagnosed afterwards.

`groundhog.dll` is not involved (E-H2-3). H2EK contains none of this renderer,
so the "discover semantics from H2EK" rule cannot apply to it; H2EK remains
authoritative for the classic engine and for the shared upstream
camera/observer system, which is where E-H2-4's lever lives.

## E-H2-5: the classic eye capture was fake stereo (identical eyes), and why

### The measurement that proves it

The 2026-08-21 02:55 headset run (Steam, Quest 3, source `ec9b268`) was the
first with steady classic-mode pairs: `Halo 2 stereo core: outer=2153
inner=2153 claimed=2036 eyes=4072 complete=2036 dropped=0`. The same log
carries the GPU readback comparison of the two eye caches, five times:

```
M2 VALIDATION: distinct eye pixels mean RGB delta=0.000, changed samples=0.0% (0/23842)
```

Zero of 23,842 sampled pixels differed between the eyes. The call counters
proved our detours ran; the readback proves the engine produced ONE image.
That is fake stereo by definition, and the counters were never proof of
anything else. The player's report ("fake stereo", "a flat picture on my
face", "awful low FOV") is exactly what identical eyes presented per-eye
look like.

### The mechanism, from the engine's own render order

1. `render_view 0x7E30D0` does not just draw the world. At `0x7E34C9` it
   calls `0x952F50`, which at `0x952FE9` calls `0x7E2460`, which at
   `0x7E2BE6` calls the postprocess `0x951EC0`; that ends in the final-output
   helper `0x975230` (sole caller `0x9523B0`, a tail jump) which binds the
   backbuffer RTV from slot `0x197EE58`. So resolve, postprocess and final
   output are all INSIDE each render_view call. Capturing during render_view
   is the right time.
2. The D-H2-1 census taken inside the host render-target window
   (`c63facd` run, classic mode) shows the classic renderer has no separate
   scene target:

   ```
   backbuffer RTV slot 0x197EE58:     view 0000026B8AAF45E0 tex 0000026AE7060760 2912x2100 fmt 28
   primary scene RTV slot 0x197EE60:  view 0000026B8AAF45E0 tex 0000026AE7060760 2912x2100 fmt 28
   resolved scene RTV slot 0x197EE88: view 0000026C302116A0 tex 0000026C2CE77BA0 2912x2100 fmt 28
   ```

   The primary scene target IS the backbuffer view. The world is drawn
   straight into it; the "resolved scene" is a second texture the engine
   fills from it for shader reads.
3. The capture redirected every slot-0 bind of that exact RTV to our eye
   cache. The world pass therefore landed in our texture, but everything the
   engine subsequently READS (the resolve into `0x197EE88`, bloom, the final
   composite) goes through the engine's own SRVs of the ORIGINAL backbuffer
   texture, which we had just prevented from being written. The final
   composite then paints that stale source over our eye cache. Both eyes
   receive a composite of the same untouched source: identical pixels, and
   a source whose field of view is whatever the last un-redirected frame
   had, not the cover we wrote - hence the zoomed, narrow image.

This is the Reach "two eyes share one buffer" family of bug again: a
redirect is only correct when the engine never reads the redirected target
back, and Halo 2's classic postprocess reads it back by design.

### The fix (C-H2-11)

Halo 2 redirects nothing. `VR_RedirectRenderTargets` returns false for the
title. Each eye runs the engine's complete pipeline into the engine's own
backbuffer; when render_view returns, `VR_EndRasterEye` resolves the texture
behind the proven final-output RTV (`GetResource`/`QueryInterface`, no
reference held, so the swapchain can still resize) and `CopyResource`s it
into that eye's cache. An eye is complete only when that copy was issued
(`kHalo2SynchronousRequiredCaptures = 1`). Every other part of the
transaction - the per-eye camera writes and restores, the exact-serial pair
contract, the stock-screen fail-open - is unchanged.

### The proof that must appear in every future log

`ValidateHalo2EyePairPeriodic` reads both eye caches back every two seconds
while an exact-serial pair is being presented and logs one of:

```
Halo 2 eye-pair pixel check: DISTINCT eyes, mean RGB delta=..., changed samples=...% (n/N) ... - true per-eye rendering
Halo 2 eye-pair pixel check: IDENTICAL eyes (0/N samples differ, ...) - this is FAKE stereo: ...
```

Three consecutive IDENTICAL results add "the per-eye camera is NOT reaching
the renderer". A Halo 2 headset result is not "stereo" unless the log says
DISTINCT; counters do not count.

### render_view does honour the written camera (agent trace, 2026-08-21)

Independently of the capture defect, a whole-module trace of `render_view
0x7E30D0` established that the per-eye camera write is consumed: at
`0x7E3205-0x7E3263` the rdx camera record is copied byte-for-byte (0x74) into
`g_render_camera` (RVA `0x165C260`); `0x7DF7A0` builds `g_projection`
(`0x165C2D4`) from it reading FOV `+0x28` (`0x7DF88C`), forward/up
`+0x0C..+0x20`, position `+0x00`, znear/zfar `+0x40/+0x44`; the scene pass
`0x7E34A0 call 0x7DECC0` receives `&g_render_camera, &g_projection` and
`0x7F0F50` derives the view record from them. The 707-function closure of the
scene pass contains no reference to the observer arrays `0x15F297C` /
`0x15F28B8` nor to the camera builder `0x7DF5A0`, and no RIP-relative write
into `0x165C260..0x165C2D3` is reachable before the scene call. So once the
capture copies what each pass actually drew, the two eyes differ.

Two caveats the same trace produced:

- The asymmetric-frustum fields `+0x58..+0x70` are consumed in
  `render_player_window` (`0x7E2315 call 0x7DFCD0`) BEFORE `render_view`, and
  passed as its 8th argument. A write to those fields at render_view entry is
  silently ignored. Position/forward/up/FOV/viewport/znear/zfar are honoured.
- `0x7E236F`: `comiss [window+0x34]` (render-camera FOV) against `.rdata`
  `0xC32580` = `1e-4f`; `jbe 0x7E2417` skips render_view entirely. Any FOV
  the mod writes must exceed 1e-4 rad, which every cover does.

## E-H2-6: the head pose was applied twice in Classic mode

The classic window cameras are not an independent camera. `0x960230` (the
per-frame setup that ends in `0x96040E call render_frame 0x7E1600`, its only
caller) runs `0x6F0E60 -> 0x960780 -> 0x7DF5A0`, which builds the window's
render camera (`window+0x0C`) and raster camera (`window+0x80`) FROM the
observer RESULT record at `0x15F297C + user*0x368`. The observer core
(C-H2-8, hook on `0x6F0250`) has already written the head-tracked
position/forward/up into that record by then. The classic stereo core then
read those cameras as "stock" and applied the head delta again through
`Halo2BuildTrackedCenterCamera`. The 02:55 log shows 87.0 observer poses
applied per second against 87.6 `render_player_window` callbacks per second,
1:1. The Anniversary path read the same record and doubled identically.

Because the two cores read separate publications of the OpenXR snapshot, the
composite is `delta(N-1) + delta(N)` whenever a publish lands between them,
not a clean 2x, so halving either contribution was never a valid fix.

Fix (C-H2-11): while the observer core is armed, both per-eye cores take their
camera as the already-tracked centre and add only the per-eye offset; the
classic telemetry line names `poseOwner=observer|classicCore`. `Game_Recenter`
and the head-tracking toggle recenter all three references together; the
toggle's missing braces had left the observer recentering unconditionally and
the Anniversary core never.

## Anniversary: why 791 callbacks produced 0 pairs (2026-08-21)

All 790 stock passes took the first gate: `VR_Halo2GetSynchronousRenderSnapshot`
returned false because `Game_AutoVrTick` computed `halo2StereoUsable` from
`Halo2Stereo_Armed()` alone, and the classic core is (correctly) never armed
while the remastered renderer is live, so stereo was forced OFF within ~100 ms
of every switch (`M2 alternate-eye stereo OFF`). Behind it, two further
defects guaranteed the single drop: the eye loop asked
`VR_Halo2GetSynchronousHalfFovs` while the pair token held the RESERVED
serial (never true), and `VR_Halo2BeginSynchronousEye` opened a raster-eye
scope that nothing ever closed, so no capture could be published.

Refuted suspects: `0x197EE58` IS populated in remastered mode (the per-frame
draw entry writes both `0x197EE58` and `0x197EE60` from the same vtable call
before the classic/remastered branch); `ResolveViewRecord`, the count offset,
the view-index range and the 0xC8-byte context memset match the engine
byte-for-byte.

Every bail in `EyeLoopBody` now counts a named reason and the two-second
report prints the nonzero ones, so the next log names the gate if one remains.
The Anniversary eye reports the engine's own stock field of view (degrees at
camera `+0x150/+0x154`) as its cover; widening that cover to the headset's
native frustum is the next step once DISTINCT pairs are logged in this mode.

## C-H2-12: tracking brought to Halo 3 / Reach / Halo 4 parity

A side-by-side audit of the three accepted camera cores against Halo 2's
(2026-08-21) found four divergences, all in the shared pure logic
`Halo2BuildTrackedCenterCamera` and its callers, none in engine facts:

1. The recenter reference was the FULL head quaternion; the accepted titles
   recenter against YAW ONLY. A recenter taken while pitched or rolled tilted
   Halo 2's world permanently. Now `Halo2YawOnlyQuaternion` extracts the yaw
   about room +Y and the head's pitch/roll are absolute, composed onto the
   game camera in its local axes (`Halo2ApplyLocalQuaternion`), yaw adding
   to the game's own.
2. Room-scale lean was mapped through the live camera's 3-D basis, so engine
   pitch tilted the lean plane every frame. Now the displacement is taken in
   the recentered horizontal frame and re-applied in the GAME's horizontal
   frame with room up on world +Z, exactly Halo 3's `fwdComp/rightComp/dy`
   model (game.cpp ~5938-5975); a camera looking straight along world Z
   falls back to its own basis.
3. A head more than 4 m from the recenter point, or more than 4 m from the
   tracking origin (`ValidHeadPose` in the observer and Anniversary cores),
   REJECTED the sample, which cost the frame its stereo pair. The accepted
   titles clamp. The per-axis delta is now clamped to +/-4 m, the absolute
   sanity bound is 64 m, and the existing +/-1.5 world-unit write bound is
   the real limit, as in the other three titles.
4. Halo 2 read none of the universal knobs. The F6 positional toggle and the
   lean world scale are now honoured (`Game_IsPositionalTracking`,
   `Game_GetWorldScale`). Pitch trim / pitch and yaw sign remain unused
   because Halo 2's observer composes the head quaternion whole.

Tests: `C-H2-12` cases in `tests/core_tests.cpp` pin room forward/right/up
onto a +Z-up Halo camera at 1/3.048 world units per metre times world scale,
the F6 toggle, re-application along a yawed game camera, the out-of-range
clamp, an absolute pitch surviving a pitched recenter, and an exact yaw
cancel.

## E-H2-7: the Saber camera's field of view, and how the eye pass sets it

Agent trace of halo2.dll, 2026-08-21 (all RVAs; Saber camera object 0x398
bytes, embedded copy at view record +0x20):

- **+0x150 is the HORIZONTAL and +0x154 the VERTICAL field of view, in
  degrees.** The mod's constants had the names swapped. Proof: the
  projection builder `0x1C82C0` reads `+0x150` at `0x1C82CF` into
  `P[0][0] = 1/tan(fovx/2)` (`0x1C8386`) and `+0x154` at `0x1C8328` into
  `P[1][1] = 1/tan(fovy/2)` (`0x1C8390`); the constructor's aspect at
  `+0x158` is `0.75` for 640x480 (`0xBC162`), i.e. HEIGHT/WIDTH; and
  `0xBC4F0(cam, 80.0)` leaves `+0x150 = 80` and `+0x154 = 2*atan(tan 40 *
  0.75) = 64.0`, the classic 4:3 pair.
- `0xBC560(cam, V)`: `+0x154 = V`, `+0x150 = 2*atan(tan(V/2) / aspect)`
  (`0xBC571-0xBC5B5`), then `jmp 0xBC380`. `0xBC380(cam)` refreshes the
  near-plane rectangle/polygon and px-per-metre fields (`+0x88..+0xA8`,
  `+0x12C..+0x138`, `+0x15C/+0x160`) from `+0x150/+0x154/+0x80/+0x144/
  +0x148`; it reads neither `+0x158` nor the matrix. `0xBC2B0` normalises
  the three basis rows, copies `+0x00..0x3F` to `+0x40..0x7F` and inverts
  the copy in place (`0xBC5D0`); it touches nothing at `+0x80` or above.
- **`0x1C6D80` takes four arguments.** `0x1C7740` clears r9 as well
  (`0x1C790D xor r9d,r9d`); `0x1C6D80` stores r9 (`0x1C6DD7`) and, when
  non-null, dereferences it as a float[4] clip plane and rewrites the
  projection column. The mod's three-argument typedef left r9 as garbage
  on every call. Fixed: four arguments, all pointers null.
- `0x1C6D80(record,0,0,0)` copies `cam+0x40` to `record+0x46C` (view),
  bakes `record+0x4AC` (projection, reversed-Z, row-vector) from the
  degree fields through `0x1C82C0`, and derives `+0x3EC = view*P` and
  `+0x56C = rotView*P`. The scene render `0x2DF190` latches `record+0x20`
  and the record itself into its context (`0x2DF2E8-0x2DF2F3`) and never
  reads the per-user camera object; several per-view consumers re-derive
  tangents from the record's degree fields (`0x1D6530`, `0x1CB0F0`,
  `0x1D8FE0`, `0x247150/0x249120`, the shadow builders `0x21BC20`,
  `0x21A2C2`), so the degree fields are load-bearing and writing matrices
  alone would be insufficient.
- No clamp exists on this path: `0xBC560`, `0xBC380`, `0x1C82C0`, `0x5F5ED`,
  `0x7DFF10` contain no `maxss/minss`; the only `comiss` are two 1e-6 tests.
- `+0x14C` (fovChanged) is written only by the bridge (`0x5F80E`); a
  whole-module displacement scan found no camera-relative reader.
- X off-centre exists natively (`+0x12C/+0x134` -> `P[2][0]`, `0x1C83B0-
  0x1C8445`); Y off-centre does not. Left for a later candidate.

### Per-eye recipe (C-H2-12, `ApplyEyeCamera`)

1. Write the 16-float camera->world at `cam+0x00` (as before).
2. Write `cam+0x150` = horizontal and `cam+0x154` = vertical cover in
   degrees. The cover is solved PER AXIS from both eyes' native frusta
   (`Halo2DeriveSaberEyeCover`), not aspect-locked, because `0x1C82C0` takes
   the two fields independently.
3. `0xBC380(cam)`, `0xBC2B0(cam)`, `0x1C6D80(record, 0, 0, 0)`.
4. Run `0x2DF190` for the eye; restore the 0x398-byte camera copy and
   re-run `0x1C6D80(record,0,0,0)`.

The three helpers are pinned to their proven entry bytes at install
(`kHalo2Saber*EntryBytes`); a module that differs keeps stock rendering.
One line per generation logs the engine's stock degrees against the
written cover.

## Accepted C-H2-1 runtime contract

- Existing registry row/slot only; no new title descriptor or alias. These are
  the accepted C-H2-1 behavior settings; C-H2-2 through C-H2-6 change only
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
C-H2-5 false-RTV-fatal candidate, or C-H2-6's zero-hook correction. C-H2-5 and
C-H2-6 were both headset-rejected and compile-disabled; a successor must bind
the live player render route before its own headset result and Halo 3 regression.

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

## E-H2-12: the projection each eye was really rasterised with (C-H2-17)

Offline decompilation of the pinned `halo2.dll`, 2026-08-21 (all RVAs). The
C-H2-12 baseline (`5d713cf`) was headset-reported as real stereo with a
cropped, magnified image in BOTH graphics modes ("the world feels too big,
the gun is huge, I can't see enough of the world"). Its log crops each eye
to the headset's native frustum out of the cover the mod WROTE (classic
126.6 x 110.2 deg, Anniversary 108.1 x 110.1 deg); nothing in that build
reads back what the engine rendered with. A narrower real frustum under
that crop is exactly a magnified, cropped image. C-H2-17 therefore derives
the crop from the projection the engine built for each eye pass, and names
any disagreement with the written cover.

### Classic: `0x7DF7A0` and the raster-context stack

- `0x7DF7A0(camera, asymmetric, out)` builds the projection straight from
  the camera: `tan(camera+0x28 * 0.5)` is the vertical half-tangent, the
  rectangle at `+0x30` supplies `aspect = width/height`, and it stores
  `out+0x78 = 1/(aspect * tanY)` and `out+0x8C = 1/tanY` (plus `+0x68..+0x74`
  frustum planes, `+0xB8/+0xBC` half-pixel extents). **No global FOV scale
  is applied here**; the `0xE13470..78` scales live in the camera builder
  `0x7DF5A0`, upstream of the mod's write.
- Callers of `0x7DF7A0` (Ghidra): `0x7E3269` and `0x7E32F8` in render_view
  `0x7E30D0` (render camera -> `g_projection 0x165C2D4`, then raster camera
  `r8` copied to a local block -> that block's `+0x100`), `0x7E183D/0x7E18BE`
  in `0x7E1780`, `0x7E1C03`, `0x7E2001/0x7E20A2`, `0x7E0FE1`, `0x960623`,
  `0x7DF2C8`, `0x7E2EA3/0x7E2FC3`, and `0x9560BE` in `0x955F60`.
- `0x955EC0(block)`: `++depth` (int32 at `0xE19208`), copies the 0x318-byte
  block verbatim into `0x1996D30 + depth*0x318`, then `0x955310(slot)`
  applies it. The block layout (from `0x7E1780`): raster camera `+0x18`
  (0x74 bytes), its projection `+0x100`.
- `0x955F60()`: `--depth`, re-applies `slot[depth]`, copies that slot's
  camera back to `g_render_camera 0x165C260` and **rebuilds `g_projection`
  from the restored OUTER camera**. So after render_view returns,
  `g_projection` is the wrong thing to read; `slot[depth+1]` is untouched
  and still holds the eye pass's raster camera and projection.
- Capacity: three slots fit before the window array `0x19976E0`.

`ReadEngineProjection` (classic core) reads `depth`, resolves `slot[depth+1]`,
proves the slot is this eye's by comparing its camera bytes with the raster
basis and cover FOV the mod wrote (copied verbatim by `0x955EC0`), and takes
`+0x100+0x78/+0x8C`.

### Anniversary: `record+0x4AC`

`0x1C6D80(record,0,0,0)` calls `0x1C82C0(record+0x20, near, far,
record + 299*4 = +0x4AC)`, which writes `[0] = 1/tan(+0x150/2)` and
`[5] = 1/tan(+0x154/2)` (degrees, `0x1C82CF`/`0x1C8328`). The Anniversary
core reads `[0]`/`[5]` after the scene render returns and before its own
restore rebuilds the record from the stock camera.

### Shared helper and log lines

`Halo2HalfFovsFromProjectionScales` (`atan(1/|P00|)`, `atan(1/|P11|)`) feeds
`VR_Halo2CompleteSynchronousEye` in both cores; the written cover is used
only when the read-back is unreadable, and that is counted. Each core logs
`Halo 2 <classic|Anniversary> eye projection read-back: written cover H x V
deg, engine rasterised H x V deg - AGREES|MISMATCH ...` whenever the verdict
or the numbers change. A MISMATCH line is the evidence that the engine did
not render the written cover; the crop follows the engine either way, so the
headset image keeps its true scale (with the loud whole-slice fallback in
`vr.cpp` if the real frustum is narrower than the native one).

## E-H2-13: C-H2-17 headset result, the Saber aspect lock, and the Back button (C-H2-18)

C-H2-17 (`54b97bc`, Steam, SteamVR, log preserved at
`out/test-runs/54b97bc-halo2-c17-cropped-20260821-1647`) read back
`AGREES` in both renderers - classic `126.5 x 110.1` written and rasterised,
Anniversary `108.1 x 110.1` written and rasterised, `0` mismatches of 474/480
- so the compositor crop matches the engine's projection and the crop
geometry is NOT the cause of the "cropped" feeling. The user additionally
reported (a) the renderer flipping Classic<->Anniversary on its own (the
log shows ~15 `live renderer CHANGED` lines in two minutes) and (b) the
Anniversary first-person weapon "squished tall".

### (b) Anniversary weapon squashed: the cover must stay aspect-locked

E-H2-7 proved the engine's own setter `0xBC560(cam, V)` always derives
`+0x150 = 2*atan(tan(V/2) / aspect)` from `+0x154` and `+0x158`
(height/width); C-H2-12..17 instead wrote the two axes independently
(`108.1 x 110.1` on a 2912x2100 raster, tangent aspect 0.96 against the
raster's 0.72). The scene projection `0x1C82C0` takes both fields, so the
world was right and the read-back agreed; the weapon pass evidently does
not - it came out narrowed by about the ratio of the two horizontal
tangents (`tan 54 / (tan 55 / 0.721) = 0.69`), i.e. squashed tall. C-H2-18
derives the Saber cover aspect-locked (`Halo2DeriveSaberAspectLockedEyeCover`,
vertical lifted until the derived horizontal contains both eyes: 110.2 x
126.6 deg at 2912x2100, the same pair the classic core uses), so every
consumer, locked or not, sees one frustum. Which Saber function draws the
weapon and how it builds its frustum was not located; the runtime evidence
is the weapon shape next to the scene, and C-H2-18 tests exactly that.

### (a) The renderer flips: the mod's synthetic Back button

The retail `halo2.dll` request global `0xE21278` is written by the Saber
frame driver `0x515E0` (five sites) and the start-up applier `0x6CCA0`;
the request comes from the SSL `switch_render_mode` path, which MCC binds
to the controller's Back/View button in Halo 2. The log reports
`reporting virtual gamepad as connected (no physical pad found)`, so every
Back press the game saw came from the mod's virtual pad, whose only Back
source is the D-pad gesture (controller held within 30 cm of the head:
left-stick click -> Back, added for ODST's map screen). In Halo 2 that
button is the instant renderer switch, so C-H2-18 never synthesises Back
while Halo 2 is the active title (the click stays a stick click) and logs
once that it did so.

## E-H2-14: "Classic dropped out of the hook" = the generation quarantine (C-H2-20)

C-H2-18 log (`out/test-runs/18e4877-halo2-c18-goggles-20260821-1656`),
16:55:12.621: `Halo 2 stereo generation 1 QUARANTINED
(CoreClaimedTransactionFailed): the current claimed frame remains dropped;
later frames use stock screen presentation until a new module generation`,
followed by `stereo core removed (CoreClaimedTransactionFailed)` and
`Runtime mode: gameplay -> loading`. From then until the user quit, every
switch to Classic graphics logged the renderer change and nothing else: the
classic core's poll refused to reinstall for the rest of the level
(`g_rejectedGeneration == generation`), the adapter cleared the Halo 2
heartbeat because no core was ready, and the headset stayed on the stock
flat screen. That is the silent fallback the project forbids.

C-H2-20 removes the generation-wide quarantine in the classic core: a
failed claimed frame is dropped by its own transaction (as before), the
quarantine word is cleared on the next poll, the drop is counted and logged
at most every two seconds (`Halo 2 stereo: claimed frame DROPPED (<reason>,
drop #N this generation); the core stays armed`), and the core is never
removed for it. Install/enable failures keep their own rejection path.

## E-H2-15: why the VR controllers, chords and F1 menu were dead inside Halo 2 levels (C-H2-22)

C-H2-21 log (`out/test-runs/7075832-halo2-c21-vr-controllers-dead-20260821-1729`):
the XInput hook was alive (the L3+R3 chord and the menu pointer logged), but
only while MCC was in its shell. Inside a Halo 2 level `Game_AllowsSharedControllerInput()`
returned false, because the Halo 2 registry row granted
`admissionCapabilities = TitleCapability_None` and `kHalo2Stereo6DofRuntimeCapabilities`
carried neither `ControllerInput` nor `Haptics`. `ProcessGetState` then returned
the physical pad untouched and `MergeVrPad` never ran: no VR sticks or buttons,
no L3+R3 / Y+B chords, no D-pad gesture, and the welcome page that opened at
17:28:11 could not be closed until the level was left (17:28:43, in the shell).
"It works in all the other games" because every other supported title grants
`ControllerInput` (Halo 4: `kHalo4AdmissionCapabilities`).

C-H2-22 grants Halo 2 `ControllerInput` (admission + runtime) and `Haptics`
(runtime, arm-gated), matching the Halo 4 pattern. Aim, HUD, arm IK and
cutscene theatre remain denied until each has Halo 2 evidence.

### The pictures (C-H2-19 eye dumps, same run)

`HaloMCCVR-halo2-eye0/1.bmp` are correct per-eye renders of the 126.5 x 110.1
deg cover: distinct eyes, full frame lit, world at the expected scale. The
first-person weapon fills roughly the right half of each frame; the headset's
native crop (the middle ~56% x 84%) therefore shows mostly the weapon and the
ground in front of it. That is the "looking through goggles / 6-foot gun /
can't see the world" report, and it is the weapon's framing, not the
projection. MCC keeps the first-person weapon at a constant screen fraction
across its FOV slider, so at the wide headset cover the weapon is scaled up
with it. The weapon's projection/placement path is the next target; the
classic camera `+0x2C` "FOV ratio" is NOT it (its three readers 0x708D90 /
0x7E4D30 / 0x76DC90 scale an object distance for LOD), and 0x7DF7A0's other
callers (0x7E2D50, 0x7E1F80, 0x7DF1A0) build ordinary render/raster pairs.

## E-H2-16: headset-owned pitch, walk-where-you-look (C-H2-23)

Until C-H2-22 the Halo 2 observer composed the head orientation (relative to
a yaw-only recenter reference) onto the engine's FULL stock camera, so the
engine's own look pitch (right stick / aim) added to the head's pitch: the
horizon moved with the stick and the view pitched twice. C-H2-23 mirrors the
accepted Halo 4 construction (E-H4-9):

- `Halo2BuildTrackedCenterCamera` flattens the stock camera to its yaw
  (horizontal forward, world +Z up) before composing the head, so pitch and
  roll are the headset's alone and yaw still adds (the stick turns the body).
- `Game_ComputeHalo2PitchStick` drives the right stick's vertical axis with
  the shared pitch servo (`Halo4PitchServoStep`, direction and resolution
  measured) from the engine pitch in the observer's published `stock` camera
  to the head's pitch, so the engine's aim - the shot line - follows the
  view. It steps once per observer publication and idles loudly.
- `Game_MapMoveStick` rotates the walk vector by the yaw between the
  published `tracked` and `stock` cameras, so forward goes where you look.
- The input hook passes the horizontal turn stick through and feeds the
  servo on the vertical axis (`Game_Halo2OwnsLookPitch`).

## E-H2-17: the renderer flips were the gesture's Back button (C-H2-25)

The C-H2-24 Steam log (20:01-20:03, source cb9f1f3) shows six
`Halo 2 live renderer CHANGED` lines in two minutes, and every one is
preceded within 0-47 ms by the virtual pad feeding `0x0020`
(XINPUT_GAMEPAD_BACK) - the D-pad gesture's stick click. The user's screen
recording of the same session shows the image alternating Classic /
Anniversary at those times. The single C-H2-23 flip with no pad button fed
(mask 0x0000) was the keyboard Tab path and stays a Steam Input setting.

In Halo 2 campaign Back has exactly one meaning - MCC's Classic <->
Anniversary switch - so "the stick click is Back like ODST" IS a renderer
flip there. C-H2-25 applies one rule to every source: Back reaches Halo 2
only with `halo2_gamepad_graphics_switch = 1`; the gesture click is
swallowed with a logged `M3: Halo 2 - the D-pad gesture's stick click (Back)
was swallowed` line otherwise. Other titles keep click = Back.

The recording also shows the first-person weapon filling roughly half of the
frame in BOTH renderers (E-H2-15) - that remains the open framing item; the
large black shape in the Classic frames near 0:06 is a crashed Warthog on its
side in front of the player, ordinary scene geometry.

## E-H2-18: the Anniversary weapon, the Anniversary HUD, and one head pose (C-H2-26)

Three facts from the C-H2-24 session (screen recording + log + the saved
eye pictures of C-H2-21), each with its offline proof in halo2.dll:

### (a) The weapon is drawn through a SECOND projection

`0x1C6D80` (the view-record rebuild this core already calls after writing
the eye camera) builds two projections. The world one goes to record+0x4AC
from the camera's +0x150/+0x154 degrees (E-H2-12). It then copies the camera
to the stack, overwrites the copy's vertical FOV with the literal
`0x424660D5` = **49.594 degrees** (`tanf(0.43279424)` - the tangent of its
half-angle - is precomputed), derives the horizontal through the record's own
aspect (`atanf(tan / aspect)`), rebuilds the copy with `0xBC380`, and stores
the result at **record+0x4EC** through the same `0x1C82C0`. Disassembly at
`0x1C7593`: `LEA R8,[RDI+0x56C]; MOV RCX,R12; CALL 0x9FCE0` stores
view-without-translation (+0x52C, translation row zeroed) x world projection
at +0x56C (inverse at +0x5AC), and the earlier product stores
view-without-translation x first-person projection at **+0x5EC**.

That is MCC's "the weapon looks the same at every FOV" rule. At the
headset's 126.5 x 110.1 degree cover the weapon is drawn
tan(55.05)/tan(24.797) = **3.1x larger** than the world around it (the
"6-foot gun", "goggles", "squished"), and every per-eye offset moves it 3.1x
further across the frame than the world moves (the saved eye pictures: the
world shifts a few pixels between eyes, the weapon shifts ~120 of 728).

C-H2-26 detours `0x1C6D80` (MinHook, entry bytes already pinned): after the
original returns, and only for the record the eye loop is preparing
(`g_fpPatchRecord`) and only when no clip plane was passed (the water-mirror
rebuild passes one), +0x4EC := +0x4AC and +0x5EC := +0x56C. The stock camera
restore at the end of the pair clears the record first, so the engine's own
rebuild puts the 49.6-degree projection back. A read-back after each scene
render compares the two projections' diagonals and logs
`first-person weapon projection ... AGREES` or `OVERWRITTEN`.

### (b) The HUD is the Blam interface draw, run once, after the views

`0x2DEC00` renders every view, then `0x2E3F70 -> 0x2819A0` invokes the
function pointer the host stored at `0x1A6E538` during start-up (`0x69730`,
`LEA RAX,[0x696A0]; MOV [0x1A6E538],RAX`). `0x696A0 -> 0x69540 ->
0x960230(1) -> 0x7E1990 -> 0x831CB0`: `0x831CB0` has exactly two callers,
`0x7E1990` and the classic `render_view 0x7E30D0` - it is the interface/HUD
draw. In Anniversary it runs ONCE per frame on the Saber render thread over
whatever the backbuffer holds, i.e. after this core's two eye copies, which
is why the desktop shows a HUD and the headset never did (the C-H2-21 eye
pictures have none).

C-H2-26 detours `0x696A0` (entry bytes `40 53 48 83 EC 20 83 3D`). With a
complete pair for the last completed serial: the original draws the HUD over
eye 1 (the last eye rendered, still in the backbuffer) and eye 1 is
recaptured; eye 0's finished scene is copied back into the backbuffer, the
original runs again, eye 0 is recaptured. Both eyes carry the HUD the way
the classic render_view draws it per eye. Without a complete pair the
callback runs once, untouched. Logged as `Halo 2 Anniversary HUD: ...
replayed per eye on N frames`.

### (c) One head pose for the weapon and the world

The observer writes the tracked camera once per game tick; the Saber scene
renders per headset frame on its own thread, so ~1/3 of its frames saw a
different prepared serial and REBUILT the centre from a newer head sample
(`rederived=179` of 223 in the first 2 s of the C-H2-24 log). The engine had
already placed the weapon against the observer's pose, so on those frames
the weapon and the world were drawn at two different head poses - times
3.1x from (a). C-H2-26 publishes the observer's full sample (head pose, eye
offsets, absolute view poses) with its camera; the Anniversary core builds
its eyes from THAT sample whenever the observer's tracked camera is in hand,
and the pair is submitted with those view poses (`VR_Halo2GetSynchronousPairPoses`),
so the compositor reprojects from the pose the image was really drawn at.
Self-tracked frames keep the prepared serial's own sample and located poses.

## E-H2-19: the self-flipping renderer, and a switch that needs a hand behind it (C-H2-27)

The C-H2-26 Steam log (05:14:30-05:14:48) shows TWELVE `live renderer
CHANGED` lines in eighteen seconds, alternating Classic/Anniversary about
once a second, every one with `virtual pad buttons fed ... none` - the mod
fed nothing, and the physical pad's Back is swallowed. That is the "flashing
back to the flat screen" in the headset: each flip tears one core down and
installs the other.

Offline: the request dword 0xE21278 is written only by the frame driver
0x515E0 and the start-up writer 0x6CCA0; it is applied by 0x511E0, which
writes the mode dword 0xE21280 and the gate byte 0xE70CF8 and tells Saber
`isLegacy`. 0x515E0 toggles the request when its switch-input flag is set
(pad Back / keyboard Tab / SSL `switch_render_mode`), and separately, when
the "forced legacy fading" flag 0x15A3D91 (0x6AFA00) is set, drives the
request from the fade target byte *(0x15A3D88)+0x12A (0x6AFA10) every
frame and clears the flag (0x6B03B0(1)). The flag is set by 0x6B03C0,
0x6AF7E0 and 0x6AF600.

C-H2-27 detours the applier 0x511E0 (entry bytes pinned). A request that
differs from the applied mode is honoured only while a switch input is
present at that instant: keyboard Tab held (GetAsyncKeyState), the physical
pad's Back held with `halo2_gamepad_graphics_switch = 1` (the raw pad mask is
now recorded before the swallow), or the mod's own Back fed within 250 ms.
Otherwise the request is written back to the applied mode and the event is
logged with the evidence read at that instant (both inputs, the fading flag,
the fade target byte). The CHANGED line now carries the guard's verdict.

The head-gesture stick click is back in Halo 2 as a deliberate switch: hold
the click at the head for 350 ms and Back is pressed once (logged); a brief
click does nothing, so it can never flip the renderer by accident.

The same log explains the second flash: after the eye pair, the Saber frame
rendered a second view (`serialRepeated` stock passes climbing 126 -> 253)
INTO THE BACKBUFFER, and the C-H2-26 HUD replay then recaptured "eye 1"
from it - a stock, mono, full-FOV frame in one eye for that frame. The
replay now restores eye 1's finished scene into the backbuffer before its
first HUD draw, always.

## E-H2-20: the classic first-person weapon (C-H2-27)

H2EK names it: `draw_first_person` (profile string, H2EK 0x29D8F0, next to
render_view 0x2A0160). It copies the render camera globals, stores
`0x3F5D9734` - 0.86557 rad = **49.594 degrees**, the very constant Saber
bakes - into the copy's vertical FOV, rebuilds the projection with the
asymmetric-frustum helper and the projection builder, sets it as the
rasterizer camera and draws the first-person models, then restores the
world camera. Retail: 0x7E0C60 does exactly that with the globals at
0x1996A28 (camera) / 0x1996B10 (projection), helpers 0x7DFCD0 / 0x7DF7A0 /
0x955590, and the constant lives in .rdata at 0xB3D99C with exactly one
reader, the MOVSS at 0x7E0F35 (`F3 0F 10 05 5F CA 35 00`). A byte scan of
halo2.dll finds the float only there and in an unrelated .data global read
by the interface model widget (0x81BFB0).

C-H2-27: the classic core pins the constant (entry bytes of 0x7E0C60, the
MOVSS bytes, the stock value), makes its page writable, writes the eye's
vertical cover into it when it writes the cover FOVs for a pair, and
restores 0x3F5D9734 whenever it restores the cameras and on removal. The
weapon is therefore drawn through the same frustum as the world, per eye,
in Classic exactly as in Anniversary.

## E-H2-21: the frame's own sample, main views only, Y+B and the click (C-H2-28)

The C-H2-27 Steam log (13:09-13:11): zero renderer flips (the guard stood),
HUD replayed on every pair, both eye pictures correct and static - yet the
weapon still "moves around" in Anniversary while Classic is rigid. Classic
is rigid because `draw_first_person` places the weapon against the camera
at DRAW time (E-H2-20); Anniversary's weapon is an object the game frame
placed against the observer pose that frame pushed (0x51510 -> 0x5F510), and
the Saber thread renders after the main thread has moved on: the log counts
45% of pairs rendered "from an older observer serial", i.e. the newest
publication was NOT the one the frame was built from, and the observer
republishes thousands of times per second.

C-H2-28 stops guessing which sample the frame used and READS it: the view
record's camera matrix at record+0x20, saved before anything is written, is
the matrix 0x5F510 built from the observer pose of this frame. The observer
keeps a ring of its last eight distinct publications; the Anniversary core
rebuilds each candidate's matrix with the same `Halo2BuildSaberViewMatrix`
it uses to write eyes and takes the one that reproduces the frame's matrix
(rotation rows within 2e-3, translation within 2 cm). The eyes are built
from THAT sample and submitted as its view poses. The report line counts
matches against the latest sample, an older sample, and no sample.

The same decompile of the Saber frame (0x2DEC00) renders water-mirror
records first (`(flags >> 3) & 1`) and main records after (`(flags & 0x4C)
== 0`); the detour now claims the pair only for a main view (new bail
reason `notMainView`), so a mirror view can no longer claim the serial and
leave the main view rendering stock - the "occasional flash".

Y+B: `TitleSpecificPauseToggleOwner` named Reach, Halo 4 and ODST only;
Halo 2 is added. The head-gesture stick click is one Back press per click
again (ODST behaviour), which in Halo 2 is the renderer switch - deliberate,
honoured by the guard because the mod fed it.

## E-H2-22: the Anniversary weapon is one frame behind the camera (C-H2-29)

H2EK carries MCC's `main\halo_frame_interpolator.cpp` with four
first-person-weapon slots per local player (stride 0x33D4; retail reset
0x723010, read side 0x7228B2, buffers 0x164B2E8/0x164B2F0) - the weapon's
animation is blended between ticks. Its camera-dependent placement, though,
is per frame: the H2EK frame function 0x24950 calls the first-person update
chain (0x6BD30 at 0x24FF3 -> 0x6B680 -> 0x30A656 -> 0x309553 ->
first_person_weapons.cpp 0x307536) BEFORE observer_update_all (0x448B0 at
0x250C1). So in every frame the weapon is placed against the observer pose
of the PREVIOUS frame, while the frame's camera push (0x51510 -> 0x5F510)
carries the CURRENT one. Rendering the eyes from the frame's own sample
(E-H2-21) therefore still leaves the weapon one head-sample behind the
world on every frame - the Anniversary "moving around like crazy". Classic
has none of this: `draw_first_person` (E-H2-20) places the weapon against the
camera at draw time.

C-H2-29: the Anniversary core renders the eyes from the publication BEFORE
the one the frame's camera matched (the sample the first-person update
saw) and submits them as that sample's poses; the world is one frame older
and the compositor reprojects it, the weapon is rigid to the view. The report
line counts how often no older sample was available (start of a level).
The tick-locking and interpolator-reset variants were considered and
dropped: the placement is per frame, not per tick.

## E-H2-23: one head sample per game frame (C-H2-30)

C-H2-29 rendered the Anniversary eyes from the publication BEFORE the one the
frame's camera matched, on the H2EK evidence that the first-person update runs
before `observer_update_all`. The 93cdc1a headset run kept the swim, and the
retail chain says why choosing a publication can never fix it.

Retail proof, independent of the kit. The frame interpolator's first-person
slot reset `0x723010` (E-H2-22) has exactly ONE caller: `0x818CA0`. That
function IS `first_person_weapons`. Its callers are `0x81AE30` and `0x81BF20`,
and `0x81BF20` is called from `0x3CAB0`, which the main frame function
`0x67A220` calls at `+0x3FD`. `observer_update_all` is `0x6F1A60` - the only
caller of the observer transform `0x6F0250` - and `0x67A220` calls it at
`+0x428`, i.e. AFTER the weapon placement, in the same frame. The Saber camera
push (`0x51510 -> 0x5F510`) then reads the observer record as it stands after
that update. The observer republishes thousands of times per second, so a
publication chosen at render time is a lottery: the weapon and the camera are
built from two different head samples no matter which one is picked.

C-H2-30 latches instead of choosing. The observer core takes a second hook on
`first_person_weapons` (pinned by its entry bytes
`48 8B C4 44 88 40 18 89 50 10 89 48 08 55 53 56 57`). On entry it:

- reads one head sample and stores it as the frame's sample;
- re-places the observer record on that sample, but only while the record still
  holds, bit for bit, the tracked pose the mod published at the previous
  observer update (`Halo2CameraBasisMatchesExactly`) - the record already
  carries a mod-written pose, so re-deriving from it unguarded would apply the
  head delta twice. The rewrite derives from the STOCK pose published with it;
- lets `first_person_weapons` run, so the weapon is placed on that sample.

`ApplyHeadPose` then consumes the same latched sample instead of a live one, so
the observer publication, the Saber camera push and the submitted eye poses are
all one sample. The Anniversary core drops the C-H2-29 shift and renders the
eyes from the sample its frame camera matched. Failure is loud: the latch is
WITHHELD with a named reason when the entry bytes do not match, and the
observer report counts weapon placements, samples latched, records re-placed,
records left stock because the engine had moved them, placements with no head
sample, and observer updates that fell back to a live sample.

## E-H2-24: the classic eye image is in neither learned slot (C-H2-30)

93cdc1a, Steam, Classic: `IDENTICAL eyes (0/40527 samples differ)` on every
check, and the two quarter-size dumps the mod writes next to the log were
byte-identical files. In the same seconds the draw census counted a full render
inside EACH eye (118327 and 118704 draws) and the projection read-back AGREED
for both eyes, so the engine received the per-eye cameras and drew two
different frames. Yet `capture probes` reported `pair changed 0.0%` for the
primary scene slot as well: neither `0x197EE58` nor `0x197EE60` changed between
the two passes.

The classic final output explains it. `0x975230` (single caller `0x951EC0`,
the postprocess) begins:

```c
if ((DAT_181996a17 != '\0') && (DAT_18186d300 == '\0')) {
    uVar5 = FUN_1807f0590(); if (uVar5 == 0xffffffff) return;
    FUN_1809513d0(FUN_1809519b0(param_1,0), 0, FUN_1809519b0(uVar5,0), 0);
    return;                      /* resolved elsewhere; no bound output */
}
```

With that render-to-texture flag set the finished frame is resolved into
another target and the function returns without drawing to the bound output;
only the interface (`0x831CB0`) and the CHUD (`0x7FFD70`) reach the backbuffer
afterwards, which is why the backbuffer still shows a world at all and why it
is the same world for both eyes.

C-H2-30 therefore stops choosing between two fixed slots. The eye scope records
every DISTINCT slot-0 target the engine binds inside that eye (up to
`kHalo2EyeBoundRtvSlots`), the capture candidate set becomes the two named
slots plus those targets, and the two probe caches ROTATE across the set
(`Halo2ProbeCandidateForSlot` / `Halo2NextProbeRotation`) until a candidate's
two eyes differ, at which point the existing switch takes it as the capture
source. The probe cost per eye is unchanged - still two copies - and the log
names the candidate and its pointer, so the target is identified by evidence
rather than by another guess.

## E-H2-23 addendum: C-H2-30 REJECTED, C-H2-31 witnesses the tick instead

C-H2-30 (`c011a8b`, Steam, 16:46-16:49) made the camera markedly worse. Its
own report says why: `1890 weapon placements` in ~30 s = 60/s, the game TICK,
against ~200 observer updates per second. `first_person_weapons` is a tick
function, not a frame function; the latch fed every observer update between
two ticks the same stale sample, so the camera updated at 60 Hz with a tick
of added lag. The observer update is now UNTOUCHED again.

The same numbers prove the weapon is placed at tick rate and interpolated
between ticks (E-H2-22), so no choice of observer publication can make it
rigid. C-H2-31 keeps the hook on `first_person_weapons` but as a WITNESS:

- before the placement it reads the observer record, checks bit for bit that
  it still holds the newest publication's tracked pose, and records that
  publication's serial (`Halo2Observer6Dof_WeaponTickSerial`); a record the
  engine moved is counted and not witnessed;
- after the placement it calls the engine's own interpolator reset
  `0x723010(player, slot)` (`*(u32*)(cur + 0x1A06008 + (player*4+slot)*0x33D4)
  = 0`, a no-op while `0x164B2E0` is null) for the owned player's four slots,
  so the renderer draws that tick's placement un-blended;
- the Anniversary core renders the eyes from the ring entry with that serial
  and submits its poses. Weapon and world are then one pose, at most one
  tick old, and the compositor reprojects both together. A witness serial
  that has left the ring falls back to the frame's own sample and is counted.

## E-H2-24 addendum: the `0x975230` claim is RETRACTED; C-H2-31 measures

The C-H2-30 text read the `DAT_181996a17` early-out of `0x975230` as "skips
the bound output". That byte is block+7 of the applied raster context, which
render_view fills from its fourth argument - render_player_window's flag,
which is 0 for the player window and 1 only for the camera-object
render-to-texture path (`0x7F07B0`). The early-out does not apply to the
player window; the per-eye image is NOT explained by it. The probe rotation
also could not sample the extra bound targets (`pair changed -1.0%`): they
are a different shape from the backbuffer.

C-H2-31 therefore adds the two measurements that decide between the only two
remaining explanations, both in the existing 2-second pixel check:

- the captured target is copied once at PAIR BEGIN (one copy per check, not
  per frame) and compared with each eye's copy. `~0% vs both eyes` = the eye
  pass never wrote the captured target (capture-side); `>0% vs both, eyes
  identical` = two passes drew the same picture (camera-side);
- the classic core logs the two render-camera positions it actually wrote for
  the last pair and their separation in millimetres;
- the eye-end census lists every distinct target bound inside the eye with
  its texture, size, format and sample count, once per generation.

## E-H2-25: the classic world pass rebuilds the view matrix WITH translation per render_view (offline sweep, 2026-08-22)

Four independent decompile lenses over the classic pipeline plus two refuters,
all reading halo2.dll; every step below is PROVEN by code unless marked.

- `render_view 0x7E30D0` copies its render camera into `0x165C260` (three
  `movaps` at 0x7E3210/0x7E321C/0x7E3228) BEFORE `0x7DF7A0(&0x165C260, frustum,
  &0x165C2D4)` at 0x7E3269, and copies the raster camera into its 0x318-byte
  block at +0x18, builds block+0x100 with `0x7DF7A0`, pushes it (`0x955EC0 ->
  0x955310`: block -> 0x1996A10, camera 0x1996A28, projection 0x1996B10).
- `0x7DF7A0` stores the camera POSITION verbatim at record+0x5C..+0x64 inside a
  3x4 at record+0x34 and calls `0x729C90(record+0x34, record)`, which writes
  R^T at record+0x04..+0x24 and the translation row record+0x28/+0x2C/+0x30 =
  -(R^T * pos) (loads at 0x729CEC/0x729CFC/0x729D0B, stores at 0x729DE9/
  0x729DEE/0x729DF8). The view matrix carries the translation.
- `0x955590(block+7, camera, projection, idx)` (from 0x955310 at 0x95544A)
  composes view x projection through `0x954ED0` into VS constants c0..c3,
  writes c4..c6 = basis and c7 = {pos.x, pos.y, pos.z, 767.8} into the VS
  shadow `0x197DB40`, copying from the first differing float4 and clearing the
  clean byte `0x197EB48` on ANY difference (value-equality only; no frame or
  tick key). `0x82C070` (from the draw wrappers 0x964F80/0x965160/0x965270/
  0x965430) maps the 0x1000-byte dynamic cbuffer `0x197EB40` with WRITE_DISCARD
  and uploads the shadow whenever the clean byte is 0, then
  VSSetConstantBuffers slot 0.
- No reader of the observer array 0x15F297C, the interpolator buffers
  0x164B2E8/0x164B2F0, or any "previous camera" exists on the world path.
  Only two code writers touch the 0x1996A28 position fields (0x7E10C4 in
  draw_first_person, 0x7F3BB2 in the water reflection), both restore-from-
  saved-copy. `0x955F60` (stack pop, at the END of render_view) restores the
  outer camera between the mod's two calls - not in flight.
- Frame counters 0x165C154 / 0x165C158 / 0x165C15C gate nothing on the camera
  path (readers enumerated).
- `0x7DF5A0` (observer record -> window camera: pos +0x00, fwd +0x20, up +0x2C,
  vfov +0x4C) has seven call sites; the player window's is in setup 0x960230,
  before render_frame. INFERRED: none runs between the push and the draws.

Consequence: two render_view calls whose raster blocks hold positions 3 cm
apart CANNOT yield the same c0..c3/c7, so the identical classic eye copies are
not an engine result. Either the eye pass did not write the texture the mod
copies (C-H2-31 measures this: "captured target before the pair vs eye 0/1"),
or the two calls were given the same position (C-H2-31 logs the written eye
separation and the window camera's distance from the published observer pose).

## E-H2-23/24 addendum: C-H2-32 review fixes (15-agent adversarial review of the C-H2-31 diff)

Confirmed defects, all fixed before the headset saw C-H2-31:

- `Halo2FirstPersonWeaponsDetour` loaded the trampoline BEFORE raising its
  active-callback count, so `RemoveCore`'s drain could free the trampoline
  under a thread already holding it (level exit / generation change). The
  count is now raised first, mirroring the observer detour.
- An unwitnessed tick (record moved by the engine, or no publication) left the
  PREVIOUS witness standing, so the eyes rendered from a pose older than both
  the weapon and the frame camera for ~2.5 ticks. The witness is cleared on
  those paths; the Anniversary core then uses the frame's own sample.
- The witness was keyed on the VR serial, which several ring entries share
  (observer ~200/s vs 90-120 VR frames): the lookup took the NEWEST entry with
  that serial, not the witnessed one. Each publication now carries a unique
  `index` (one per ring write) and the witness/lookup use it.
- The interpolator reset ran in Classic too, stepping the Classic weapon at
  the 60 Hz tick. It is gated on the classic-disabled byte `0xE70CF8`
  (remastered renderer live) and the skips are counted.
- Probe candidates >= 2 are per-eye bind-order indices; the 2 s comparison
  could compare two different targets and learn an unstable source. The
  candidate index and RTV are recorded per [slot][eye] and must match before
  a pair is compared or a switch is taken.
- A learned source index that a later eye could not offer silently fell back
  to the final slot while the report still named the learned one; now logged
  and written back.
- The pre-pair reference copy always came from the final-output slot while the
  eye copies come from the learned source; it now copies the learned source
  (a learned bound target leaves it unsampled and says so), and a failed
  creation is logged once.
- The pixel check's final Unmap followed the FIRST map result after several
  unmap/re-map rounds; it now follows the last.

## E-H2-26: the weapon gets its own view; the eyes keep the frame rate (C-H2-33)

C-H2-31/32 rendered the eyes from the publication the game tick placed the
weapon against. The 8cb89b6 log shows what that costs: `1283 placements` in
~21 s = 60/s (the tick) against ~200 observer updates per second, and `eyes
rendered from the publication the weapon tick was witnessed against 951` of
952 pairs - so the CAMERA ran at the 60 Hz tick. The player runs 72-144 Hz;
that is the "fighting to follow my head".

C-H2-33 separates them. The eyes always render from the frame own matched
sample (frame rate). The weapon keeps its tick pose, and the FIRST-PERSON
view-projection the Saber renderer draws it with (record+0x5EC) is rebuilt
from THAT pose plus the eye offset, so the weapon sits where the engine put
it while the world moves with the head.

No matrix convention is assumed. The engine writes
`viewNoTranslation(frame eye) x world projection` at +0x56C immediately
before the rebuild detour runs; the mod reconstructs exactly that product
from the eye camera it just wrote (`Halo2SaberViewWithoutTranslation` +
`Halo2MultiplyMatrix4x4`) and compares (`Halo2MatricesClose`). Only when the
reconstruction MATCHES is the weapon own product written; otherwise the
engine +0x56C stands exactly as C-H2-26 shipped it. The verdict is logged
once per generation and the applied/kept counts appear in the stereo line.

## E-H2-27: the classic eye image is in a TYPELESS target of a different shape

The 8cb89b6 log settles the classic question that E-H2-25 left open:

- `render cameras written for the last pair - eye 0 (...) eye 1 (...),
  separation 0.0214 wu = 65.3 mm; the engine window camera stood ... 0.0 mm
  from the observer pose the mod published` - the per-eye positions AND the
  lean both reach the engine.
- `captured target before the pair vs eye 0: 93.5% changed, vs eye 1: 93.5%` -
  the captured target IS written during the pair, and identically for both.
- the eye census names the reason: `bound target #0 = ... 3788x2732 fmt 90`,
  two pixels wider than the backbuffer and format 90 = `B8G8R8A8_TYPELESS`,
  while every probe of it reported `-1.0%` (not sampled).

Two things kept that target out of reach, both fixed in C-H2-33:

1. The probe only copied candidates of the backbuffer exact shape and format.
   It now also takes a 256x256 centre box with `CopySubresourceRegion`, which
   works for any shape and any 32-bit format, and the report marks those
   comparisons `(centre box)`.
2. A TYPELESS resource has no default shader resource view, so the blit could
   never sample it. `Halo2ConcreteFormat` names the view format explicitly
   (90 -> 87, 27 -> 28, 92 -> 88, ...) and `AcquireSrcSrv` uses it.

When a differently shaped source wins, the eye caches keep the FINAL slot
shape (so nothing else in the pipeline churns) and the finished eye is
blitted rather than copied; the switch and the blit are both logged.

## E-H2-28: the interpolator reset was the 60 Hz (C-H2-34)

MCC ships halo_frame_interpolator precisely so the 60 Hz game tick does not
show at 72-144 Hz: the first-person weapon is blended between tick N-1 and
tick N every frame. C-H2-31 called the engine own slot reset 0x723010 after
every placement to stop the weapon swimming, which threw that blending away -
so the weapon stepped at exactly the tick rate no matter what frame rate the
player ran. That was the 60 Hz.

It is no longer needed. E-H2-26 fixes the swim by mathematics rather than by
removing motion: the weapon is drawn with the view of the pose it was placed
at, so on screen it is

    inverse(P_tick) x P_tick x L  =  L

exactly its designed offset, for any head motion since the tick, while the
world keeps the frame own view. C-H2-34 therefore never calls the reset. Its
entry bytes are still verified, because being its SINGLE caller is what proves
0x818CA0 is first_person_weapons - the identity check stays, the call goes.

## E-H2-29: the classic second eye drew into a different target (C-H2-35)

This is why Classic never had depth while every other title does.

The classic render-target selector `0x9540F0(target_id, ...)` resolves the
world pass id 0 through a once-per-frame byte at RVA `0x1994935`:

```c
case 0:
  if (DAT_181994935 == 0) target = *0x197EE60;   // the SCENE target
  else                    target = *0x197EE58;   // the BACKBUFFER
```

`render_player_window 0x7E2130` clears that byte at `0x7E2368`, once, before
its single `render_view`. The postprocess `0x951EC0` SETS it at `0x951F97`,
inside `render_view`. The stereo core calls `render_view` twice inside one
`render_player_window`, so when the second eye starts the byte is already 1
and that eye world pass binds the backbuffer while the first eye bound the
scene target. Both eyes render, both with the correct per-eye camera - and
whatever the mod captures, one of them is not in it. That is exactly the
8cb89b6 evidence: correct cameras (65.3 mm apart), the captured target written
during the pair (93.5%), and the two eyes byte-identical.

C-H2-35 saves the byte when the pair scope is built (as
`render_player_window` left it) and restores it before EVERY eye, which is
precisely what the Anniversary core already does with the Saber
once-per-frame latch `kHalo2SaberSceneOnceLatchRva` between its two passes.
The classic telemetry gains `sceneLatchRearmed` and `sceneLatchUnreadable`.

Mechanism note: Classic cannot use the Halo 3 / ODST / Reach / Halo 4
render-target REDIRECT, because the classic postprocess builds its shader
resource view from the engine own scene texture, so a substituted view is
invisible to it. Classic therefore uses the Anniversary mechanism - copy the
finished frame after each eye - and with the latch restored both eyes finish
in the same place.

## E-H2-30: the weapon is BETWEEN ticks, not at one (C-H2-36)

C-H2-34 removed the interpolator reset (that was the 60 Hz) but kept drawing
the weapon with the view of the CURRENT tick. With the interpolator on, the
weapon is not at the current tick at all:

- `0x723580`, called once per frame from the main frame function at
  `0x67A220+0x618`, does `movss [rip+0xF27D6F], xmm0` at `0x723589` - RVA
  **`0x164B300`**, a float.
- the interpolator read side `0x722850` passes exactly that float as the third
  argument of `0x723040(previous, current, factor, out)`, which takes it in
  `xmm2`.

So the drawn weapon is `lerp(previous tick, current tick, factor)`. Drawing it
with the view of the current tick leaves it trailing by up to a whole tick,
and a fast turn is exactly where that shows. C-H2-36 keeps the previous
witnessed tick as well as the current one, reads the engine own factor and
builds the weapon view from `Halo2LerpCameraBasis(previous, current, factor)`.
The report counts how often the view followed the blend and how often it fell
back to the tick pose alone.

## E-H2-31: the classic weapon needs ZERO eye separation (C-H2-36)

With C-H2-35 the classic world finally has true per-eye rendering - and the
weapon then showed double vision, because `draw_first_person 0x7E0C60` copies
the pushed raster camera `0x1996A28`, which now carries a true per-eye
position. A weapon a few centimetres from the eye at that separation lands far
outside what the eyes can fuse.

The Saber renderer never has this problem: its first-person pass uses a view
WITHOUT translation (`record+0x5EC`), so the weapon has no eye separation at
all. C-H2-36 gives the classic pass the same by owning `draw_first_person`:
on entry the pair centre position (the tracked camera before the per-eye
offset) replaces the position at `0x1996A28`, the eye rotation is left alone,
and the mod restores what it overwrote when the call returns. Both eyes then
place the weapon identically, exactly like Anniversary. Counted as
`firstPersonCentred` / `firstPersonUnreadable`.

## E-H2-32: the weapon geometry is re-anchored per frame (C-H2-37)

C-H2-36 failed on both guns and the failure identified the real consumers.

Anniversary lag: writing the first-person view-projection (+0x5EC) was
verified applied (358 writes, reconstruction MATCHES) and changed nothing on
screen, so the weapon is not drawn through that matrix. The fix moves the
GEOMETRY instead: 0x722850(player, id, slot, node**, count*) - the
interpolator read the first-person render 0x8181F0 makes per frame at
0x81858D/0x81872E - hands out the blended node array, stride 0x34:
+0x00 scale, +0x04 the node frame as three world-axis COLUMN vectors
(0x72A9C0: a quat (x,y,z,w) is stored as [1-2(y2+z2), 2(xy+wz), 2(xz-wy)],
the world image of X, then Y, then Z), +0x28 world position. The detour
applies the rigid transform axes' = R*axes, pos' = R*(pos - C_tick) +
C_frame with R = F_frame * F_tick^T, C_tick the witnessed tick camera and
C_frame the newest published frame camera - so the weapon follows a head
turn at the player frame rate and jumps with the world at every tick, in
BOTH renderers. The interpolation reset is restored at the placement (the
weapon must share the world tick-quantised state on stick turns; the 60 Hz
complaint against it was really the camera, fixed in C-H2-33 and untouched).

Classic double vision: draw_first_person read in full shows it re-uploads
c0..c11 from the raster camera copy 0x1996A28 - which C-H2-36 centred with
no effect on the measured weapon disparity (world 3-24 px, weapon >240 px at
full resolution, railing the +-60 px quarter-scale search). The FP model
draws therefore take their view from the per-object path (the render camera
global 0x165C260), so C-H2-37 centres BOTH globals - position, forward and
up - for the duration of draw_first_person and restores them after, giving
the classic weapon the same zero eye separation the Saber first-person pass
has.

## E-H2-33: the classic FOV pin failed behind the mod's own hook (C-H2-38)

Not an engine fact - a mod-install ordering defect, proven by the C-H2-36 and
C-H2-37 headset logs, both of which print, one line apart:

    Halo 2 classic first-person weapon pass owned at draw_first_person +0x7E0C60 ...
    Halo 2 classic first-person FOV constant NOT pinned for generation 1:
      draw_first_person entry / its MOVSS / the stock value do not match the
      proven module; the weapon keeps the engine's 49.6 deg

`PinFirstPersonFovConstant` (E-H2-20) verifies the entry bytes of
`draw_first_person` before it will write the constant. C-H2-36 added the
weapon-pass hook on that same function and created it BEFORE the pin ran, so
the bytes the pin read were MinHook's jump and the pin refused every run. The
classic gun was therefore drawn at the engine's 49.594 degrees inside a
110 degree eye frustum - 3.1x magnified, "FOV awful, shoved in my face" -
from C-H2-36 on, while C-H2-27..35 had it pinned.

Consequences for the C-H2-36/37 conclusions:

- The classic weapon-disparity measurement that "railed the +-60 px search"
  was taken on that 3.1x gun; a search that rails has found no match, not a
  240 px disparity. "Centring 0x1996A28 had no effect" is therefore unproven
  and E-H2-31/E-H2-32's classic reasoning is withdrawn until re-measured with
  the pin working (this candidate; `firstPersonCentred` ran for every eye in
  the C-H2-37 log, 482 of 482).
- The C-H2-37 re-anchor detour on 0x722850 never opened its gate in either
  renderer (`re-anchor applied 0, identity 0, skipped 0`). No counter said
  which term closed it; C-H2-38 classifies every entry (unhandled / other
  player / no nodes / not live / no witnessed tick) and reports the last
  call's arguments. The Anniversary weapon seen in C-H2-37 was therefore the
  C-H2-36 view-matrix weapon with the interpolation reset restored: geometry
  held at the tick while the view blends between ticks, which is the
  "trailing mesh" the user reports and the next target.

C-H2-38 pins first and hooks second. Per user directive (2026-08-22) Halo 2
discovery from here on uses the H2EK tools, not Ghidra on retail.

## E-H2-34: the classic weapon is drawn from the PREVIOUS pass's camera; the re-anchor's gate was the wrong term (C-H2-39)

Measured, not read. Two eye-picture pairs and the C-H2-38 headset log.

**Anniversary (C-H2-38 pictures, 20:40:48, 110 deg):** the weapon sits ~50 px
(quarter scale) further LEFT in eye 1 than in eye 0; the world ~4 px. With
eye 0 the left eye, a near object shifts that way: the Saber first-person pass
draws the weapon with a real eye separation at ~26 cm, and the user calls this
gun good. E-H2-31's premise ("Saber's first-person pass has zero eye
separation") is therefore withdrawn - the view WITHOUT translation is the
camera-relative precision trick, not zero parallax.

**Classic (C-H2-36 pictures, 49.6 deg unpinned, raster camera centred):** the
gun sits ~150 px further RIGHT in eye 1 (scope 620 -> 760, ammo 650 -> 815 at
quarter scale) while the world matches to 0-2 px. That is the full eye offset,
the wrong way round, x3.1 by the unpinned FOV (3.1 x 50 = 155). A classic pass
therefore draws the weapon from the OTHER eye's camera - full magnitude, so the
camera it uses is an eye of ours, not the centre. The only per-pass camera that
is "the other eye" is the one the previous render_view popped: E-H2-12 says the
pop 0x955F60 re-applies slot[depth] and REBUILDS the derived projection/camera
state from the OUTER camera, and draw_first_person copies the render camera
globals whole (E-H2-20). Centring the raw fields at draw time (C-H2-36/37) could
not touch state rebuilt at the previous pop - which is exactly what happened.
The mechanism is inferred; the symptom (other-eye camera, full magnitude) is
measured, and C-H2-39 corrects the measured symptom.

**The re-anchor never ran (C-H2-38 log):** `read detour entered 6388: unhandled
6386 ... last call player 0 id -96724985 slot 0 handled 0 count 42`. The read's
return value is 0 on every call that hands back 42 valid nodes: it is not a
success flag, and the gate on it was the one closed term.

**C-H2-39:**
1. The gate is the node array itself. Per-slot cache makes the transform
   idempotent (the engine may hand back the array the mod wrote last time);
   the node space is classified at runtime (world / camera-relative / unknown -
   unknown is left untouched) and reported.
2. The owning core names the pass cameras (`Halo2Observer6Dof_SetFirstPerson
   PassCameras`): the tracked centre the eyes render from, and for classic the
   stale camera the pass draws from (previous pair's last eye for the first
   pass, this pair's first eye for the second) plus the correct eye. The
   geometry is moved tick -> frame centre, then correct eye -> viewing camera
   (x' = F_v F_c^T (x - c) + v), so the stale camera's image is the right eye's.
3. Anniversary draws the weapon with the frame's own +0x56C view again (the
   tick view of C-H2-33/36 is disabled, not deleted); classic camera centring
   is disabled, not deleted; the interpolation reset stays on (the geometry is
   at the witnessed tick exactly, which the re-anchor needs).
4. The eye-pair pixel check now measures the weapon box's best horizontal
   shift against the world band's, signed, every 2 s; and a renderer switch
   requests eye pictures 3 s later, so a short classic visit leaves pictures.

**Read first next time:** `weapon box ... best shift %+d px` vs `world band` -
same sign and larger = a fused gun; `geometry: ... node space world N /
camera-relative N / unknown N` - unknown means the re-anchor did nothing;
`classic stale-camera compensated N`.

## E-H2-35: the second eye's weapon was drawn at the stock 49.6 deg in every classic pair (C-H2-40)

A mod defect in the classic core since C-H2-27, found by reading the code
after the user rejected the C-H2-38 "FOV constant written 110.1 deg" line as
proof of anything (it was not):

- `WriteOuterCoverFovs` writes the first-person FOV constant ONCE per pair,
  before render_player_window.
- The inner render_view loop calls `RestoreOwnedSpans` in its `__finally`
  after EVERY eye - and `RestoreOwnedSpans` begins with
  `RestoreFirstPersonFovConstant()`, putting the stock 0x3F5D9734 back.
- `WriteEyeSpans` (per eye) rewrote the six vector spans and the two cover
  FOV fields - the WORLD's FOV, which is why the projection read-back AGREED
  for both eyes - but never the first-person constant.

So the first eye's weapon was drawn through the 110 deg cover and the second
eye's through 49.6 deg: one gun 3.1x the size of the other, in the same pair.
That is C-H2-35's "classic gun double vision", and C-H2-38's "it isn't working"
after the pin was repaired. The C-H2-36/37 pictures did not show it only
because the pin had failed and BOTH eyes were at 49.6 deg (E-H2-33).

C-H2-40: the constant is written in `WriteEyeSpans` for every eye, and read
back after each eye's render_view (before the restore) - `fpFovHeld(eye0/eye1)`
/ `fpFovLost(eye0/eye1)` in the stereo core line say which FOV each eye's
weapon pass actually found. The eye pictures remain the acceptance.

## E-H2-36: H2EK proves the native floaty-hand palette and shot-direction loop (C-H2-41)

This discovery used only the official H2EK `halo2_tag_test.exe` at
`N:\SteamLibrary\steamapps\common\H2EK`, SHA-256
`D0B71186D3948C48DDD02E2CCB88FA13E77E25A3D8F7FA60922F23A2A0073E36`.
It was imported into the ignored local H2EK analysis project and queried with
`tools/ghidra/DumpKitEvidence.java`. Retail `halo2.dll` was not used to invent
a binding; C-H2-41 consumes only the already signature-verified observer,
weapon-tick, and interpolator-read bindings from E-H2-23/E-H2-32.

The kit gives the first-person geometry path directly:

- `halo_frame_interpolator_get_interpolated_first_person_node_matrices` at kit
  RVA `0xD7CD0` has the exact `(user, animation graph, slot, node**, count*)`
  shape of retail `0x722850`. Its matrix stride is `0x34`; it interpolates the
  two frame banks through kit RVA `0xD83E0` and otherwise returns no palette.
- Its only first-person consumers are the two calls in
  `first_person_weapons.cpp` at kit RVA `0x306E04`, homologous to the existing
  retail render-path calls. The fallback is the current weapon palette at
  weapon-data `+0x320` (**corrected by E-H2-40**: the original reading of
  `+0x800` took Ghidra's DECIMAL `weapon_data + 800` for hex;
  `node_matrices_count` sits at `+0x31C`).
- The placement tail in the same source at kit RVA `0x307536` constructs a
  transform with an identity 3x3 and a translation, then assembles the node
  matrices. The helper at kit RVA `0xBB2F0` confirms that transform shape.
  These are absolute camera-relative node matrices, not a guessed world-space
  wrist bone. One rigid `Head^-1 * Controller` transform can therefore move
  the complete authored primary assembly while preserving all weapon, hand,
  marker, and animation relationships.

This corrects the C-H2-40 `CURRENT-STATE` inference that retail `0x722850`
was not the first-person geometry path. It is the exact homolog. The accepted
C-H2-40 tick hook resets all four first-person interpolation slots immediately
after placement; the kit read then correctly reports no two-bank palette on
almost every render call. C-H2-41 bypasses that reset only while controller
aim is requested, the palette hook is installed, and both head and aim poses
are live. Every other state executes the accepted reset unchanged.

The kit also proves that ordinary Halo 2 aim is the projectile direction:

- `units.cpp` kit RVA `0x48E350` updates the unit desired aiming vector at
  `+0x168` and native aiming vector at `+0x174`.
- `items/weapons.cpp` kit RVA `0x49C960` builds the projectile origin and
  direction before calling `weapon_barrel_simulation_action`.
- Its firing-only helper at kit RVA `0x47DC20` is called with its final flag
  set and copies the owning unit's three floats at `+0x174` verbatim into the
  shot-direction output. That output continues into the stock barrel/projectile
  creation path.

C-H2-41 therefore does not hook or rewrite projectiles. The shared calibrated
aim pose defines a controller ray; a bounded XInput servo steers Halo 2's
ordinary look loop until the published stock observer forward reaches that
ray. The engine then updates its own unit aim and launches the shot through its
own proven firing path. The reticle consumes the same current aim pose. A stale
observer publication, tracking loss, invalid matrix, missing optional palette
hook, or non-finite servo input fails only that feature to stock; it never
disarms the observer, either stereo renderer, or OpenXR.

The visible placement owns only first-person slot 0, matching the established
right-controller primary-weapon behavior in the other titles. A second/left
weapon slot remains stock until H2-specific dual-wield ownership is separately
proven. Headset acceptance must test the primary weapon and shot convergence
in both Anniversary and Classic, then regress Halo 3 on the same DLL.

### C-H2-41 headset rejection and C-H2-42 disable

The first C-H2-41 headset result rejected the interaction before visual
placement could be accepted: its virtual-stick servo replaced the physical
right stick and made hand motion turn the character camera. The required
behavior is independent ownership: the physical right stick keeps ordinary
character/camera turning, while the controller moves and aims only the gun and
the shot line. C-H2-42 withdraws `TitleCapability_ControllerAim`, which makes
the C-H2-41 servo and palette transaction dormant without deleting them. The
next candidate must write or hook the H2EK-proven unit/shot aim path directly;
it may not use XInput or any observer/camera field as the motion-control
actuator.

## E-H2-37: direct local-player shot direction without camera or XInput (C-H2-43)

C-H2-43 replaces the rejected actuator, not the H2EK-proven first-person
palette. Halo 2 is now explicitly excluded from `Game_ComputeAimStick`, so
the mod synthesizes no aim-stick value for this title. The physical right
stick continues through the established Halo 2 input path for ordinary
character/camera turning. Controller motion writes neither XInput, the
observer, nor a camera field.

The direct shot boundary starts in official H2EK and is matched to retail:

- H2EK `weapons.cpp` firing function kit RVA `0x49C960` has one call at kit
  RVA `0x49D0CD` to helper kit RVA `0x47DC20`. With its `use_unit_aim` flag,
  the helper copies the firing unit's three-float aiming vector at `+0x174`
  into the caller's direction output.
- Local BSim maps that helper to retail `halo2.dll+0x8F0F70` (similarity
  `0.3205`, significance `42`). Retail verification finds the same eight-
  argument shape, the same `+0x174/+0x17C` copy under argument 7, and one code
  caller at retail `+0x8E4FC8` inside the BSim-matched firing function
  `+0x8E4940` (significance `120.9`).
- The retail helper's 22-byte entry
  `48 8B C4 48 89 58 20 55 56 41 55 48 8D 68 C1 48 81 EC C0 00 00 00`
  occurs exactly once in the pinned Steam module
  (`DE65B4F4FDBF3F0A5EAB7431FE530DA17DD815599182DFD6AE9B7E21CF171946`)
  and remains at RVA `0x8F0F70`.

That helper is shared by all firing units, so helper identity alone is not
sufficient. The local-player guard is independently proved:

- H2EK `players.cpp` shows `players_globals->player_user_mapping` at globals
  `+0x0C`, player `unit_index` at player datum `+0x2C`, and a maximum of 16
  players. H2EK player update kit RVA `0x3FE80` also asserts that the unit's
  `player_index` matches the player iterator.
- BSim maps that complete player update to retail `+0x6A3910` with similarity
  `0.4873` and significance `243.5`. Retail verification preserves player
  datum `unit_index +0x2C`, stride `0x224`, the data-array storage pointer at
  `+0x48`, player data-array pointer slot RVA `0xE80A28`, and players-globals
  pointer slot RVA `0xE80A20`.
- The independent H2EK output-user iterator kit RVA `0x3D270` maps to retail
  `+0x6A1D50`; retail reads the same globals pointer, begins its four user
  mappings at `+0x0C`, and returns only occupied output users. Both matched
  identities are exact-RVA, unique-pattern prerequisites for the shot hook.

The detour first lets the stock helper author origin and direction. It then
resolves output user 0's live player unit read-only and requires the helper's
unit handle to equal it. AI, remote-player, missing-player, and invalid-data
calls retain their stock result. For the exact local player only, the authored
projectile origin is preserved and the returned direction is normalized from
that origin toward the configured-distance point on the controller ray. No
projectile object is rewritten, and all barrel/projectile creation after the
helper remains stock.

The first-person palette remains primary slot 0 only. Headset acceptance must
separately confirm: right-stick turning is unchanged; moving the controller
moves the gun without moving the camera; shots follow the visible gun; and the
behavior holds in both Anniversary and Classic.

### C-H2-43 headset result and C-H2-44 pitch correction

The first C-H2-43 headset result proved a single controller-carrier convention
error: physical controller up produced Halo 2 aim down. This is not a game look
setting and must not be corrected through XInput or the observer. The shared
calibrated OpenXR aim pose presents physical pitch-up to this Halo 2 carrier as
a negative local-X quaternion component; `Halo2BuildControllerCarrier` had
passed that component directly to Halo 2's camera basis.

C-H2-44 negates only that relative local-X component at the Halo 2 controller
carrier. Controller position and yaw are unchanged, the physical right stick
retains the C-H2-43 camera-turn path, and no camera field is written. The
first-person palette consumes the corrected current aim carrier. The direct
local-player shot path consumes the compositor-published reticle pose, so any
configured reticle stabilization is part of the shot target the player
actually sees. It builds that carrier with zero gun trim, then aims the
authored barrel origin at the exact configured-distance reticle point. A
deterministic pitch regression covers the headset-observed negative-X input;
a separate parallax regression requires an offset barrel origin to converge on
the presented reticle point rather than merely copying a parallel ray.

### E-H2-38 (C-H2-45): the controller-owned Halo 2 aim experiment is reverted

Three consecutive headset sittings rejected the same behavior. The player's
report on C-H2-43 and C-H2-44 is the same one that rejected C-H2-41: turning
the character and camera with the right stick had been taken away and given to
controller motion, which is "nothing like the other Halos". C-H2-44 was
additionally reported to have changed nothing at all.

C-H2-45 reverts the experiment. `TitleCapability_ControllerAim` is withdrawn
from Halo 2 in both `title_registry.cpp` and `game.cpp`, and a single build
switch, `kHalo2ControllerOwnedAimEnabled` in `src/common/halo2_render_logic.h`,
is checked before that capability inside `Game_Halo2ControllerAimActive()` so
re-granting the bit alone cannot silently re-arm a rejected behavior. The
switch disarms all three paths at once:

- C-H2-41's XInput aim stick (already unreachable: `Game_ComputeAimStick`
  returns false for Halo 2 before consulting any capability, and C-H2-45 keeps
  that early return permanently).
- C-H2-43's controller-placed first-person palette and the interpolation-reset
  bypass that came with it. The accepted C-H2-40 per-eye re-anchor and the
  reset both run on every pass again.
- C-H2-43's firing-helper detour at `+0x8F0F70`. It is now not installed at
  all, and `HaloMCCVR.log` names the revert as the reason.

The E-H2-37 identities above (`+0x8F0F70`, `+0x6A1D50`, `+0x6A3910`, the
players-globals and datum layout) remain verified and are still pinned by a
core test. Only their use is withdrawn.

**Negative result worth keeping.** C-H2-44's correction was
`relativeOrientation[0] = -relativeOrientation[0]`. That is not "invert pitch".
For a quaternion `q = (x, y, z, w)`, negating only `x` yields `F R^-1 F` with
`F = diag(1, -1, -1)`: a mirrored INVERSE rotation. It happens to invert a pure
pitch and to leave a pure yaw or roll alone, which is why it survived a reasoned
argument and a single-axis regression, but for any combined hand pose it also
reverses how yaw and roll compose onto the camera. It was never measured against
a live controller. The headset reported no change from it. The plain, unmirrored
head-relative rotation is restored, and a core test now pins that an OpenXR
controller pitched up (+x) carries the Halo 2 carrier's forward up (+world Z),
which is what both engines' own conventions already say.

**For whoever resumes the floaty-gun goal.** Excluding Halo 2 from
`Game_ComputeAimStick` provably stops the mod from writing the right stick at
all, so the player's C-H2-43 experience did not come from an XInput write. The
two remaining suspects are the controller-placed first-person assembly and the
bypassed interpolation reset, which C-H2-43 armed together in one candidate.
Do not re-arm those two together again: place the weapon on the controller with
the accepted C-H2-40 reset still running, get a headset result, and only then
consider the reset.

### E-H2-39 (C-H2-46): the two defects behind the rejected floaty hands

Halo 2's floating hands are re-armed. The mechanism is unchanged from E-H2-36 -
first-person slot 0's node palette is moved rigidly onto the controller through
one `Head^-1 * Controller` transform, preserving every authored weapon, hand,
marker and animation relationship. Two defects that had nothing to do with the
mechanism are fixed.

**Defect 1: the carrier was mirrored.** C-H2-44's
`relativeOrientation[0] = -relativeOrientation[0]` is `F R^-1 F` with
`F = diag(1, -1, -1)`, a mirrored INVERSE rotation (E-H2-38). The preserved
per-eye frame dump from the C-H2-44 session
(`HaloMCCVR-halo2-eye0.bmp`, Outskirts, classic renderer) shows the battle
rifle tilted far off the hand as a result. Removed in C-H2-45; it stays removed.

The unmirrored mapping is now derived rather than assumed. `Halo2ApplyLocalQuaternion`
maps local `+x -> camera right`, `+y -> camera up`, `+z -> -camera forward`.
Working each axis through with Halo 2's own basis (`+Z` world up, right =
forward x up):

| local axis | Halo 2 result | OpenXR head-local result | agree |
| --- | --- | --- | --- |
| `+x` (pitch) | forward tilts toward world `+Z` (up) | forward `(0,0,-1) -> (0, sin, -cos)` (up) | yes |
| `+y` (yaw) | forward tilts toward `+Y`, which is LEFT | forward tilts toward `-X`, which is LEFT | yes |
| `+z` (roll) | right tilts up (head tilts left) | right tilts up (head tilts left) | yes |

All three agree, so the plain head-relative rotation is the correct carrier and
no title-local sign correction belongs anywhere in this path.

**Defect 2: the mesh and the bullet were built from different poses.** C-H2-43
placed the visible palette from `VR_GetAimPose` with the `gun_forward_m` trim,
while its firing-helper detour aimed from the carrier built out of
`VR_GetPresentedReticleAimPose` with no trim; C-H2-44 kept them apart. Two
different origins and two different orientations means the bullet does not
follow the gun the player is pointing, which is exactly the complaint.

C-H2-46 introduces one builder, `BuildFirstPersonCarrier`, and both consumers
call it. `VR_GetAimPose` is the shared mount-calibrated aim pose - `gun_yaw_deg`,
`gun_pitch_deg`, `gun_roll_deg` and two-handed aim are already folded into it -
and it is the same pose the compositor draws the VR crosshair quad from. Mesh,
crosshair and bullet therefore agree by construction rather than by tuning. A
core test pins it: a shot leaving the carrier's own origin travels exactly along
the carrier's forward at 2 m, 10 m and 50 m.

**What is still true and must stay true.** `Game_ComputeAimStick` refuses Halo 2
before any capability is read, so the mod never writes the right stick for this
title. The physical right stick keeps ordinary character/camera turning and the
headset keeps pitch (C-H2-23). The controller reaches exactly two things: slot
0's node matrices, and the direction the local player's own firing helper
returns. It reaches no camera field, no observer field and no XInput byte.

**Still open.** The left hand rides the gun rigidly, as authored. Putting it on
the LEFT controller independently needs Halo 2's own first-person node identity
(which of slot 0's 42 nodes are the left wrist/forearm/upperarm chain), which no
Halo 2 evidence in this repository establishes yet. Do not infer it from Halo 3,
Reach or Halo 4 node names or indices.

## E-H2-40: Halo 2 tags its own left and right hand - the node binding for an independent left controller (H2EK, 2026-08-23)

Discovery source: the official H2EK only - `halo2_tag_test.exe`
(SHA-256 `D0B71186D3948C48DDD02E2CCB88FA13E77E25A3D8F7FA60922F23A2A0073E36`),
the already-analysed local kit Ghidra project, and nine
`model_animation_graph` tags plus one `.jmm` extracted read-only from the kit's
own shipped archive. Retail `halo2.dll` was not read. This answers the question
C-H2-46 left open: which of first-person slot 0's nodes are the left arm.

**No node index needs to be hardcoded. Halo 2 marks the hands itself.**

### The animation-graph skeleton node record - PROVEN

Argument 2 of the interpolator read (retail `+0x722850`, kit `0xD7CD0`) is NOT a
pointer: it is the `model_animation_graph` (`'jmad'`) **tag index**. The kit only
integer-compares it, and its sole first-person consumer
`first_person_weapons.cpp` (kit `0x306E04`) passes
`first_person_weapon_interface->animations.index` under the assert
`"weapon_data->animation_manager.get_graph_tag_index() == first_person_weapon_interface->animations.index"`.

The skeleton nodes are a tag_block at kit graph `+0x14`, element stride `0x20`.
Proven twice: `get_node_count()` (kit `0x1512D0`) is literally
`MOV AX,[ECX+0x14]; RET`, and kit `0x14E600` iterates the same block at stride
`0x20`. The record layout is proven byte-for-byte by the v0 -> current upgrade
function at kit `0x14C420` and the field array at kit `0xA6F328`
(`sizeof(animation_graph_node)` = `0x20`):

| offset | size | field |
| --- | --- | --- |
| `+0x00` | 4 | name (`string_id`) |
| `+0x04` | 2 | next sibling index (i16) |
| `+0x06` | 2 | first child index (i16) |
| `+0x08` | 2 | **parent index (i16)**, `-1` at the root |
| `+0x0A` | 1 | **model flags (byte)** |
| `+0x0B` | 1 | joint flags |
| `+0x0C` | 12 | base vector |
| `+0x18` | 4 | vector range |
| `+0x1C` | 4 | z_pos |

`model flags` bit names come from the flag-name array at kit `0xA6F2F8`, in bit
order - PROVEN:

| bit | mask | name |
| --- | --- | --- |
| 0 | `0x01` | primary model |
| 1 | `0x02` | secondary model |
| 2 | `0x04` | local root |
| 3 | `0x08` | **left hand** |
| 4 | `0x10` | **right hand** |
| 5 | `0x20` | **left arm member** |

The array the mod already holds is indexed by animation-graph node index -
PROVEN: kit `0x307536` asserts
`"animation_graph_node_index>=0 && animation_graph_node_index<weapon_data->node_orientations_count"`,
and the publisher `0xD87E0` writes `count = get_node_count()`. So the interpolator's
`*count` and the graph's node count are the same number, which makes their
equality a free correctness gate.

### What the shipped rigs actually contain - PROVEN

`masterchief fp_battle_rifle`, 42 nodes - the exact count the retail log reports
for slot 0:

```
 0  base          parent -1  flags 0x05 primary|local root
 1  l_upperarm    parent  0  flags 0x21 primary|left arm member
 2  r_upperarm    parent  0  flags 0x01 primary
 3  l_forearm     parent  1  flags 0x21 primary|left arm member
 4  r_forearm     parent  2  flags 0x01 primary
 5  l_hand        parent  3  flags 0x29 primary|LEFT HAND|left arm member
 6  r_hand        parent  4  flags 0x11 primary|RIGHT HAND
 7..36  finger roots/mids/tips, left ones carrying left arm member
37  gun           parent  6  flags 0x06 secondary|local root
38  magazine      parent 37  flags 0x02 secondary
39  OpHandle      parent 37  flags 0x02 secondary
40  safety        parent 37  flags 0x02 secondary
41  camera_control parent 0  flags 0x00
```

`l_hand` = `0x06000097`, `r_hand` = `0x0600009B`, `camera_control` = `0x0E0000E0`,
each an exact index+length match against the kit's own 2462-entry `string_id`
table at kit RVA `0x7B50A8`.

Cross-checked on nine shipped first-person graphs (eight Master Chief, one
Elite), 36 to 44 nodes: **every one** has exactly one `left hand` node, exactly
one `right hand` node, left chain `5 -> 3 -> 1 -> 0`, right chain
`6 -> 4 -> 2 -> 0`, and a `secondary|local root` weapon node parented to the
RIGHT hand. The Elite rig has 36 nodes and its weapon root at index **31, not
37** - so node counts and the weapon-root index vary. **Only the flags are
invariant. Bind on the flags, never on an index.**

### The binding, when the retail side is verified

Inside the `+0x722850` detour, with `(user, graphTagIndex, slot, node**, count*)`:

1. Resolve `graphTagIndex` to `'jmad'` tag data.
2. Read the skeleton-node block at stride `0x20` and REQUIRE its count to equal
   the interpolator's `*count`. Mismatch fails the feature to stock.
3. `rightWrist` = the single node with `flags & 0x10`; `leftWrist` = the single
   node with `flags & 0x08`. Missing or duplicated fails to stock.
4. Arm chains by walking `parent (+0x08)` to `-1` or to a `local root` (`0x04`).
   Equivalently, `flags & 0x20` marks the whole left arm.
5. Wrist descendants = transitive closure over `+0x08`. The held weapon is
   exactly the `flags & 0x02` set, whose local root parents to the right wrist.
6. To drive the left hand independently: apply the existing rigid
   `Head^-1 * RightController` transform to `{rightWrist} + descendants` (which
   already contains the gun), and a separate `Head^-1 * LeftController`
   transform to `{leftWrist} + descendants`. The matrices are absolute
   camera-relative frames (stride `0x34`, E-H2-32), so per-subtree rigid
   transforms compose without touching the parent nodes.

### What H2EK CANNOT give - do not guess these

1. **The retail cache-format offset of the skeleton-node block inside a `jmad`
   tag.** `graph + 0x14` is an EDITING-format kit offset (12-byte `tag_block`);
   retail maps are cache format. The node CONTENT and ORDER transfer - they are
   the same authored tags baked into the shipped maps - the block offset does not.
2. **Retail homologs of `tag_get` (kit `0x60660`) and
   `animation_graph_definition_get` (kit `0x150650`).** Legitimate signature /
   BSim targets now that H2EK explains them, but that is a retail VERIFICATION
   step and it has not been done.
3. **Retail `first_person_weapon_data` offsets.** In the kit: `+0x7C` graph tag
   index, `+0x10C`/`+0x114` weapon remap table, `+0x110`/`+0x214` **hands** remap
   table, `+0x318` orientation count, `+0x31C`/`+0x320` node matrices. If they
   hold on retail, `+0x214` is a shorter path - it lists exactly the animation
   node indices the arms model uses. Also unverified on retail.
4. **Dual-wield slot 1 was not traced.** `first_person_weapons.cpp` loops two
   weapon slots, each with its own graph and palette. Do not assume slot 1
   mirrors slot 0.

Until item 1 or item 3 is verified against the pinned retail module, the left
hand keeps riding the gun rigidly as authored. A hardcoded index 5 / index 6
binding is consistent with all nine shipped graphs but is NOT proven invariant,
and shipping it would be exactly the guess `AGENTS.md` forbids.

## E-H2-41 (C-H2-47): the retail verification of E-H2-40 - the left hand can be bound

Every item E-H2-40 left UNKNOWN is now verified against the pinned retail
module. H2EK explained the behavior; retail was used only to locate the
homologs and confirm the layout, exactly as `AGENTS.md` requires.

### The three readers, all matching exactly once

| function | retail RVA | AOB skew | what proves it |
| --- | --- | --- | --- |
| `animation_graph_definition_get(tag_index)` | `0x0079EEA0` | +8 | `MOV RAX,[rip]; MOVZX ECX,CX; ADD RCX,RCX; MOVSXD RAX,[RAX+RCX*8+8]; ADD RAX,[rip]; RET`. The kit's `tag_get` is inlined into it; no standalone retail `tag_get` exists. The 8 padding bytes disambiguate it from two further inlined copies. |
| `get_skeleton_node(graph, index)` | `0x0079F430` | 0 | `MOVSXD RAX,[RCX+0x10]` then `SHL RAX,5` - the node block address and the `0x20` stride in one instruction pair. |
| `get_node_count(graph)` | `0x0079F470` | +15 | literally `MOVZX EAX, word [RCX+0x0C]; RET`. |
| `find_node_by_model_flags(graph, mask)` | `0x0079E8D0` | 0 | scans at stride `0x20` testing `MOVZX ECX, byte [R10+RAX+0x0A]; AND ECX,EDX; CMP ECX,EDX`. |

**Verified in this repository, not merely reported:** an offline PE mapper
scanned both editions' `halo2.dll` (Steam
`DE65B4F4...CF171946`, Store `81E5F41A...E6BE689D`, both file size 15807960,
`SizeOfImage 0x02A38000`). All four patterns matched **exactly once**, at their
pinned RVAs, in **both** editions.

### The layout, PROVEN on retail

Cache format shifts the skeleton-node block 8 bytes down from the kit's `+0x14`
(a cache `tag_reference` is 8 bytes where an editing-format `tag_block` is 16):

- `graph + 0x0C` node count (uint16), `graph + 0x10` node block address.
- node stride `0x20`; `+0x00` name (string_id), `+0x08` parent (int16, `-1` at
  the root), `+0x0A` model flags (byte).
- `+0x04` next sibling and `+0x06` first child are read by NO located retail
  code. They stay unverified and C-H2-47 does not read them; the parent walk
  gives the same descendant sets.

**The engine uses the hand bits itself.** `find_node_by_model_flags` is called
from `+0x81A8E0` with `0x10` (right hand) and `0x08` (left hand), and that same
function culls a hands remap table to `flags & 0x20` (left arm member) for the
second weapon slot. The bits mean on retail exactly what H2EK named them.

### The interpolator's count IS the graph's node count - PROVEN

`weapon_data[+0x318] = get_node_count(graph)` -> `weapon_data[+0x31C] = [+0x318]`
-> the publisher at `+0x7232F0` writes that number into the bank ->
`+0x722850` returns it as `*count`. So `*count == *(uint16*)(graph + 0x0C)` is a
free exactness gate, and C-H2-47 uses it as one. It matters:
`animation_graph_definition_get` never compares the group tag, so a wrong index
would silently hand back another tag's bytes - the count equality is what
catches that.

The first-person matrix palette is **64** entries (`weapon_data + 0x320`, stride
`0x34`, struct `0x1028`). C-H2-47 refuses any palette past it, which is also
what makes its 64-bit left-subtree mask exact rather than merely convenient.

### What C-H2-47 does with it

Inside the existing `+0x722850` detour: resolve the graph from the read's own
second argument, gate on the count equality, read `(parent, model flags)` for
every node through `get_skeleton_node`, and build the binding purely
(`Halo2BuildFirstPersonArmBinding`). Where `find_node_by_model_flags` is
available its answer must agree with ours, or the binding is refused. The result
is cached per graph id, so the hot path is a comparison.

The left wrist and its descendants then take the LEFT controller's carrier;
everything else - including the gun, which the engine parents to the right wrist
in every shipped rig - keeps the right controller's carrier from C-H2-46. The
node matrices are absolute camera-relative frames (E-H2-32), so two independent
rigid transforms compose without touching the arm nodes between them.

**Failure isolation.** The left hand is its own transaction. A missing
signature, a wrong count, a malformed parent tree, duplicated or absent hand
flags, a disagreement with the engine's own lookup, or an untracked left
controller all drop the slot back to C-H2-46's single right-controller
placement, counted in the periodic report as `left hand: N rigid fallback`. The
gun and the right hand never depend on any of it.

### Still UNKNOWN - do not guess

1. `+0x04` / `+0x06` sibling and child links: unverified on retail, unused.
2. **Dual-wield slot 1 was never traced end to end.** What is now proven is that
   retail treats it DIFFERENTLY - `+0x81A8E0` culls the hands table to left-arm
   members only for the second slot. Do not assume slot 1 mirrors slot 0.
   C-H2-47 owns slot 0 only.
3. The kit's `first_person_weapon_data` valid-bit NAMES map onto retail bits
   `0x02` / `0x04` by behaviour, not by symbol.

## E-H2-42 (C-H2-48): C-H2-47 headset rejection and implementation audit - 2026-08-23

The headset rejected C-H2-47 with four directly observable faults: offsets on
both hands, both arm assemblies riding the right controller, shots missing the
visible crosshair, and controller tracking becoming incorrect after turning
away from the initial forward direction. Three code-level defects explain the
report without adding a new retail binding.

1. **Wrong ownership complement.** C-H2-47 transformed `leftSubtree` with the
   left carrier and every other node with the right carrier. E-H2-40's proven
   42-node rig shows that complement contains root 0, both upper arms 1/2, both
   forearms 3/4 and `camera_control` 41. The implementation therefore did
   exactly what the headset showed. C-H2-48 computes both disjoint wrist
   descendant masks. The left mask alone takes the left carrier; the right mask
   (which contains the weapon because its local root parents to the right wrist)
   alone takes the right carrier. Only the two wrist-to-root ancestry chains
   (upper arms and forearms, excluding wrists and the root) collapse to
   `0.0001`. Root, `camera_control`, and unrelated control nodes remain byte-for-
   byte stock. The bounded scale-collapse presentation mechanism is the same
   one used by the accepted Halo 3 path and the forced Reach/Halo 4 paths.
2. **World/local matrix reversal.** The first-person matrices at `+0x722850`
   are camera-relative (E-H2-32). Their carrier rotation must therefore be
   `H^T C`, controller axes expressed in the current camera frame. The function
   comment stated that equation, but called `Halo2BuildWorldDeltaRotation`,
   whose proven contract is `C H^T`. Those products coincide in the identity
   forward frame and differ after turning, exactly matching the headset result.
   C-H2-48 computes `H^T C` by axis dot products. A regression test applies the
   same 90-degree world/body turn to camera and controller and requires the
   local rotation and translation to remain identical.
3. **Raw shot pose versus presented crosshair pose.** The compositor optionally
   smooths `VR_GetAimPose` into `g_reticleAimPose`, publishes that pose, and
   places the visible crosshair on its ray. C-H2-47's firing helper instead read
   a newer raw `VR_GetAimPose`, so nonzero `aim_stabilization` deliberately made
   the visible ray and shot target different. C-H2-48 accepts only the fresh
   (at most 250 ms old) seqlock-published presented reticle pose and converges
   Halo 2's engine-owned muzzle origin on that exact displayed point. With no
   fresh visible crosshair, only this optional direction substitution stays
   stock.

No new address, struct offset, or signature is introduced. The E-H2-41 graph
and firing bindings remain the evidence base. `Game_ComputeAimStick` still
unconditionally refuses Halo 2, so this correction does not write XInput or a
camera field. A missing hand binding/pose now leaves the complete optional
palette presentation stock for that frame instead of invoking C-H2-46's
known-bad whole-slot/right-controller fallback.

### C-H2-48 headset rejection

The next headset sitting rejected C-H2-48 with the same four visible faults,
reported as worse. The preserved Steam log is
`out/test-runs/a64da3e-halo2-c48-rejected-20260823-0941/HaloMCCVR.log`
(SHA-256 `73E3789AF0447486689BA01E819A587515FFE1519EF0984F05D8D7029E4C1D7A`).
It recorded 5,152 successful controller placements with zero failures, one
successful hand-binding build, and 18/18 local-player shot substitutions. This
falsifies the claimed visible/coordinate semantics of the successful
`+0x722850` writes. C-H2-49 disables the complete C-H2-46/47/48 transaction.
Do not iterate another transform on that upstream bank.

## E-H2-43 (C-H2-50): the final visible-palette root composition - 2026-08-23

Halo 3, ODST, Reach and Halo 4 all apply controller ownership to a private or
final render palette, after the title has completed its authored pose and root
composition. Halo 2 C-H2-46/47/48 was the structural exception: it rewrote the
interpolator source before Halo 2's renderer consumed it.

Official H2EK supplies the missing boundary. In the pinned
`halo2_tag_test.exe`, `first_person_weapons.cpp` kit RVA `0x306E04` obtains the
interpolated source bank from kit `0xD7CD0`, then gives that source, the
first-person root, the model mapping and the render-packet destination to the
packet fill at kit `0x306D16`. That fill authors a 12-byte packet header followed
by `0x34`-byte node matrices. Therefore the interpolator output is not itself
the final visible palette: the packet fill still maps and composes it through
the first-person root.

Retail is used only to verify that H2EK-explained operation. The homologous
retail first-person renderer is `halo2.dll+0x8181F0`. Immediately after its two
existing interpolator reads at `+0x81858D` and `+0x81872E`, its node loops call
the generic matrix composer at `+0x81861E` and `+0x81876E`. The calls are exactly
`compose(first_person_root, interpolated_source_node,
render_packet_destination_node)`. The composer is retail `+0x72A150`; its
decompile proves `destination = root o source`, including basis, translation
and scale. Its 32-byte entry
`48 83 EC 48 49 3B C8 75 24 0F 10 01 8B 41 30 0F 10 49 10 89 44 24 30 0F 11 04 24 0F 10 41 20 48`
is unique in the pinned module. The two admitted return addresses are
`+0x818623` and `+0x818773`.

C-H2-50 keeps `+0x722850` only as a read witness. It records the exact source
pointer, graph id and count supplied by that render invocation. The generic
composer detour first runs stock and may alter its destination only when all of
the following agree: the caller return is one of the two final first-person
loops; the source lies at an exact `0x34` stride inside that witnessed bank;
the graph/count passes the E-H2-41 engine-native wrist binding; controller poses
are live; and the output matrix is finite. This is the same source-identity plus
final-palette shape used by the accepted Halo 3 and Reach paths.

At each final wrist matrix, the rigid world motion is
`D = desired_controller o inverse(stock_wrist)`. That one motion is carried
over the exact wrist descendant mask. Every final-palette node outside the two
hand masks is scale-collapsed after root composition, where no hierarchy is
evaluated again. Thus the left hand alone follows the left controller; the
right hand and its tag-proven held-weapon descendants follow the right
controller; and no arm/root/control geometry can remain visible. The rejected
interpolator-space controller switch remains false.

The projectile hook remains the H2EK-proven firing-only helper from E-H2-37,
guarded to output user 0's unit. C-H2-50 corrects its remaining crosshair
divergence: the compositor places the visible reticle at the presented aim
position plus its local `-Z` ray times `crosshair_distance_m`. The firing target
now uses that exact stabilized pose with zero `gun_forward_m`; that setting is
a mesh-only presentation trim and adding it to the shot created a second ray.
The engine-owned muzzle origin is preserved and converges on the displayed
point. A missing final-palette hook withholds the shot hook too, so this optional
feature fails as one stock presentation rather than mixing a stock gun with a
controller-owned shot.

Dual-wield slot 1 remains outside this claim. Headset acceptance must cover
primary weapons in Anniversary and Classic, including 90- and 180-degree body
turns, and must separately record edition, OpenXR runtime, headset and refresh
rate. Halo 3 regression is required on the same DLL because the shared VR-side
presented-reticle publication is consumed but not changed.

## E-H2-44 (C-H2-50 rejection/C-H2-51 revert): installed is not executed - 2026-08-23

The player's first C-H2-50 Steam Anniversary run rejects E-H2-43's runtime
claim. The exact log is preserved at
`out/test-runs/d43aaad-halo2-c50-rejected-fatal-20260823-1023/HaloMCCVR.log`,
SHA-256 `0AE96156F50F33CCF9FD7FABEB4A4833B4F2F981ABB3B788E67957BB77FD1605`.
It identifies source `d43aaad0fff64f3d38f74c042b87506760445b9c`, Steam,
SteamVR/OpenXR 2.17.7, headset `SteamVR/OpenXR : oculus`, and a 120 Hz panel.

The hook installer reported the C-H2-50 final-palette and direct weapon-aim
transactions armed, but the periodic runtime witnesses stayed exactly zero
through the playable mission:

- weapon tick: 0 placements and 0 witnessed records;
- interpolator read detour: 0 entries;
- final palette: 0 changed, 0 right, 0 left, 0 collapsed, 0 refused;
- direct shot aim: 0 calls and 0 applications.

Therefore the uniquely matched `+0x72A150` composer and the two
`first_person_weapons.cpp` return sites are not the live first-person consumer
for this Anniversary path. The code identity proof remains a fact, but the
claim that this boundary owns the player's visible Anniversary hands is
falsified. The zero counters also prove C-H2-50 did not transfer head-owned aim
to the controllers. A future attempt needs a positive execution witness in the
actual Anniversary renderer path before it may alter a matrix.

At the second-mission transition, the log records observer and Anniversary
stereo removal before the module generation changed. The replacement generation
then repeatedly failed coherent `game_time_globals` reads and stopped presenting.
Windows Application Error event 1000 and WER event 1001 record an access
violation (`0xc0000005`) in `halo2.dll` at RVA `0xDE63E`. No mod exception is
logged, and the C-H2-50 optional controller callbacks had admitted zero calls.
This proves the fatal error and preserves its exact native fault location; it
does not prove that an unexecuted controller hook caused it. C-H2-51 still
disables the full rejected optional transaction as the required safety revert.

## E-H2-45 (C-H2-52): the live final first-person packet - 2026-08-23

C-H2-50 failed because it hooked a generic composer behind a witness chain that
never executed in the reported Anniversary mission. The replacement discovery
starts again from official H2EK, not from retail guesswork.

In H2EK `halo2_tag_test.exe`, `first_person_weapons.cpp` kit RVA `0x306E04`
builds the complete first-person packet array. Its packet-fill helper at kit RVA
`0x306D16` takes the first-person root, source palette, authored remap and
destination packet, writes the 12-byte packet header, then emits root-composed
`0x34`-byte matrices. BSim selects retail `halo2.dll+0x818080` as the fill
homolog; retail verification shows the same arguments and matrix loop. The
verified outer retail homolog is `+0x8181F0`, whose 29-byte entry is:

`48 89 5C 24 18 4C 89 4C 24 20 89 54 24 10 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57`

It returns the number of packets written to argument 7. Each packet has stride
`0xD0C`: a 12-byte header and capacity for 64 final matrices. The packet with
header object `unit_object` is the authored hands model. The separate packet
with header object `weapon_data+0x04` is the held gun. This outer boundary feeds
both Classic and Anniversary/Saber and executes whether the source came from an
interpolator bank or the authored `weapon_data+0x320` palette.

The same H2EK structure and the verified retail homolog establish user-array
pointer RVA `0x187C300`, user stride `0x20FC`, first weapon-data offset `0x0C`,
slot stride `0x1028`, graph id `+0x7C`, gun-node count `+0x10C`, hands-node
count `+0x110`, authored hands remap `+0x214`, and animation-graph node count
`+0x31C`. The counts are intentionally not required to be equal: the remap is
destination-hands-node to source-animation-node. Every remap entry must be -1
or inside the graph binding before any packet byte may change.

C-H2-52 calls the packet builder stock first, resolves the source wrist masks
from E-H2-41's engine-native graph flags, maps them through the authored hands
remap, and computes each final-world wrist delta as
`desired_controller * inverse(stock_wrist)`. Only the exact remapped left and
right wrist descendant nodes receive their respective deltas. Every other node
in the hands packet is scale-collapsed, so no root, upper arm, forearm or body
geometry remains. Every matrix in the separate primary gun packet receives the
right-wrist delta. The rejected interpolator controller transaction remains
disabled, and the game's normal first-person interpolation reset is preserved.

Packet ownership publishes a timestamp only after the complete transaction
succeeds. Controller reticle/shot ownership requires that positive witness to
be at most 250 ms old. The existing H2EK firing-helper hook remains limited to
output user 0's unit, preserves the engine muzzle origin, and directs the shot
to the exact fresh compositor-presented crosshair point. A missing packet,
invalid remap, invalid graph, missing controller or non-finite matrix leaves the
whole optional hands/gun/shot feature stock for that frame without disarming
camera, stereo or OpenXR.

## E-H2-46 (C-H2-53): consecutive-mission same-module hook lease - 2026-08-23

The rejected log proves the old lifecycle removed the observer and Anniversary
detours when MCC briefly withdrew title/liveness proof, then installed hooks
again after the same H2 DLL reappeared. WER proves the later native access
violation at `halo2.dll+0xDE63E`; disassembly verifies it is a dereference after
an array search can return no element. That evidence does not identify hook
churn as the native function's internal cause, but it does identify remove /
free-reference / recreate against a retained game DLL as an avoidable loading
transition that must not occur.

C-H2-53 therefore gives each enabled H2 hook group one session-long physical
module lease: observer and its optional packet/firing hooks, Anniversary
stereo, Classic stereo, and the renderer switch guard. A missing/ambiguous
title, level, cold-proof or renderer signal parks the lease and publishes its
hot gates false; every detour calls stock while parked. A later generation at
the same non-zero module base adopts the new generation, clears all level-local
pose/serial/palette witnesses, requests recenter, and re-arms without calling
MinHook remove/create or releasing the `HMODULE` reference. Physical teardown
is reserved for an explicitly failed OpenXR path or a proven non-zero different
module base. This prevents consecutive H2 mission loads from churning hooks or
allowing `halo2.dll` to unload underneath retained trampolines.

The cumulative C-H2-53 offline verification covers the exact packet constants, invalid-remap
fail-before-write behavior, independent left/right final wrist placement,
complete non-hand collapse, separate-gun right-wrist motion, stock interpolation
reset policy, Release compilation, core tests, and the Reach consistency gate.
Dual-wield slot 1 remains outside the ownership claim because E-H2-41 proves it
has different culling semantics.

## E-H2-47 (C-H2-53 rejection / C-H2-54): MCC owns every game-DLL lifetime - 2026-08-23

C-H2-53 is rejected. The preserved Steam run is
`out/test-runs/faac277-halo2-c53-rejected-consecutive-load-20260823-1712/HaloMCCVR.log`,
SHA-256 `DC58753D19894D152B9110CE5AC9D29D2278A0B7D1E7F306BFADFAD3A5E8A71C`.
It identifies source `faac277c9f41d6494d32c083604c74d66a102d1e`. At the first
level exit the game-time singleton became disposed/uninitialized and the C-H2-53
observer/Anniversary leases parked. When Halo 2 was selected again,
`TitleRuntimeState` still reported Halo 2 generation 1: the physical DLL never
became absent because the mod's session-long `HMODULE` references prevented
MCC's unload. The replacement level never established coherent game time or
presentation. WER then recorded `0xc0000005` at `halo2.dll+0xDE63E`.

The same run positively executes C-H2-52: 14,044 final-packet builder calls,
3,679 owned packets, 47,827 right nodes, 47,827 left nodes, 18,395 collapsed
nodes, 12,638 gun nodes, and 62/62 direct-shot substitutions. Therefore the
aim implementation stays enabled; only C-H2-53's loader-lifetime behavior is
reverted by commit `f182e8c`.

The cross-title source audit finds the same unsafe ownership primitive in every
newer engine core: ODST, Reach, Halo 4 and all enabled Halo 2 hook groups called
`GetModuleHandleExW(FROM_ADDRESS)` without `UNCHANGED_REFCOUNT`, retained the
returned handle, and called `FreeLibrary` during teardown. H2, Reach and Halo 4
cold/preflight paths also temporarily changed the refcount. The existing Reach
load-bounce evidence in `game.cpp` independently records the mechanism: a mod
reference held across MCC's requested unload defers the actual unload to the
mod's later `FreeLibrary`, racing the loader. Halo 3 did not hold that reference,
but retained MinHook records beyond the level-liveness boundary and attempted
to clean them only after a later mapping appeared.

C-H2-54 makes the title mapping an exact-base, non-owning identity. Every
`GetModuleHandleExW(FROM_ADDRESS)` used for a game DLL now includes
`GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT`, and no title lifecycle calls
`FreeLibrary`. Hook retirement instead uses the already proven common
level-liveness boundary, which the rejected H2 log measured before module-set
ambiguity/unload. H2 observer, Classic/Anniversary stereo, packet/firing hooks
and renderer guard, ODST, Reach, Halo 4, and Halo 3 all retire their hook epoch
while the current mapping remains owned by MCC. Existing callback counters and
quiescence rules still protect trampoline removal; existing stock-state restore
and next-level readiness gates still run. A mapping that unexpectedly vanished
is never written through.

This is one shared loader-lifecycle correction, not a feature toggle. C-H2-52
hands/gun/crosshair-shot behavior and all other title features remain enabled
and reinstall after the next level proves live. Offline verification at this
stage: Release build PASS, core tests PASS, Reach consistency gate PASS, 14-call
loader-refcount source gate PASS, producer/installer parser PASS, and diff check
PASS. Exact source `85afbd0bf4000b404038a167b66d5715f5489571`, package
`85afbd0-halo2-c54-shared-unload-safe-hook-epochs-20260823-222828978Z`, and DLL
SHA-256 `B0DCE4E27F7BD2A6A6173D44488E538ACC9DBA7B19E72415E1C31827E978D436`
were installed and independently hash-verified in both editions without launch
or config change. The headset result rejects C-H2-54 as a complete fix: it did
not crash, but its second same-generation Halo 2 level failed to reinstall the
dependent hooks, as E-H2-48 records.

## E-H2-48 (C-H2-54 rejection / C-H2-55): cached proof must rebuild level-derived bindings - 2026-08-23

The preserved Steam C-H2-54 run is
`out/test-runs/85afbd0-halo2-c54-no-crash-rehook-failed-20260823-1755/HaloMCCVR.log`,
SHA-256 `B31798F0908C7A1E0AA7206F3F0701B8D9A8144512D5B1B6C7696A592BD46117`.
It proves the unload-safe epoch change removed the consecutive-level crash.
Generation 1's first level passed cold proof, installed and executed observer,
Classic/Anniversary stereo, final-packet and firing hooks, then retired them at
the level-liveness boundary. When a second level opened in the same generation,
only the renderer switch guard reinstalled; observer, stereo, packet and firing
hooks stayed absent and stereo pairing remained zero.

The source exactly explains this split. `Halo2ColdObservation_Rearm` resets the
level-derived graphics-mode validity, observer-result array and Classic gate
address, while intentionally retaining the expensive full-image proof for the
same physical module instance. The next gate therefore sees completed cached
proof and skips `RunColdObservation`; before C-H2-55, that routine was also the
only caller of `ObserveGraphicsMode`, so the cleared bindings could never return.
The guard could reinstall from cached proof alone, while every missing hook
depended on one or more of those derived bindings.

C-H2-55 preserves the cached image proof and C-H2-54's non-owning, unload-safe
hook epochs. After each new level gate opens, a passed same-instance proof with
cleared derived bindings invokes `ObserveGraphicsMode` exactly once for that
gate epoch. Normal hook readiness checks then consume the rebuilt addresses.
Failure remains isolated and loud: a failed rebind leaves only dependent
features stock for that level and does not disarm the camera core or OpenXR.
The C-H2-52 hands/gun and presented-crosshair shot implementation remains
enabled and unchanged.

Offline verification PASS: Release build, core tests including the complete
derived-rebind decision matrix, Reach consistency gate, loader-refcount and
rehook source gates, producer/installer parsing, and diff check. Exact source
`d87314402bcf75fee7e515b9e477d2adb2b39eb9`, package
`d873144-halo2-c55-consecutive-level-rehook-20260823-230708465Z`, manifest
SHA-256 `F1969CEA6C80BD2C42ED5A0331B4F5743D331698CAE61A62C1F12BFC13BA8DDE`,
and DLL SHA-256 `D1DB75E70E4EA75EDE7FEFA87DCE8509B25855EF1D5CA70D34E85FEBEFCCC2EA`
were installed and independently hash-verified in both Steam and Store editions.
The rejected C-H2-54 bytes were preserved separately for each edition. MCC was
not launched and neither existing configuration was changed. The player's
2026-08-23 headset result confirms the fix and explicitly promotes C-H2-55 to
the new accepted cumulative development baseline.

## E-H2-49 (C-H2-56): preserve authored mounts and own every local shot result - 2026-08-23

The next headset report rejects only C-H2-55's inherited C-H2-52 optional
hands/gun/shot behavior: both hands and the separate held gun float at wrong
offsets, and aim remains tied to the head on one axis instead of consistently
following the visible crosshair. C-H2-55's consecutive-level lifecycle result
remains accepted independently.

The preserved live evidence is the Steam log written by exact source
`d87314402bcf75fee7e515b9e477d2adb2b39eb9` on SteamVR/OpenXR 2.17.7 with an
Oculus headset at 120 Hz. The final packet boundary executed continuously and
reported 4,331 owned packets, 69,296 right nodes, 69,296 left nodes, 21,655
collapsed nodes, 17,324 separate-gun nodes and zero refusals. The visual defect
therefore comes from the successful transform rather than hook admission,
binding, or remap failure.

The code audit identifies the discarded authored frame. C-H2-52 computed
`desired_controller * inverse(stock_wrist)` and thus made the raw wrist bone
equal the controller root. That throws away Halo 2's live authored
camera-root-to-wrist offset and orientation. It then carries that incorrect
wrist delta over the independent gun packet, throwing the gun through space by
the same error. No new binding is needed: E-H2-45 already proves the packet
builder's `position/forward/up` arguments are the root through which every
destination matrix has just been composed. C-H2-56 therefore applies
`controller * inverse(authored_root) * stock_matrix` to each admitted subtree.
This preserves the complete live root-to-wrist and wrist-to-gun relations while
replacing only the root that owns them. The existing engine-authored remap and
left/right descendant masks still select ownership, and every existing invalid
root, graph, remap, count, pose and finite-value guard leaves the optional
feature stock for that frame.

The same log exposes an independent boolean gate in the claimed shot boundary:
54 firing-helper calls produced 30 substitutions, five classified non-owned
units, and 19 unclassified stock results. The detour required the helper's
`useUnitAim` flag before it evaluated the already-proven local-player guard, so
flag-false calls could not be classified at all. E-H2-37 proves
the helper's completed origin/direction output is the firing function's
boundary; it does not make controller ownership conditional on that flag.
C-H2-56 therefore preserves the stock helper call and local-unit equality but
replaces the completed local direction for both flag values, converging the
stock-authored muzzle origin on the fresh compositor-presented crosshair point.
AI, remote units, missing/stale reticle poses and invalid values remain stock.

Offline verification before packaging: cumulative Release build PASS and
`halomccvr_core_tests` PASS. The packet fixture proves independent hands,
complete non-hand collapse, the separate right-hand gun, and preservation of
nonzero authored root offsets. Headset acceptance remains required in both
Anniversary and Classic through 90/180-degree body turns, followed by the exact
candidate identity and result; this evidence entry does not advance the
accepted pointer.

The first package attempt contained the correct gameplay source `991e596` and
installed DLL SHA-256
`1C3945374F26ABCE95E319655FB0A857B13D842CF59B8B695E53EEE23F8B680C`,
independently equal in Steam and Store,
but the producer still emitted a stale `C-H2-55` id, behavior description and
package slug. It is superseded before headset use. The producer metadata is
corrected and a newly committed, manifest-identified C-H2-56 package is required
before testing; the stale label is not accepted as C-H2-56 evidence.

The next correctly labeled package from source `daf3e65` built, passed tests and
was staged as
`daf3e65-halo2-c56-authored-hand-weapon-mounts-20260823-234023690Z`, but the
installer rejected it before copying because its exact manifest validator still
required the preceding C-H2-55 slug and behavior fields. This is a safe
packaging-contract refusal, not a runtime result. Producer and validator are
advanced together before generating the final headset candidate.

The final exact package is
`74152be-halo2-c56-authored-hand-weapon-mounts-20260823-234134640Z`, source
`74152be45b51b3956e30a137032cabdb740f0ce7`, manifest SHA-256
`21B72ED320D86991DC6B13B9685FCD5259465D247E1E92F90E049AD2A306646F`,
DLL SHA-256
`7C8EA2B3BD54E6CA236BE5BE7EFB1CFA124735DC76E50F4478B2148DAEB70B0F`,
and launcher SHA-256
`87F1DF6202BE26065BAEB769BC19E06B526F25DDC5FCBF6FE75FAE779C54A902`.
Its manifest identifies C-H2-56 and names both corrected policies. The verified
installer preserved the preceding bytes independently and installed this exact
DLL to Steam and Store; independent post-install SHA-256 checks match in both.
MCC was not launched and neither configuration changed. This remains an
unaccepted headset candidate.

## E-H2-50 (C-H2-56 rejection / C-H2-57): successful writes, wrong consumers - 2026-08-23

The player's Steam headset result rejects C-H2-56: the gun moved onto the
player's face, shots behaved exactly as before, and the presentation regressed.
The preserved exact-source log is
`out/test-runs/74152be-halo2-c56-face-gun-aim-unchanged-20260823-1845/HaloMCCVR.log`,
SHA-256 `DE01A1E99DC7479684C68B7ABA64C49C8D6F420E4C6A006BF753EEB30A80F401`.
It records source `74152be45b51b3956e30a137032cabdb740f0ce7`, Steam,
SteamVR/OpenXR 2.17.7, an Oculus headset at 120 Hz.

Runtime execution is positive and rules out another dead-hook explanation. The
final packet reported 2,839 owned packets, 45,424 right nodes, 45,424 left
nodes, 14,195 collapsed nodes, 11,356 gun nodes and zero refusals. Thus
`controller * inverse(authored camera root)` is proven to execute and proven
not to be a valid controller attachment: it carries Halo 2's face-relative
first-person placement onto the controller replacement and leaves the gun at
the face. The shot hook reported 69 applications out of 69 calls with no stock,
non-owned, missing-unit or exception result before the last level transition,
yet the player saw no projectile change. The completed helper direction is
therefore not the visible projectile owner in this live firing path. Do not
iterate another direction transform at that boundary.

C-H2-57 sets the optional final-packet/shot master switch false. It preserves
the failed code inert, keeps the already-false interpolator transaction inert,
and leaves camera, stereo, observer 6DOF, OpenXR and lifecycle rehook untouched.
The next implementation must compare Halo 4's accepted controller mount,
two-hand/config consumption and final projectile owner, then find Halo 2's own
homologous consumer from H2EK before any new runtime write.

## E-H2-51 (C-H2-58): stable physical wrists, rigid support grip and traced projectile direction - 2026-08-23

The replacement starts from the accepted Halo 4 behavior in source, not from
C-H2-56's failed equation. Halo 4 freezes right aim, raw left controller and
two-hand state in one prepared-frame snapshot; builds controller positions from
the pre-HMD body origin and recenter displacement; gives each final wrist the
physical controller translation while retaining only the live eye-root-to-wrist
rotation; applies the right wrist's exact rigid delta to the visible left wrist
during two-hand aim; and carries that same right delta once into the adjacent
held model. Its scale is part of the wrist/subtree affine transform.

Halo 2 already has the corresponding proven inputs and consumer: E-H2-45's
final world packet and E-H2-41's authored wrist subtrees. C-H2-58 publishes the
exact-frame controller data beside `Halo2VrRenderSnapshot`, maps it through the
observer publication's stock camera and recenter reference, and stages the
complete hands/gun transaction before writing. For each wrist it builds
`controller_rotation * inverse(authored_root_rotation) * stock_wrist_rotation`,
but copies the physical controller position directly. It never composes the
authored root-to-wrist translation that put C-H2-56 on the player's face. Free
left uses the raw left controller with mirrored configured yaw/roll and ordinary
pitch. Support left instead receives the right wrist's rigid delta, retaining
the weapon's authored grip. Right and left scales are applied about their own
wrist pivots, including descendant translations; the separate gun receives the
right affine delta once. Invalid input leaves the stock packets byte-identical.

The official H2EK firing function at kit RVA `0x49C960` was traced past the call
to helper kit RVA `0x47DC20`. It passes `&local_1c4c` as the three-float origin
and `&local_1c40` as the three-float direction. After the helper returns, that
direction is copied to the firing locals (`local_1dd8` / `local_1dd0`), receives
the weapon's random error, is normalized and enters projectile creation. The
separate `local_1c94` data is impact/action state, not projectile direction.
This corrects E-H2-50's inference from the prior headset result: the helper is a
real direction consumer; C-H2-56's 69 writes did not prove those writes were
materially different from its head-derived carrier. C-H2-58 retains the already
verified retail helper binding, builds its target from the stable recenter/body
mapping used by the mesh, and adds angular telemetry against the stock result.

Offline verification before packaging: Release DLL compilation PASS,
`halomccvr_core_tests` PASS (stable recenter mapping, mirrored trim, independent
affine wrist/gun placement, rigid two-hand support lock, invalid-remap
write isolation), Reach consistency gate PASS, and diff check PASS. Headset
acceptance remains required and this entry does not advance the accepted build.

## E-H2-52 (C-H2-58 rejection / C-H2-59 / C-H2-60): packet ownership must consume the observer publication, not the independently latest VR serial - 2026-08-23

The player's exact C-H2-58 Steam run is preserved at
`out/test-runs/7ffcf2b-halo2-c58-hooks-admitted-then-serial-gated-20260823-1916/HaloMCCVR.log`,
SHA-256 `A213F65FB88759C415996BED283C42F1BF68B4D75219A67A6EF793CAAF20E610`.
It identifies source `7ffcf2b44ea42b5543d7b2191ec2ca984e676a2e`, Steam,
SteamVR/OpenXR 2.17.7, an Oculus headset at 120 Hz. All C-H2-58 hooks installed.

At 19:16:21 the packet boundary reported 2,203 calls / 507 owned and all 13
firing-helper calls materially changed direction. A brief
gameplay/loading/gameplay transition occurred at 19:16:23. By 19:16:26 packet
ownership reached 689 and shot ownership 26. From then until exit the packet
builder continued from 3,955 to 6,998 calls, but ownership remained exactly 689;
the firing helper continued from 48 to 65 calls, but ownership remained exactly
26. No binding, matrix, remap or exception refusal occurred. Thus the feature's
recent-packet witness expired and both visible ownership and shot ownership fell
stock while their hooks continued executing.

The source has one ordering-dependent gate that exactly explains the freeze:
`BuildStableFirstPersonCarriers` required the separately read latest
`Halo2SynchronousVrRenderSnapshot.preparedSerial` to equal the latest
`Halo2ObserverPosePublication.serial`. E-H2-21 already proves Anniversary's
scene thread frequently consumes an observer publication after a newer VR
serial exists; its accepted eye path embeds the exact head/eye sample in each
observer publication rather than imposing latest/latest equality. C-H2-58
violated that established title-local threading contract.

C-H2-59 disables C-H2-58 without deleting it. C-H2-60 extends the existing
observer publication snapshot with right aim, raw left controller and two-hand
state copied from the same prepared sample used to construct `stock`, `tracked`
and the recenter reference. The packet hook consumes that single publication
and no longer reads or compares an independent latest VR serial. Generation,
pose validity, controller validity, graph/remap, matrix, range and full staged
write guards remain. Runtime telemetry now separates weapon-state, publication,
controller-snapshot, carrier and ownership failures and reports last/maximum
right and left wrist displacement in millimetres. Release compilation and core
tests pass; headset acceptance remains required.

## E-H2-53 / E-H2-54 (C-H2-60 rejection / C-H2-62): the visible matrix copy occurs inside the builder; native unit aim is the shared engine consumer - 2026-08-23

The exact C-H2-60 Steam log is preserved at
`out/test-runs/c3ebb45-halo2-c60-material-writes-no-visible-effect-20260823-1929/HaloMCCVR.log`,
SHA-256 `0BD91BEA0588A7B738B9CA657D81A439E254B9A0BA61AD4F791CB11BE8B31314`.
It identifies source `c3ebb45ae781835b52799da7ab77a67a05c8aa70`. The packet
transaction continuously reached 2,262 owned builds and measured wrist motion
up to 0.631 m. The firing detour materially changed 21 directions by up to
32.635 degrees. The player's result was still "nothing changed." This rejects
both consumers, not their admission or arithmetic. C-H2-61 disables that
behavior in commit `95b40a7`.

The official H2EK packet fill remains kit `+0x306D16`, but retail optimization
inlines the primary hands and weapon fills into outer builder `+0x8181F0`. More
importantly, retail's verified outer tail reads the registered callback pointer
at RVA `0x187C2E8` and, while still inside the builder, invokes it once per
completed packet with `(user, model, owner, weapon_slot, matrices)`. The pointer
is assigned at retail `+0x69A67` to `halo2.dll+0x6BB40`. That callback consumes
the matrix pointer immediately and copies node transforms into the live render
model under its own synchronization. Therefore any matrix edit after
`+0x8181F0` returns is too late. The `+0x6BB40` 29-byte entry pattern and the
builder pattern each occur exactly once in the pinned retail module.

C-H2-62 detours both functions. The outer detour publishes only thread-local
identity, graph/remap, controller and authored-root context before calling
stock. The registered callback detour recognizes the hands packet by owner unit
and slot `-1`, applies the independently tagged left/right wrist subtrees, then
recognizes primary held model slot 0 and applies the exact right-wrist delta.
It calls the stock callback only after those writes. No post-return packet path
is enabled.

For aim, official H2EK `units.cpp` function kit `+0x48E350` is the native
`unit_update_aiming`: it updates `desired_aiming_vector` at unit `+0x168` and
`aiming_vector` at `+0x174`, validates the result, and publishes an aim-change
witness. BSim maps it to retail `halo2.dll+0x8FDF50` (similarity `0.335462`,
significance `55.5718`). Retail preserves the same members (its unit pointer is
typed as `ushort*`, so `+0xB4*2=+0x168` and `+0xBA*2=+0x174`). Its 31-byte
wildcarded entry pattern occurs once. Retail object-datum accessor `+0x8D7000`
and its entry pattern also occur once. C-H2-62 runs stock aim update first, then
for output user 0's exact unit writes both native vectors from the same immutable
observer/controller publication used by the visible mesh. The rejected
firing-helper detour is not installed.

## E-H2-55 (C-H2-62 partial result / C-H2-63): Classic bypasses the registered callback and consumes persistent packets directly - 2026-08-23

The exact C-H2-62 Steam headset run is preserved at
`out/test-runs/d72ebf7-halo2-c62-anniversary-working-classic-flat-no-hands-20260823-1955/HaloMCCVR.log`,
SHA-256 `2A2A4ECE9FD448404E7FA80E3A1641113EACE65E8E497DADC1D1F1607EC2C3D0`.
It identifies source `d72ebf79214a89bc7d81c6dee9504687eb0f65f6`, Steam,
SteamVR/OpenXR 2.17.7, an Oculus headset at 120 Hz. The player confirms the
hands/gun finally move in Anniversary, but Classic remains unchanged and the
Classic gun appears in a separate plane without stereo depth. During the exact
Classic interval 19:53:11.874-19:53:16.604, per-eye world rendering remained
live while the weapon box's best horizontal shift was `+0 px`. The C-H2-62 log
still counted 8,914 Anniversary visible-consumer hand applications and 8,914
gun applications, including background remastered packets while Classic was
visible. Those counts do not establish Classic ownership.

Read-only official H2EK analysis identifies the missing renderer split. Outer
builder kit RVA `0x306E04` invokes its registered callback only under the exact
condition `callback != 0 && publish_to_renderer != 0`. H2EK Classic caller kit
RVA `0x2B6DE0` passes final argument 0 and writes the result to persistent
globals `DAT_01230000` / `DAT_0122FFFC`. H2EK render-object consumer kit RVA
`0x2B6140` reads those packets directly and returns model, matrices at packet
`+0x0C`, and the render-model node count. Thus a callback-only implementation
cannot affect Classic.

Pinned-retail verification preserves both explicit consumers. Builder
`halo2.dll+0x8181F0` has two direct callers. Classic homolog `+0x7E5430` calls it
with final argument 0 into retail persistent packet globals. Anniversary caller
`+0x81BFB0` passes 1 into stack packets, which triggers registered consumer
`+0x6BB40`. This corrects E-H2-53's overbroad statement: post-return edits are
too late for the publish/callback path, but are exactly the available ownership
boundary for Classic's retained persistent packets.

C-H2-63 preserves the headset-working Anniversary callback. On only the
builder's explicit `publish_to_renderer == 0` call while the Classic stereo core
is armed, it recognizes unit and weapon packets by the same proven object IDs,
applies the same staged split hands/gun transform after stock returns, and
retains their exact pointers and counts. The existing exact Classic
`draw_first_person` detour then brackets the native draw. Before each eye it
stages both palettes and applies `viewing * inverse(correct)` to every final
world node; after the draw it restores the controller-owned centre packets.
This compensates the already-measured stale Classic first-person camera without
changing a view matrix or creating a separate compositor gun layer. Bounds,
finite values, packet freshness, exact pass-camera publication and transactional
restore are required. The rejected interpolator, composer, post-return generic
packet and firing-helper switches remain false.

The packaged C-H2-63 source is
`14a7c466593766f0a27aaf9642a8207768bb9fda`; its DLL SHA-256 is
`AC7522093B489D9A4BE8580A82C28D5D48FF9B2C5F6F2B0684858866BBEEA0AD`.
The player's accepted Steam headset run is preserved at
`out/test-runs/14a7c46-halo2-c63-accepted-good-baseline-steam-20260823-2052/HaloMCCVR.log`,
SHA-256 `FCF552242413ADB87E037F0F0B7DF2464C53391132AA2159C3949C60BAAAC707`.
It identifies SteamVR/OpenXR 2.17.7, an Oculus headset at 120 Hz and exercises
both renderer directions. Final telemetry records 13,965 Classic packet calls,
13,965 owned, 13,927 Classic eye calls, 13,896 compensated, 16 without a fresh
packet, 15 without a named pass and zero refusals. The player explicitly calls
the result a good baseline for the next development session while retaining
future bug fixing/tuning. This advances the accepted Halo 2 pointer from
C-H2-55 to C-H2-63. The pixel-box heuristic remained `+0 px` in its sampled
lower-right region and is retained as an inconclusive diagnostic, not allowed
to override the headset acceptance.

## E-H2-56 (C-H2-64): separate free-hand turnover from live two-hand barrel seating - 2026-08-23

The player's first tuning report after accepting C-H2-63 identifies two
presentation defects in both Classic and Anniversary: the independently tracked
left hand is flipped in its idle/free state, and the two-hand support pose needs
to sit closer to the barrel. This is headset evidence about presentation, not a
new engine binding. E-H2-55 already proves both renderers consume the same split
hand/gun algebra at different renderer boundaries, so no renderer-specific
address or title-global transform is introduced.

C-H2-64 changes only the left target selected inside that shared Halo 2 packet
transaction. Free mode retains the controller-rerooted wrist translation and
scale, then applies a determinant-+1 pi rotation about the live controller
forward axis. The axis is normalized and finite-checked; failure leaves the
complete optional hand/gun packet stock. Two-hand mode retains C-H2-63's
authored rigid support-grip rotation, but replaces its stale flat-screen wrist
translation with the immutable publication's live left carrier. The same
publication's right aim already uses that support point as its barrel-line end,
so the visible support hand and barrel aim now share one sample.

The right wrist and separate gun still receive the exact existing
`controller * inverse(authored root)` rigid delta, native unit aim remains at
the proven `+0x168/+0x174` members, and Classic per-eye compensation remains
after this centre-packet presentation solve. Tests pin free turnover, live
support translation, staged-write isolation and the unchanged gun delta.

The headset result rejects that inference. The exact Steam log from source
`b41330d61ff9c6be06f85138ef18e2ce98557402` is preserved at
`out/test-runs/b41330d-halo2-c64-visible-difference-no-alignment-steam-20260823-2119/HaloMCCVR.log`
(SHA-256 `1426DED54B29ED273286205036D61229171B190DCB613512D9F3EDD0BD2347AF`).
It proves continuous execution in Anniversary and Classic with zero palette or
Classic-eye refusals. The player saw a difference but none of the required
alignment improved: free left hand, double-barrel support location, and visible
barrel-to-crosshair direction all remain wrong. C-H2-65 disables C-H2-64 and
restores C-H2-63. A future correction requires weapon-authored geometry; no
additional constant hand rotation or translation is supported by this result.

## E-H2-57 (C-H2-66): live authored barrel frame, pump grip and anatomical palm turnover - 2026-08-23

Official H2EK tags supply the geometry C-H2-64 lacked. The shipped shotgun
first-person render model authors root node 0 `gun`, pump node 3 as its child,
`primary_trigger` on node 0 at local translation `(0.169144, 0, -0.003689)`
with identity rotation, and `left_hand` on the pump. Thus the gun root's local
`+X` is the firing/barrel direction and the support contact inherits the live
pump animation. The shotgun weapon tag independently selects `primary_trigger`
for its muzzle-flash attachment.

The same official exports prove the free-hand anatomy. Master Chief's
`l_thumb_low` is node 11, a direct child of tagged `l_hand` node 5, at local
translation `(0.008537, -0.004043, 0.014680)`; all four finger bases are more
than twice as far from the wrist. The Elite shotgun rig has the same shape with
`l_thumb_low` node 10 and fewer fingers. C-H2-66 therefore resolves the thumb
base without a hardcoded node index: among the final hands packet nodes whose
verified animation-graph parent is the tagged left wrist, select the closest
live origin. A malformed topology or non-finite/degenerate ray refuses only
this packet transaction.

C-H2-66 reads the current gun packet root and constructs one proper rigid delta
whose rotation maps its live `+X/+Z` authored frame to controller forward/up,
while its translation pivots at the physical right hand. That same delta moves
the right subtree, every gun node, and the left subtree during two-hand aim, so
the pump contact cannot separate from the barrel. In free mode the existing
controller-rerooted left wrist is turned by pi around the resolved live thumb
ray (`R = 2aa^T - I`), preserving translation, scale and thumb side.

Retail's verified callback order is hands then gun. Anniversary therefore
defers only the hands consumer call until the immediately following gun packet,
applies the combined staged transaction, then calls the stock consumer in its
original hands/gun order. If no matching gun arrives, the outer builder flushes
the untouched hands before returning. Classic already retains both packets and
applies the same combined helper directly before its accepted per-eye camera
compensation. All writes remain staged and fail open to stock; no camera,
observer, projectile, input, or non-Halo-2 path is changed.

## E-H2-58 (C-H2-67): C-H2-66 headset rejection - 2026-08-24

The exact Steam / SteamVR / Oculus / 120 Hz run is preserved under
`out/test-runs/161dd68-halo2-c66-left-hand-whack-arbiter-face-arms-steam-20260824-0650/`.
The user reported the left hand was wrong in both renderers and that Arbiter's
arms floated at the face rather than attaching like Chief. The log proves both
Classic and Anniversary consumed owned hands and gun packets with no palette
refusals, so this is not evidence of a renderer callback failure. It rejects
the C-H2-66 thumb-ray pi rotation and rejects claiming one undifferentiated
Chief/Elite presentation from aggregate packet ownership. C-H2-67 disables the
C-H2-66 selection and restores the prior safety path before further work.

## E-H2-62 (C-H2-71): the shared observer FOV is the pre-visibility headset cover - 2026-08-24

The accepted C-H2-70 Steam run and screenshot are preserved under
`out/test-runs/4ea2035-halo2-c70-accepted-chief-arbiter-both-renderers-steam-20260824-0729/`.
At head pitch about 46 degrees, Anniversary retains nearby walls but removes a
broad lower band of floor/world geometry. The same run proves the late scene
camera rasterises `126.5 x 110.1` degrees exactly with zero projection
mismatches, while its logged stock camera is only `110.2 x 91.9` degrees. The
missing lower band therefore cannot be repaired by widening the late raster
projection again.

Existing H2EK/retail evidence identifies one earlier title-native input shared
by both renderers: `observer_result+0x4C` is the vertical FOV. Retail Classic
builder `+0x7DF5A0` copies it to camera `+0x28`. Retail bridge `+0x5F510` reads
the same field and calls Saber's `+0xBC560`, which derives the matching
horizontal FOV and refreshes the camera. Read-only decompilation for this result
adds the final ordering fact: Saber view builder `+0x1C7740` copies its supplied
camera into the new `0x758`-byte view record, calls matrix builder `+0x1C6D80`,
and only then publishes that record to the collection consumed by scene render
`+0x2DF190`. The observer final-transform hook is upstream of both the Classic
builder and the Saber bridge.

C-H2-71 therefore expands only `observer_result+0x4C` from the engine's stock
value to the proven symmetric headset vertical cover. It takes the maximum, so
an authored wider FOV is preserved exactly. A one-float allow-list prevents the
feature from writing horizontal FOV, aspect, FOV ratio, clusters, leaves or any
other observer field. Invalid OpenXR FOV, invalid stock FOV or an unreadable
write is counted and leaves this feature stock without withdrawing pose/stereo
ownership. The late eye raster cover remains unchanged. This is an
evidence-backed candidate awaiting headset confirmation, not yet a proven
culling fix.

## E-H2-61 (C-H2-70): semantic hand axes and Elite wrist-pivot collapse - 2026-08-24

The corrected semantic mount is title-independent player behavior but is built
through Halo 2's own data: controller carrier forward, computed left, and up
form `[forward,left,up]`; the active H2 rig marker local transform is then
inverted from that mount. This is the accepted Halo 3/ODST/Reach hand-frame
contract and differs exactly from rejected C-H2-68's camera-column order
`[right,forward,up]`. Chief and Elite keep their distinct official marker
rotations and translations.

The official graph parent chains split the four non-root arm ancestors into
left `5 -> 3 -> 1` and right `6 -> 4 -> 2`. For Elite only, C-H2-70 copies each
hidden ancestor record to the corresponding final solved wrist, then applies
the existing `0.0001` visibility collapse. Thus all cross-weighted hidden
influences on one wrist share one pivot and cannot draw a strip back toward the
stock face anchor. Root and unrelated records retain the existing collapse;
Chief retains its existing partition. The full hands/gun transaction remains
staged and fail-open. Tests pin both rig marker compositions, the exact
left/right ancestor split, and co-location of hidden Elite records at each
solved wrist.

The player explicitly accepted C-H2-70: "ok hands are great now." Preserved
Steam / SteamVR 2.17.7 / Oculus / 120 Hz evidence is under
`out/test-runs/4ea2035-halo2-c70-accepted-chief-arbiter-both-renderers-steam-20260824-0729/`.
The Arbiter log SHA-256 is
`399C68D996D48EBC8ADF0C9461094DD7D5ED0FDAD9806E4172B92C5170DDE55D`;
it exercises Anniversary and Classic, owns 8,233 Elite packets, co-locates
32,932 hidden Elite arm records, and records zero palette refusals. The Chief
log SHA-256 is
`5B4FBD2C1EB9496D56638043404A33E12E01281760EAA748A0A7E4EA58AB5867`;
it exercises both renderers, owns 18,121/18,121 Chief packets, and records zero
palette or Classic-eye refusals. Source is
`4ea203566054cdb11b953af330f129fa2f0cdeed`; the independently verified
installed DLL SHA-256 is
`763048E03CE2918197D5DF372CFFF16E4F9FEA6972B3618E0D2AC5A252742A16`.
C-H2-70 therefore advances the accepted Halo 2 development pointer from
C-H2-63. The newly reported high-look world-culling defect is independent and
does not qualify this hand/wrist acceptance.

## E-H2-60 (C-H2-69): C-H2-68 axis and collapsed-pivot rejection - 2026-08-24

The Steam / SteamVR / Oculus log preserved under
`out/test-runs/4b860bf-halo2-c68-arbiter-wrist-stretch-left-axis-permutation-steam-20260824-0708/`
records 15,871 Arbiter contexts and 15,871 owned packets, including Classic and
Anniversary, with zero ownership misses. Arbiter admission is therefore proven.
The user's screenshot rejects its presentation: a long wrist/forearm wedge runs
from the controller-owned hand toward the stock arm pivots, and both Chief and
Arbiter left-hand rotations remain wrong.

The rotation defect is exact. Halo 3's accepted semantic controller basis is
`[forward,left,up]`; C-H2-68 passed Halo 2's camera basis conversion
`[right,forward,up]` into the hand-marker solve. That pre-marker 90-degree axis
permutation invalidates both rig results. The stretched Elite mesh is the same
linear-skinning class already proven in Reach: hidden arm influences collapsed
at distinct translations while the cross-weighted wrist moves elsewhere draw a
ribbon between those pivots. C-H2-69 disables the marker presentation while
retaining the independently proven Arbiter admission.

## E-H2-59 (C-H2-68): rig marker solve and Arbiter packet admission - 2026-08-24

The user clarified that Arbiter was never hooked: his stock arms remained at
the face, while Chief alone showed the attempted controller ownership. The old
context gate required `(weapon_data[0] & 7) == 7` before resolving a graph or
examining a final packet. H2EK proves the outer builder call, graph/count,
authored remap, hands packet object (`unit_object`) and gun packet object
(`weapon_data+0x04`) as the final packet contract; it does not establish that
three-bit byte predicate as character-independent. C-H2-68 therefore removes
only that prefilter. All of the exact graph, remap, object, count, finite-value,
controller-publication and staged transaction gates remain. New per-rig context
and owned-packet counters record Chief and Arbiter separately.

Official H2EK render models provide the rig-specific semantic hand frames.
Master Chief's `left_hand` is on node 5 with translation
`(0.022750,-0.008561,0.000398)` and quaternion
`(0.016078,0.073613,0.085080,-0.993521)`. Arbiter's `left_hand_elite` is on
node 5 with translation `(0.033088,-0.009315,0.000442)` and quaternion
`(-0.001854,0.001501,-0.001833,-0.999995)`. Their official graph anatomy also
distinguishes five direct digit bases for Chief from four for Elite. C-H2-68
selects by that live topology and solves `wrist * markerLocal = left controller
mount`; it does not copy Chief's wrist frame into Arbiter or add another guessed
pi rotation. Two-hand mode instead keeps the authored support grip rigid inside
the gun-root delta, while the gun's proven +X barrel axis maps to the crosshair
ray. The identical helper is consumed by Classic and Anniversary packets.
