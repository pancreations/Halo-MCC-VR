# Halo 3: ODST byte-identical signature evidence

Verified 2026-07-21 on `feature/odst-bringup` at `4dddbc1`.

This is an evidence manifest, not an ODST runtime-support manifest. It covers
only the eight production signatures that already match `halo3odst.dll`
byte-for-byte. It does not validate the twelve failed signatures, any consumed
structure layout, or any ODST hook. ODST must remain stock when selected:
`runtimeSupported=false`, with game-hook dispatch still restricted to
`GameTitle::Halo3`.

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

## Result summary

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

## Gate decision

The eight byte-identical patterns pass this static evidence stage: each has one
loaded-image match in the installed ODST module and lands in a function
semantically equivalent to its Halo 3 target. No signature in this set is
ambiguous.

This result does **not** authorize installing any ODST hook. Before runtime work,
the twelve failed production signatures still need independent derivation and
uniqueness checks, and every consumed ODST camera/view/first-person/HUD/bone/
marker layout still needs title-specific proof under the established bring-up
order.
