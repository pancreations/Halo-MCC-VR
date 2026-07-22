# Halo 3: ODST signature evidence

The eight byte-identical signatures were verified 2026-07-21 on
`feature/odst-bringup` at `4dddbc1`. The twelve re-derived candidates were
verified after documentation checkpoint `0144982`; runtime source remained
unchanged from `4dddbc1`.

This is an evidence manifest, not an ODST runtime-support manifest. It covers
the eight production signatures that match `halo3odst.dll` byte-for-byte and
the twelve ODST-specific candidate signatures derived for production patterns
that do not. It does not validate any consumed structure layout or authorize an
ODST hook. ODST must remain stock when selected: `runtimeSupported=false`, with
game-hook dispatch still restricted to `GameTitle::Halo3`.

## Evidence inputs

- Halo 3 comparison module:
  `halo3/halo3.dll`, 11,127,768 bytes, SHA-256
  `B209D8454B12DC77E54CCD2C9924EC8D44B8619D21CF98E36FFAF601E67EFB63`.
- ODST module under test:
  `halo3odst/halo3odst.dll`, 11,496,920 bytes, SHA-256
  `5BB20976EFDFD9E1CE59C589339804725FEC239021027C8D65B2733EAB94829A`.
- ODST editing kit: `H3ODSTEK`, build tag
  `2023.09.08.177386.1-QFE1` / `QFE1`.
- Editing-kit executables inspected for ODST-specific render, camera, bone,
  skinning, root-matrix, and brightness semantics:
  - `atlas_tag_test.exe`, 28,510,448 bytes, SHA-256
    `354EC94158AECCE3E9D0F6463023AD5FA6D2AFE49B390E6067EBC17465C63C2D`.
  - `sapien.exe`, 25,756,912 bytes, SHA-256
    `ECC54B409FD2C35C32D76598B8BDF7119150F2EBAC559242DAF884FE527D8F1C`.

No game binary, editing-kit binary, tag, map, extracted kit content, or memory
dump is stored in this repository. The only game-code bytes recorded below are
the already-shipped production AOB definitions being verified.

## Method

1. Read each pattern directly from `src/dll/game.cpp`; no pattern was relaxed
   or rewritten for this survey.
2. Reconstructed each PE's in-memory image using `SizeOfImage` and section RVAs,
   then applied the production wildcard semantics across that complete image.
   This avoids mistaking a unique raw-file hit for a unique loaded-module hit.
3. Required exactly one match in both the Halo 3 and ODST images and required
   the hit to be in `.text`.
4. Used x64 PE exception metadata to bound the containing functions and compared
   their complete disassembly. The normalized comparison preserves mnemonics,
   registers, non-RIP field displacements, and non-control-flow constants while
   ignoring only relocation-dependent RIP displacements and relative branch or
   call destinations.
5. Corroborated the inferred purposes against the installed ODST editing-kit
   executables. Their retained ODST source/assertion identifiers cover
   `render/views/render_view.cpp`, `render/render_cameras.cpp`, render and
   rasterizer camera position/forward/up access, base-node matrix accounting,
   skinning matrix counts, object root-node matrix computation, and display
   brightness. The editor/test builds are not byte-compatible with the MCC DLL,
   so their code is semantic evidence, not an additional AOB-count target.

Locations below are RVAs for these exact hashed modules. They are evidence
locations only and must not be copied into runtime code as hardcoded offsets.

## Eight byte-identical results

| Signature | Bytes | ODST matches | ODST match RVA | ODST containing function | Semantic result |
|---|---:|---:|---:|---:|---|
| `kPrepareViewSig` | 29 | 1 | `0x1B4694` | `0x1B4694-0x1B4708` | Equivalent per-view preparation/dispatch |
| `kBuildViewportSig` | 25 | 1 | `0x2CAC5C` | `0x2CAC5C-0x2CAED0` | Equivalent viewport/frustum derivation |
| `kBuildMatricesSig` | 30 | 1 | `0x2CB1C4` | `0x2CB1C4-0x2CB5BD` | Equivalent camera/view/projection matrix builder |
| `kComposeBonesSig` | 28 | 1 | `0x260D8C` | `0x260D8C-0x260E35` | Equivalent ordinary hierarchy bone composer |
| `kComposeSpecialBonesSig` | 28 | 1 | `0x260E38` | `0x260E38-0x26112A` | Equivalent special-bone hierarchy composer |
| `kFpVisiblePaletteSig` | 40 | 1 | `0x2EDD10` | `0x2EDD10-0x2EDDEB` | Equivalent final first-person skin-palette mapper |
| `kHudXformSig` | 28 | 1 | `0x2A6308` | `0x2A6308-0x2A63BF` | Equivalent screen color/gamma constant uploader |
| `kFpRootCallSig` | 24 | 1 | `0x37CEA0` | `0x37CBB4-0x37CF32` | Equivalent root-fetch callsite in object-node recomposition |

All eight also match Halo 3 exactly once. The Halo 3 RVAs are recorded per
entry to make the comparison reproducible.

## Per-signature evidence

### `kPrepareViewSig`

```text
48 89 5C 24 08 57 48 83 EC 20 83 3D ?? ?? ?? ?? 03 8B FA 48 8B D9 48 89 0D ?? ?? ?? ??
```

- Halo 3: one match at `0x1854C8`, function `0x1854C8-0x18553C`.
- ODST: one match at `0x1B4694`, function `0x1B4694-0x1B4708`.
- Both functions are 116 bytes / 31 instructions with the same seven-call
  sequence, including the same three view-object virtual calls.
- Normalized full-function similarity is 96.77%. The only retained semantic
  difference is a view field read at `+0x27FC` in ODST versus `+0x27F4` in Halo
  3. This is positive evidence of the same operation over a shifted ODST view
  layout, not permission to reuse either field offset.
- H3ODSTEK independently retains the ODST render-view source identity and
  checks for both render-camera and rasterizer-camera position/forward/up
  members.

Verdict: unique and semantically equivalent as the per-view preparation and
dispatch function.

### `kBuildViewportSig`

```text
40 53 48 83 EC 30 44 0F BF 49 62 4C 8B D9 4C 8B 41 38 48 8B DA 0F BF 51 50
```

- Halo 3: one match at `0x2A63E4`, function `0x2A63E4-0x2A6658`.
- ODST: one match at `0x2CAC5C`, function `0x2CAC5C-0x2CAED0`.
- Both functions are 628 bytes / 154 instructions. Their complete normalized
  instruction sequences are identical, including every non-RIP field
  displacement and constant; neither function makes an external call.
- H3ODSTEK's ODST render code retains explicit viewport state and render-view
  camera semantics, including viewport bounds and render-target viewport
  behavior.

Verdict: unique and semantically equivalent as the viewport/frustum derivation
helper.

### `kBuildMatricesSig`

```text
48 8B C4 48 89 58 08 48 89 78 10 55 48 8D 68 E8 48 81 EC 10 01 00 00 80 3D ?? ?? ?? ?? 00
```

- Halo 3: one match at `0x2A6980`, function `0x2A6980-0x2A6D79`.
- ODST: one match at `0x2CB1C4`, function `0x2CB1C4-0x2CB5BD`.
- Both functions are 1,017 bytes / 219 instructions with seven calls. Their
  complete normalized instruction sequences are identical, preserving all
  camera-field displacements and matrix constants.
- H3ODSTEK identifies the ODST camera implementation with
  `render/render_cameras.cpp` and separately retains render-camera and
  rasterizer-camera basis checks.

Verdict: unique and semantically equivalent as the derived camera/view/
projection matrix builder.

### `kComposeBonesSig`

```text
45 85 C0 0F 8E ?? ?? ?? ?? 48 89 5C 24 08 57 48 83 EC 20 45 8B D0 49 8B F9 4C 8B C9
```

- Halo 3: one match at `0x23200C`, function `0x23200C-0x2320B5`.
- ODST: one match at `0x260D8C`, function `0x260D8C-0x260E35`.
- Both functions are 169 bytes / 49 instructions with two calls. Their complete
  normalized instruction sequences are identical.
- The function retains the same count guard, source/default inputs, output
  stride, hierarchy walk, and call structure used to compose an ordinary range
  of model nodes.
- H3ODSTEK independently retains ODST checks for base-node matrix counts and
  expected total matrix counts.

Verdict: unique and semantically equivalent as the ordinary hierarchy bone
composer.

### `kComposeSpecialBonesSig`

```text
48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 55 41 56 48 8D 68 B8
```

- Halo 3: one match at `0x2320B8`, function `0x2320B8-0x2323AA`.
- ODST: one match at `0x260E38`, function `0x260E38-0x26112A`.
- Both functions are 754 bytes / 171 instructions with six calls. Their
  complete normalized instruction sequences are identical, including the
  special-node indices, matrix strides, and hierarchy operations.
- H3ODSTEK's ODST bone/matrix assertions distinguish base matrices, total
  matrices, and node mappings, supporting the same specialized continuation of
  the ordinary composer.

Verdict: unique and semantically equivalent as the special-bone hierarchy
composer.

### `kFpVisiblePaletteSig`

```text
48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 49 8B F0 0F B7 C9 4C 8B F2
```

- Halo 3: one match at `0x2C561C`, function `0x2C561C-0x2C56F7`.
- ODST: one match at `0x2EDD10`, function `0x2EDD10-0x2EDDEB`.
- Both functions are 219 bytes / 64 instructions with one call. Normalized
  full-function similarity is 98.44%.
- The sole instruction difference is the maximum node count: ODST clamps the
  tag-provided count to `0x96` (150), while Halo 3 clamps it to `0x40` (64).
  The mapping loop, `0x34`-byte matrix stride, optional root-transform path,
  direct-copy path, destination advancement, and termination are otherwise
  identical.
- The called matrix-transform functions are each 893 bytes / 168 instructions
  and are completely identical after relocation normalization.
- H3ODSTEK independently retains ODST skinning assertions for node matrices,
  skinning matrix counts, node maps, and model-node limits.

Verdict: unique and semantically equivalent as the final first-person visible
skin-palette mapper. The `0x96` limit is ODST evidence and must not be replaced
with Halo 3's `0x40` assumption.

### `kHudXformSig`

```text
40 55 48 8B EC 48 83 EC 50 0F 29 74 24 40 0F 28 F1 0F 29 7C 24 30 0F 28 CA 0F 28 F8
```

- Halo 3: one match at `0x278EE0`, function `0x278EE0-0x278FB3`.
- ODST: one match at `0x2A6308`, function `0x2A6308-0x2A63BF`.
- The ODST function retains the same input shuffle and scalar calculation, then
  uploads the same two 16-byte float vectors to the same four constant IDs:
  `0x280000`, `0x2D0000`, `0x280001`, and `0x2D0001`. The second pair still
  contains the product of the first two float inputs.
- ODST is 183 bytes / 40 instructions versus Halo 3's 211 bytes / 46
  instructions. The only removed block is Halo 3's final, separate eight-byte
  upload to constant `0x082F0005`; none of the shared color/gamma calculation or
  four shared uploads changed. Normalized similarity is 93.02%.
- H3ODSTEK independently exposes ODST display-brightness settings and describes
  screen-brightness/exposure behavior. This agrees with the headset-proven Halo
  3 purpose and with the identical ODST constant dataflow; it does not support
  the retired HUD-geometry interpretation.

Verdict: unique and semantically equivalent as the screen color/gamma constant
uploader used by the game-brightness control. It is not a HUD-size transform.

### `kFpRootCallSig`

```text
48 8D 54 24 20 8B CB E8 ?? ?? ?? ?? 4D 8B C4 48 8D 4C 24 20 49 8B D7 E8
```

- Halo 3: one match at `0x341A54`, inside function
  `0x341768-0x341AE6`.
- ODST: one match at `0x37CEA0`, inside function
  `0x37CBB4-0x37CF32`.
- Both containing functions are 894 bytes / 231 instructions with six calls.
  Normalized full-function similarity is 96.97%. All seven differences are
  layout constants: `0x38 -> 0x20`, `0x138 -> 0x12C`, `0x134 -> 0x128`,
  `0x132 -> 0x126`, and `0x136 -> 0x12A` (with repeated constants counted in
  the seven changed instructions). Control flow and matrix work are unchanged.
- At the signature, both functions pass a node index and stack `BoneMatrix` to
  a root-fetch helper, then immediately pass that result, the source node, and
  the destination to the same matrix-composition operation before iterating
  remaining nodes.
- The root-fetch callees are both 162 bytes / 39 instructions and differ only
  in two matching ODST layout constants (`0x38 -> 0x20` and
  `0x138 -> 0x12C`). The following matrix-composition callees are both 999
  bytes / 208 instructions and are completely identical after relocation
  normalization.
- H3ODSTEK independently retains ODST diagnostics for object root-node matrix
  computation and root-node matrix disagreement.

Verdict: unique and semantically equivalent as the root-fetch callsite inside
object-node recomposition. The shifted ODST record fields remain unvalidated
for runtime consumption and must not be inferred from this callsite alone.

## Eight-signature gate decision

The eight byte-identical patterns pass this static evidence stage: each has one
loaded-image match in the installed ODST module and lands in a function
semantically equivalent to its Halo 3 target. No signature in this set is
ambiguous.

This result does **not** authorize installing any ODST hook. The next section
performs the independent derivation and uniqueness checks for the twelve failed
production roles. Every consumed ODST camera/view/first-person/HUD/bone/marker
layout still needs title-specific proof under the established bring-up order.

## Twelve re-derived ODST candidates

The second static pass covers the twelve production Halo 3 patterns that have
zero matches in the installed ODST module. Names below identify the existing
production roles; the candidate byte strings are evidence and have not been
added to `game.cpp`.

Each original production pattern still has exactly one Halo 3 match and zero
ODST matches. Each candidate below has exactly one ODST loaded-image match and
zero Halo 3 matches. This is intentional title isolation: a future adapter must
select a title-specific pattern rather than weakening the proven Halo 3 AOB.

Function similarity uses the same relocation-normalized comparison described
above. A lower percentage can reflect different register allocation, stack
layout, or inlining; acceptance also requires matching control/data flow,
call relationships, and ODST editing-kit semantics.

| Production role | Halo 3 target RVA | ODST candidate RVA | ODST containing function | Full-function comparison | Purpose |
|---|---:|---:|---:|---:|---|
| `kCamCopySig` | `0x2A628C` | `0x2CAAF0` | `0x2CAAF0-0x2CAC59` | 75/79 instructions; 87.01% | Compact render-camera copy/normalization |
| `kRenderViewSig` | `0x286A14` | `0x2AFB10` | `0x2AFB10-0x2AFD59` | 131/140; 67.90% | Inner prepared-view renderer |
| `kFpCameraRebuildSig` | `0x279BEC` | `0x2A6F5C` | `0x2A6F5C-0x2A714B` | 79/120; 51.26% | First-person camera rebuild |
| `kFpCameraUploadSig` | `0x2770F0` | `0x2A45DC` | `0x2A45DC-0x2A4744` | 121/84; 71.22% | First-person camera constant upload |
| `kFpDriverSig` | `0x2835D4` | `0x2AC2E4` | `0x2AC2E4-0x2AC729` | 213/228; 58.50% | First-person render driver |
| `kFpDriverGuardSig` | `0x28599D` | `0x2AE8DE` | `0x2AE13C-0x2AEDF0` | 497/707; 42.86% | Guarded first-person driver callsite |
| `kFpInterpolateSig` | `0x184B08` | `0x1B3CB8` | `0x1B3CB8-0x1B3E74` | 101/103; 78.43% | First-person animation interpolation |
| `kGunCamRefSig` | `0x68BC` | `0x6B60` | `0x6B60-0x6B96` | 14/14; 92.86% | Four-slot gun/overlay camera-array constructor |
| `kNativePauseOwnerSig` | `0xB682` | `0xBCA5` | `0xBA78-0xC603` | 684/725; 88.57% | Native pause-transition flag owner |
| `kHudElemSig` | `0x2EDF24` | `0x329488` | `0x329488-0x32954A` | 56/53; 86.24% | CHUD widget draw/class dispatch |
| `kSwayCallSig` | `0x2C484B` | `0x2ECF16` | `0x2EC1B0-0x2ED053` | 820/839; 67.27% | Camera-control exclusion in FP bone transform loop |
| `kFpNativeWeaponIkDecisionSig` | `0x2C393C` | `0x2EBFBC` | `0x2EB2B4-0x2EC104` | 752/800; 65.21% | Native first-person weapon-IK mode decision |

### `kCamCopySig` ODST candidate

```text
48 89 5C 24 08 57 48 83 EC 30 0F 29 74 24 20 48 8B FA 48 8B D9 48 85 D2 0F 84 ?? ?? ?? ?? F3 0F 10 15 ?? ?? ?? ?? B1 01 F3 0F 59 15 ?? ?? ?? ?? F3 0F 5E 15 ?? ?? ?? ??
```

- One ODST match at function entry `0x2CAAF0`; zero Halo 3 matches.
- The ODST function retains the destination/source convention, null-source
  path, camera scalar/vector copies, normalization calls, and the same final
  writes at destination `+0x24`, `+0x28`, `+0x2C`, `+0x64`, `+0x68`, and
  `+0x7C`.
- It ends three bytes before the already verified ODST viewport builder at
  `0x2CAC5C`, exactly mirroring the Halo 3 adjacency of `0x2A628C-0x2A63E1`
  and `0x2A63E4`.
- The original pattern failed because ODST uses a near conditional branch and
  computes its initial scalar in `xmm2` with extra multiply/divide operations;
  this is the recompiled function described in `CURRENT-STATE.md`.
- H3ODSTEK's ODST `render/render_cameras.cpp` evidence and camera bounds/basis
  checks agree with this compact camera preparation role.

Verdict: unique and semantically equivalent.

### `kRenderViewSig` ODST candidate

```text
48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 54 41 56 41 57 48 83 EC 40 8B 3D ?? ?? ?? ?? 48 8B F1 85 FF 0F 84 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? BA 00 00 00 00 0F BA E0 0A
```

- One ODST match at function entry `0x2AFB10`; zero Halo 3 matches.
- Both functions retain the prepared-view pointer in `rsi`, test the same
  render-loop global, make the same 30-call render-state sequence, and restore
  the paired render-state values in the same order before returning.
- The corresponding ODST callers and callees cluster with the verified ODST
  prepare-view and camera-builder functions. H3ODSTEK independently retains
  ODST render-view source identity and render/rasterizer camera access checks.

Verdict: unique and semantically equivalent as the inner prepared-view
renderer.

### `kFpCameraRebuildSig` ODST candidate

```text
48 8B C4 48 89 58 10 48 89 70 18 57 48 83 EC 60 48 8D 79 08 0F 29 70 E8 F3 0F 10 35 ?? ?? ?? ?? 48 8B D9 0F 29 78 D8 40 8A F2 48 8B 81 A8 02 00 00 B9 80 00 00 00 41 BA 58 01 00 00
```

- One ODST match at function entry `0x2A6F5C`; zero Halo 3 matches.
- Both versions read the compact camera from `view+0x2A8`, rebuild the camera
  at `view+8`, call their title's verified viewport and matrix builders, derive
  the block at `view+0x1E8`, and tail-jump to their title's camera uploader.
- The ODST call chain is explicit: `0x2CAC5C` (verified viewport), `0x2CB1C4`
  (verified matrices), then tail jump `0x2A45DC` (candidate below).
- H3ODSTEK retains distinct first-person camera and HUD-camera-view source
  identities, supporting the first-person overlay-camera role.

Verdict: unique and semantically equivalent. This does not validate the
referenced view offsets for runtime use.

### `kFpCameraUploadSig` ODST candidate

```text
48 8B C4 48 89 58 08 55 48 8D 68 A1 48 81 EC C0 00 00 00 0F 29 70 E8 4C 8D 45 F7 0F 29 78 D8 48 8B D9 48 8B C2 48 83 C2 78 48 8B C8 E8 ?? ?? ?? ?? 48 8D 55 F7
```

- One ODST match at function entry `0x2A45DC`; zero Halo 3 matches.
- The ODST first-person camera rebuild tail-jumps directly here after rebuilding
  its compact and derived camera blocks, proving the consumer relationship.
- Both functions transform the supplied camera blocks into stack constants and
  repeatedly call the same title-local shader-constant upload helper. ODST
  emits fewer upload groups (eight direct calls versus Halo 3's twelve), but
  retains the same input role and terminal constant-upload behavior.
- H3ODSTEK's render-camera, HUD-camera, and first-person-camera evidence agrees
  with this constant-uploader placement.

Verdict: unique and semantically equivalent as the ODST first-person camera
constant uploader; the different upload set must be treated as ODST-specific.

### `kFpDriverSig` ODST candidate

```text
48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B D9 40 8A F2 8B 89 FC 27 00 00 E8 ?? ?? ?? ?? 66 83 F8 FF 0F 85 ?? ?? ?? ?? B9 03 00 00 00
```

- One ODST match at function entry `0x2AC2E4`; zero Halo 3 matches.
- It retains the `(view, flag)` convention and the same high-level first-person
  render sequence. It calls the ODST camera rebuild at `0x2A6F5C` from the
  corresponding overlay paths and is called by the guard candidate below.
- The initial view field is `+0x27FC` in ODST versus `+0x27F4` in Halo 3,
  consistent with the independently observed shifted ODST view layout.
- H3ODSTEK identifies `interface/first_person_weapons.cpp` and retains user,
  weapon-slot, model-node, overlay-animation, and camera-slaved-to-gun checks.

Verdict: unique and semantically equivalent as the first-person render driver.
The shifted field remains evidence, not an approved runtime offset.

### `kFpDriverGuardSig` ODST candidate

```text
39 35 ?? ?? ?? ?? 75 ?? 33 D2 48 8B CF E8 ?? ?? ?? ?? 40 38 35 ?? ?? ?? ?? 75 ?? 40 38 35 ?? ?? ?? ??
```

- One ODST match at `0x2AE8DE`, inside function `0x2AE13C-0x2AEDF0`; zero Halo
  3 matches.
- The site compares a RIP-relative guard against the function's zero register,
  then calls the proven ODST driver candidate with `rdx=0` and the prepared
  view in `rcx`. This is the same guard/call order as Halo 3 at `0x28599D`.
- The called target resolves exactly to `0x2AC2E4`, providing call-graph proof
  independent of prologue similarity.

Verdict: unique and semantically equivalent as the first-person driver guard
site. The RIP target is not yet approved as a runtime state offset.

### `kFpInterpolateSig` ODST candidate

```text
48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 30 33 DB 49 63 E8 38 1D ?? ?? ?? ?? 4D 8B E1 44 8B FA
```

- One ODST match at function entry `0x1B3CB8`; zero Halo 3 matches.
- ODST preserves the argument registers, index conversion, two-call
  interpolation flow, success/out-index write, and boolean return. It has 103
  instructions versus Halo 3's 101; the prologue stack allocation changed
  from `0x20` to `0x30`.
- H3ODSTEK retains the ODST first-person animation source, mode checks, model
  node limits, overlay animation checks, and animation-rig diagnostics.

Verdict: unique and semantically equivalent as the first-person animation
interpolation boundary.

### `kGunCamRefSig` ODST candidate

```text
48 89 5C 24 08 57 48 83 EC 20 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 81 C3 10 28 00 00 48 83 EF 01 75 ?? 48 8B 5C 24 30
```

- One ODST match at function entry `0x6B60`; zero Halo 3 matches.
- It resolves the array at ODST RVA `0x2D73590`, constructs exactly four
  objects, and advances by `0x2810` per object. This is the measured ODST
  gun/overlay camera-object stride.
- The prefix without the stride also reaches a second ODST builder at `0x7434`,
  but that function advances by `0x1460` and resolves a different array at
  `0x2D92DD0`. Keeping `10 28 00 00` in the candidate removes the ambiguity.
- Halo 3's equivalent constructs four objects at RVA `0x2D2F680` with the
  known `0x2820` stride. H3ODSTEK independently retains HUD-camera view and
  first-person-camera-slaved-to-gun semantics.

Verdict: unique and semantically equivalent as the four-slot gun/overlay camera
array constructor.

### `kNativePauseOwnerSig` ODST candidate

```text
E8 ?? ?? ?? ?? 84 C0 74 ?? B9 03 00 00 00 E8 ?? ?? ?? ?? 84 C0 75 ?? 8B D1 B1 01 E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01
```

- One ODST match at `0xBCA5`, inside function `0xBA78-0xC603`; zero Halo 3
  matches.
- Both containing functions implement the same event/state dispatcher and the
  same three predicate/call sequence before the native flag write. Their full
  normalized similarity is 88.57%.
- Halo 3 loads `esi=1` at function entry and uses `sil` for the final call and
  byte write. ODST compiles those same values as literal `1`; the dataflow is
  equivalent, not a guessed value.
- H3ODSTEK exposes ODST pause-related engine state, but the similarly named
  HaloScript `game_paused` external remains insufficient runtime proof. The
  owner-code path, not that external, is the evidence here.

Verdict: unique and semantically equivalent as the native pause flag owner.
The RIP target remains unapproved until ODST live transition evidence exists.

### `kHudElemSig` ODST candidate

```text
44 88 4C 24 20 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 8B D9 45 0F B7 C0 89 4C 24 20 48 8B 0D ?? ?? ?? ?? 48 89 54 24 28 46 8B 4C C0 04 45 85 C9 75 ?? 33 C0 EB ??
```

- One ODST match at function entry `0x329488`; zero Halo 3 matches.
- ODST retains the same widget/tag lookup, descriptor indexing, five-call draw
  dispatch, and final two-way widget draw path.
- The native class gate is structurally identical: playback predicate, compare
  scripting class against `2`, call the class-gated visibility predicate, then
  continue to the normal draw path. In ODST the class compare is at
  `0x32950B`; no runtime tag-table interpretation was inferred.
- H3ODSTEK independently identifies `interface/chud/chud_draw.cpp`, defines the
  CHUD scripting-class enum, and retains the scripting-class bounds check.

Verdict: unique and semantically equivalent as the CHUD widget draw/class
dispatch function. No patch location or hook is authorized by this finding.

### `kSwayCallSig` ODST candidate

```text
44 3B 8F 28 27 00 00 74 ?? 49 8B D0 48 8D 4D D8 E8 ?? ?? ?? ?? B8 01 00 00 00 44 03 C8 49 83 C0 34 45 3B CA 7C ??
```

- One ODST match at `0x2ECF16`, inside function `0x2EC1B0-0x2ED053`; zero Halo
  3 matches.
- Both sites are the exclusion test in a `0x34`-stride first-person bone loop.
  Non-excluded nodes are passed to the same matrix-transform operation used by
  the visible-palette path; ODST resolves that verified callee at `0x1410A0`.
- ODST uses exclusion field `+0x2728`, local matrix `[rbp-0x28]`, and an
  explicit increment value versus Halo 3's `+0x11A4`, `[rbp-0x38]`, and direct
  increment. The loop purpose and transform call are preserved, but none of
  those shifted fields is approved for runtime use.
- H3ODSTEK retains ODST first-person model node limits, camera-control and
  first-person weapon animation semantics.

Verdict: unique and semantically equivalent as the camera-control exclusion
callsite in the first-person bone transform loop.

### `kFpNativeWeaponIkDecisionSig` ODST candidate

```text
40 84 ED 74 ?? 45 84 FF 75 ?? 84 DB 74 ?? BA 03 00 00 00 41 0F 28 D9 44 8D 42 FF EB ?? F3 0F 10 1D ?? ?? ?? ??
```

- One ODST match at `0x2EBFBC`, inside function `0x2EB2B4-0x2EC104`; zero Halo
  3 matches.
- The local decision tree is instruction-for-instruction equivalent: the same
  three boolean tests select mode `3`, copy the active blend vector, derive the
  alternate mode, and join the same continuation.
- The byte-identical prefix failed only because ODST keeps the blend vector in
  `xmm9` (`41 0F 28 D9`) while Halo 3 uses `xmm8`
  (`41 0F 28 D8`).
- H3ODSTEK independently exposes native `weapon_ik`, disable/force weapon-IK
  controls, first-person weapon source identity, and weapon-slot/node limits.

Verdict at the time of this broad signature pass: unique and semantically
equivalent, but not yet sufficient by itself to authorize a runtime patch.
The later title-specific H3ODSTEK graph comparison, exact retail-byte
verification, reversible lifecycle design, and current authorization are
recorded in `ODST-WEAPON-IK-EVIDENCE.md`.

## Halo 3 comfort-parity addendum

### Post-observer camera effect

The production Halo 3 `kObserverCameraEffectSig` is 60 bytes long with only
three RIP-relative data displacements wildcarded. Offline scanning of the exact
retail images found one match in each module:

| Module | Match count | RVA |
|---|---:|---:|
| `halo3.dll` | 1 | `0x17DF44` |
| `halo3odst.dll` | 1 | `0x1ACAF0` |

This is the already headset-confirmed Halo 3 `observer_apply_camera_effect`
boundary. The matching ODST entry has the same prologue and camera-effect data
setup, so the private ODST path may add it as a required, unique, in-`.text`
fifth hook. Missing/ambiguous resolution or hook creation fails the entire
ODST transaction; ODST owns a separate original-function trampoline and never
reuses Halo 3's global recoil-hook state.

### ODST native motion-blur controls

ODST does not contain Halo 3's split `motion_blur_scale_x/y` and
`motion_blur_max_x/y` names. It contains its own unique type-6 float debug-table
entries:

| Name | Name RVA | Table RVA | Value RVA | Retail value |
|---|---:|---:|---:|---:|
| `motion_blur_scale` | `0x811DA8` | `0x8E7400` | `0x8F0ED4` | `0.35` |
| `motion_blur_max` | `0x811C80` | `0x8E7418` | `0x8F0ED8` | `0.08` |

Each name and table reference occurs once in the exact retail module. Runtime
code resolves the slots by name through the existing debug-table resolver and
bounds both pointers inside the identity-checked ODST image; the evidence RVAs
are never hardcoded. Both values are zeroed each monitored camera frame while
motion blur is disabled and restored only after all ODST detours reach verified
quiescence. The superficially related `motion_blur_enabled` entry is type 0 and
does not point to a float value slot, so it is deliberately rejected.

## Twelve-signature gate decision

All twelve formerly failing production roles now have a unique, title-specific
ODST candidate in the installed module, and each candidate lands in the
semantic counterpart of its Halo 3 target. The gun-camera lookalike was resolved
by the independently established `0x2810` stride; no candidate remains
ambiguous.

These candidates remained documentation-only during derivation. The subsequent
camera/view layout gate is now complete in `ODST-CAMERA-LAYOUT.md`, including
read-only stock captures through zoom, death/respawn, cutscene, unload/reload,
and vehicle transitions. That follow-on gate authorizes only the private minimal
camera/stereo/6DOF implementation described in
`ODST-MINIMAL-BRINGUP-HANDOFF.md`; it does not authorize broad ODST hooks,
gameplay patches, or a public support-gate change.
