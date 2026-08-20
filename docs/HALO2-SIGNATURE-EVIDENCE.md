# Halo 2 signature evidence

Status: **C-H2-1 passed Steam target-title headset/log validation on
2026-08-20; the accepted-build pointer remains `8ee18fd` pending the required
Halo 3 regression of `f8928bb`'s shared title-worker/lifecycle changes.**
It is a read-only cold-observation candidate. It does not claim stereo, 6DOF,
head tracking, controller input, aim, HUD, haptics, or any engine hook/write.
The machine-readable subset is `docs/HALO2-EVIDENCE-MANIFEST.json`.

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
  closed. C-H2-1 installs no hook either way.
- No H2 engine field is writable yet. In particular, the shared
  `render_far_clip_distance` scanner/writer is not a Halo 2 binding.

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

Retail window stride is `0x120` (H2EK x86 is `0x118`). Retail window fields
are type `+0x00`, player index `+0x04`, output user `+0x08`, render camera
`+0x0C`, raster camera `+0x80`, and trailing view argument `+0xF8`. Each camera
is exactly `0x74` bytes.

### Native asymmetric projection route

H2EK and retail both implement title-native off-center projection controls:

| Camera offset | Meaning |
| ---: | --- |
| `+0x58` | asymmetric-frustum enable byte |
| `+0x5C`, `+0x60` | normalized X/Y frustum center |
| `+0x64` | common half-extent scale |
| `+0x68` | alternate pixel-offset enable |
| `+0x6C`, `+0x70` | X/Y pixel offsets |

The builder clears `+0x58`, so eventual eye substitution must occur after the
builder. The player transaction reads the render camera through the helper once
before render view. Those fields are read-only through the transaction, which
supports restoring only the eye-overwritten fields afterward.

A blind 0x74-byte restore is unsafe: the player transaction may legitimately
update render-camera `z_far` at camera `+0x44`. Replaying the entire player
transaction twice is also not yet authorized. `render_view` increments counters
and runs visibility, scene, interface/HUD, and debug lifecycle work. Static
evidence does not prove that duplicate execution is side-effect safe; this is a
runtime question for the next isolated candidate.

## C-H2-1 runtime contract

- Existing registry row/slot only; no new title descriptor or alias.
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
stereo design. It does not authorize any Halo 2 hook/write or stereo/6DOF
claim, and it does not advance the repository accepted-build pointer until the
required Halo 3 regression passes.

## Offline verification method

Both retail PE files were parsed into complete mapped images: headers and each
section's raw bytes were copied to `VirtualAddress`, virtual gaps were zeroed
through `SizeOfImage`, and every possible RVA was scanned. Raw files were also
scanned independently, including certificate/overlay bytes. Function semantics
and call topology were inspected in offline Ghidra projects under ignored
`out/ghidra`; no MCC process was launched or accessed.
