# ODST camera/view layout evidence manifest

> **Status superseded:** ODST runtime support later passed the recorded headset
> gates and shipped in public release 0.2.1. No-authorization language below is
> historical; `docs/CURRENT-STATE.md` controls current product status.

Date: 2026-07-21
Status: **evidence gate closed; no ODST runtime enablement is authorized**

This manifest records the retail ODST camera and prepared-view layouts that are
required before any ODST camera, stereo, or 6DOF work begins. It is deliberately
separate from signature discovery: a unique function address does not by itself
prove the structure consumed by that function.

No game memory was written, no hook was installed, and no runtime source was
changed while producing this manifest. Static analysis was completed first;
fields it could not name conclusively were then checked with the narrowly scoped
read-only stock captures described below rather than inferred from Halo 3.

## Evidence identity and terminology

Retail module:

- Path: `Halo The Master Chief Collection/halo3odst/halo3odst.dll`
- SHA-256: `5BB20976EFDFD9E1CE59C589339804725FEC239021027C8D65B2733EAB94829A`
- All retail addresses below are module RVAs.
- H3ODSTEK corroboration is from the locally installed `sapien.exe`; its retained
  assertion text is used only when the accessed offset and surrounding formula
  match the retail implementation.

Evidence states:

- **Proven (static):** retail reads/writes establish size and purpose, sometimes
  corroborated by an H3ODSTEK member-name assertion.
- **Purpose proven / name open:** behavior is established, but the engine's exact
  member name is not recoverable from current static evidence.
- **Open:** neither an exact semantic purpose nor safe equivalence is proven. A
  read-only live capture is required before the gate can pass.

`compact camera` below is the `0x90`-byte block copied by `kCamCopy` and consumed
by the verified viewport/matrix builders. `prepared view` is the enclosing
`0x2810`-byte ODST object used by `kPrepareView` and `kFpDriver`.

## Function evidence anchors

| Role | ODST RVA | Evidence used here |
|---|---:|---|
| compact-camera initialization | `0x2CAA98` | Initializes the rectangles and display dimensions before `kCamCopy`. |
| `kCamCopySig` / compact-camera copy | `0x2CAAF0`-`0x2CAC58` | Maps observer-camera inputs into the compact block and sets near/far/default flags. |
| verified viewport builder | `0x2CAC5C`-`0x2CAECF` | Exact Halo 3-equivalent code; consumes rectangle, display, and optional projection-window fields. |
| projection helper | `0x2CAED0` | Consumes bounds, dimensions, and vertical FOV. |
| verified matrix builder | `0x2CB1C4`-`0x2CB5BD` | Exact Halo 3-equivalent code; consumes pose/depth/optional plane fields and writes through derived offset `+0xBC`. |
| prepared-view construction | `0x2ABAC8` | Builds both camera/derived pairs and initializes the ODST tail fields. |
| `kPrepareView` | `0x1B4694` | Reads the prepared-view user index at `+0x27FC`. |
| first-person camera upload | `0x2A45DC` | Consumes compact pose and the derived matrix block. |
| `kFpCameraRebuild` | `0x2A6F5C`-`0x2A714B` | Copies exactly `0x90` compact bytes and rebuilds viewport/matrices into a `0xC0` derived block. |
| `kFpDriver` | `0x2AC2E4`-`0x2AC729` | Establishes the nested first-person prepared view at root `+0x6C8`. |
| gun-camera array construction | `0x00006B60` | Constructs four objects and advances by `0x2810`. |
| prepared-view loop/call site | `0x1B4708`-`0x1B4F2A` | Independently advances the constructed view pointer by `0x2810`. |

The signature and normalized-function comparisons supporting these function
identities are recorded in `ODST-SIGNATURE-EVIDENCE.md`.

## Compact camera: complete byte coverage (`0x00`-`0x8F`)

| Offset | Size | Purpose | Static evidence | Live evidence | Ambiguity status |
|---:|---:|---|---|---|---|
| `+0x00` | `0x0C` | world position, three floats | `kCamCopy` copies source `+0x00..+0x0B`; matrix builder subtracts this position from view-space products. | Not required. | **Proven (static).** |
| `+0x0C` | `0x0C` | forward vector, three floats | `kCamCopy` copies source `+0x28..+0x33`; matrix/upload code consumes it as a basis vector. | Not required. | **Proven (static).** |
| `+0x18` | `0x0C` | up vector, three floats | `kCamCopy` copies source `+0x34..+0x3F`; matrix/upload code consumes it as a basis vector. | Not required. | **Proven (static).** |
| `+0x24` | `0x01` | camera mode flag that must be clear for the ordinary projection path | `kCamCopy` always writes zero at `0x2CAC2C`. H3ODSTEK checks this byte before emitting `mirrored cameras do not work`. | Zero in first person, zoom, death/respawn, level unload/reload, and a stock cutscene. | **Purpose proven / exact member name open.** The evidence supports a mirrored-camera flag, but this must not yet be encoded as a retail ODST type name. |
| `+0x25` | `0x03` | alignment/padding or presently unobserved flags | No verified builder reads these bytes; whole-block copies preserve them. | All three bytes stayed zero in first person, zoom, death/respawn, and cutscene captures. | **Live-corroborated padding; alternate projection modes still open.** |
| `+0x28` | `0x04` | vertical field of view, radians | `kCamCopy` maps source `+0x68`, applies the camera FOV multiplier, and stores here. The projection helper consumes it as the projection angle. Matching H3ODSTEK assertions name `camera->vertical_field_of_view` and constrain it to `(epsilon, pi)`. | Not required. | **Proven (static).** |
| `+0x2C` | `0x04` | zoom/reference FOV used to normalize first-person FOV | `kCamCopy` maps source `+0x6C`, applies the same multiplier as `+0x28`, and defaults it to `1.0`. `kFpCameraRebuild` divides a global FOV factor by this value before rescaling `+0x28`. | Normal `0.874714`; zoom `0.437357`; death/cutscene values tracked `+0x28`. Despite root-camera changes, the rebuilt FP FOV stayed near `0.883854`. | **Purpose proven; exact member name open.** |
| `+0x30` | `0x04` | first-person camera blend/eligibility weight | First half of the source `+0x5C/+0x60` copy at `0x2CAB82`; no verified viewport/matrix instruction consumes it. | `1.0` on foot, `0.0` in cutscene/vehicle cameras, and smoothly interpolated at death/respawn and both vehicle entry/exit transitions. | **Purpose proven; exact member name/downstream consumer open.** |
| `+0x34` | `0x04` | vertical projection/observer aim-offset scalar | Second half of the source `+0x5C/+0x60` copy at `0x2CAB82`. H3ODSTEK observer normalization at `0x4F08E1` evaluates `atan(tan(vertical_fov/2) * source+0x60 * scale)` and uses that angle to rotate the observer basis before `kCamCopy`. | Stayed `0.0` through first person, zoom, death/respawn, unload/reload, cutscene, and vehicle entry/exit. | **Purpose proven; exact member name open.** |
| `+0x38` | `0x08` | `window_pixel_bounds`, four signed 16-bit coordinates | Initialized by view construction and consumed by both verified builders. Matching H3ODSTEK assertions name `camera->window_pixel_bounds` and enforce `x0<x1`, `y0<y1`. | Not required. | **Proven (static).** |
| `+0x40` | `0x08` | window title-safe pixel bounds, four signed 16-bit coordinates | View construction copies `+0x54`, clamps it to the window, and the viewport builder consumes both rectangles. | `[70,128,1346,2432]`, identical to `+0x54`, while full bounds were `[0,0,1417,2560]`. | **Purpose proven / exact member spelling open.** |
| `+0x48` | `0x02` | saved original `x0` before bounds normalization | Construction saves old `+0x3A` here before normalizing the rectangle origin. ODST rectangles use `y0,x0,y1,x1` short order. | Zero for the captured full-window view. | **Proven (static purpose); exact member name open.** |
| `+0x4A` | `0x02` | saved original `y0` before bounds normalization | Construction saves old `+0x38` here before normalizing the rectangle origin. ODST rectangles use `y0,x0,y1,x1` short order. | Zero for the captured full-window view. | **Proven (static purpose); exact member name open.** |
| `+0x4C` | `0x08` | `render_pixel_bounds`, four signed 16-bit coordinates | Compact initialization creates this rectangle; the projection helper consumes it. Matching H3ODSTEK assertions name `camera->render_pixel_bounds` and enforce ordered x/y coordinates. | Not required. | **Proven (static).** |
| `+0x54` | `0x08` | render/title-safe pixel bounds | Compact initialization calls the title-safe rectangle initializer here and copies it to window field `+0x40`. | `[70,128,1346,2432]`, matching `+0x40` throughout all captured modes. | **Purpose proven / exact member spelling open.** |
| `+0x5C` | `0x08` | active raster/display bounds used by projection scaling | Compact initialization calls the second rectangle initializer here. The matrix builder consumes its four 16-bit components, and the viewport path uses its dimensions. | `[0,0,1417,2560]`, matching the full render/window bounds throughout the captures. | **Purpose proven / exact member name open; split-screen remains untested.** |
| `+0x64` | `0x04` | near clip distance (`z_near`) | `kCamCopy` initializes it from the stock `0.0078125f` global. Matrix construction uses it as a depth endpoint. Matching H3ODSTEK assertion names `camera->z_near`. | Not required. | **Proven (static).** |
| `+0x68` | `0x04` | far clip distance (`z_far`) | `kCamCopy` initializes it from the stock `10240.0f` global; matrix construction pairs it with `+0x64`. | Not required. | **Proven (static).** |
| `+0x6C` | `0x10` | optional oblique near-clip plane (`real_plane3d`) | The matrix builder passes this four-coefficient plane to `0x140E18` only when the selected near-depth endpoint compares equal to zero. Helpers `0x140E18/0x140D8C` transform its normal and distance into view/projection space. | Four zero floats in first person, zoom, death/respawn, unload/reload, cutscene, and vehicle cameras; the ordinary path uses nonzero `z_near`. | **Purpose and activation condition proven; exact member spelling open.** |
| `+0x7C` | `0x01` | custom projection-window enable | `kCamCopy` clears it. The verified viewport builder branches on it and consumes `+0x80..+0x8C` only when set. | Desirable in any mode that sets it. | **Proven (static purpose); exact member name open.** |
| `+0x7D` | `0x03` | alignment/padding or presently unobserved flags | Not read by either verified builder; whole-block copies preserve it. | All three bytes stayed zero in every captured mode; `+0x7C` and `+0x80..+0x8C` also stayed disabled/zero. | **Live-corroborated padding; custom projection mode still open.** |
| `+0x80` | `0x04` | custom projection horizontal center offset | Used by the viewport-builder centering formula when `+0x7C != 0`. | Desirable in a mode that enables custom projection. | **Purpose proven / name open.** |
| `+0x84` | `0x04` | custom projection vertical center offset | Used by the viewport-builder centering formula when `+0x7C != 0`. | Desirable in a mode that enables custom projection. | **Purpose proven / name open.** |
| `+0x88` | `0x04` | custom projection horizontal extent/scale | Used by the viewport-builder horizontal-extent formula when `+0x7C != 0`. | Desirable in a mode that enables custom projection. | **Purpose proven / name open.** |
| `+0x8C` | `0x04` | custom projection vertical extent/scale | Used by the viewport-builder vertical-extent formula when `+0x7C != 0`. | Desirable in a mode that enables custom projection. | **Purpose proven / name open.** |

The exact compact-camera size is not inferred from the last accessed member.
`kFpCameraRebuild` performs a complete `0x90`-byte copy from
`[prepared_view+0x2A8]` into `prepared_view+0x08`, in aligned chunks through
source `+0x80`. The table above covers every byte in that copied range.

## Prepared-view and first-person structures

### Front camera region

| Prepared-view offset | Size | Purpose | Static evidence | Ambiguity |
|---:|---:|---|---|---|
| `+0x000` | `0x08` | prepared-view vtable | Constructor and `kPrepareView` indirect calls. | Proven. |
| `+0x008` | `0x90` | current compact camera | Direct target of `kCamCopy`; passed to verified builders. | Proven. |
| `+0x098` | `0xC0` | current derived viewport/matrix block | Verified matrix builder writes through `+0xBC`; construction copies a full `0xC0` bytes. | Size and role proven; individual derived members are only partially named. |
| `+0x158` | `0x90` | secondary/prepared compact-camera copy | Construction copies `+0x008..+0x097` here; `kFpDriver` copies this whole block into its nested view. | Proven copy and use; temporal name remains generic. |
| `+0x1E8` | `0xC0` | secondary/first-person derived viewport/matrix block | Construction copies `+0x098..+0x157`; `kFpCameraRebuild` writes rebuilt matrices here. | Size and use proven; temporal name remains generic. |
| `+0x2A8` | `0x08` | source compact-camera pointer when this prepared view is passed to `kFpCameraRebuild` | `kFpCameraRebuild` dereferences it and copies exactly `0x90` bytes into `+0x008`. | Proven for the nested FP view. The active root object's own `+0x2A8` is not a valid camera pointer and the root is not the object passed to this rebuild. |

These sizes exactly tile the region: `0x008 + 0x90 = 0x098`,
`0x098 + 0xC0 = 0x158`, `0x158 + 0x90 = 0x1E8`, and
`0x1E8 + 0xC0 = 0x2A8`.

The derived block is therefore **`0xC0`, not `0x90`**. Its statically proven
regions include camera basis/position, projection/culling coefficients, a
`0x40`-byte projection matrix beginning at derived `+0x78`, and pixel-scale
terms at `+0xB8/+0xBC`. Exact names for every derived intermediate are outside
the minimum camera-layout gate and remain unasserted.

### Nested first-person prepared view

`kFpDriver` uses `root+0x6C8` as another prepared-view base:

| Root offset | Nested-relative offset | Size | Purpose and evidence |
|---:|---:|---:|---|
| `+0x6C8` | `+0x000` | enclosing object | Nested first-person prepared-view base passed to `kFpCameraRebuild`. |
| `+0x6D0` | `+0x008` | `0x90` | Nested current compact camera; copied from root `+0x008`. |
| `+0x760` | `+0x098` | `0xC0` | Nested current derived block by the proven front layout. |
| `+0x820` | `+0x158` | `0x90` | Nested secondary compact camera; `kFpDriver` copies root `+0x158` here. |
| `+0x8B0` | `+0x1E8` | `0xC0` | Nested rebuilt first-person derived block. |
| `+0x970` | `+0x2A8` | `0x08` | Nested source-camera pointer; `kFpDriver` explicitly stores `root+0x008` here. |

Live capture confirmed `root+0x970 == prepared_view_array+0x008` exactly while
slot 0 was active. Slots 1-3 retained their constructed vtables but their camera
blocks were zero. Root `+0x2A8` contained non-pointer state, confirming that the
source-pointer interpretation is contextual to the nested prepared view.

`kFpCameraRebuild` then:

1. Copies `0x90` bytes from the `+0x2A8` source pointer to nested `+0x008`.
2. Recomputes vertical FOV from compact `+0x28/+0x2C` and the first-person
   weapon/tag FOV factor.
3. Optionally adjusts compact `+0x64` (near clip) for the flagged pass.
4. Calls the verified viewport and matrix builders.
5. Writes the rebuilt result to nested `+0x1E8` and uploads it.

This proves the structures actually used by `kFpCameraRebuild` and `kFpDriver`;
it does not assume that unrelated Halo 3 tail fields survive in ODST.

## `0x2810` stride proof

The ODST prepared/gun-camera stride is **`0x2810` bytes** with two independent
retail proofs:

1. The constructor at RVA `0x00006B60` resolves the array at RVA `0x2D73590`,
   constructs four objects, and executes `add ..., 0x2810` between objects.
2. The live-view construction loop containing the call to RVA `0x2ABAC8`
   executes `add r15, 0x2810` between prepared views.

The object constructor at RVA `0x1B6260` initializes nested subobjects through
the tail, including vtables at `+0x6C8`, `+0x978`, `+0xC30`, `+0x1F50`,
`+0x2240`, and `+0x24E8`, and reaches state at `+0x27D8`. The last byte of one
object is therefore `+0x280F`; the next begins exactly `0x2810` bytes later.

The similar constructor that advances by `0x1460` is not the gun-camera array
and is rejected. Halo 3's `0x2820` stride must not be reused for ODST.

## The `view+0x27FC` shift

ODST `prepared_view+0x27FC` is the **render user index** supplied as the fifth
argument to the view constructor:

- RVA `0x2ABAC8` stores the fifth constructor argument at `+0x27FC`.
- `kPrepareView` reads that field and passes it into RVA `0x2A4340`.
- `kFpDriver` reads the same field and calls RVA `0x114780`.
- RVA `0x114780` preserves `-1` as the no-user sentinel; otherwise it indexes
  per-user TLS records (`0x230` stride) and reads the user's state at `+0xBFA`.

The comparable Halo 3 constructor stores the same semantic argument at
`+0x27F4`. The observed difference is real, but it is **not a uniform +8 shift**:

| Semantic tail field | Halo 3 offset | ODST offset |
|---|---:|---:|
| normalized viewport floats (four) | `+0x27D8..+0x27E4` | `+0x27E0..+0x27EC` |
| constructed view slot | `+0x27E8` | `+0x27F0` |
| view count/context | `+0x27EC` | `+0x27F4` |
| additional context | `+0x27F0` | `+0x27F8` |
| render user index | `+0x27F4` | `+0x27FC` |
| constructor result/reference | `+0x27F8` | `+0x2800` |
| table-derived field | `+0x27FC` | `+0x2804` |
| initialized zero | `+0x2800` | `+0x2808` |

ODST ends with a separately used boolean at `+0x280C` and a `0x2810` stride.
Halo 3 has different motion-tracking/tail state and a `0x2820` stride. Each ODST
tail field therefore requires semantic proof; offset translation is unsafe.

## Live-evidence queue and gate decision

Live evidence was captured on 2026-07-21 from stock ODST launched with EAC off.
`tools/odst_layout_probe.py` requested only
`PROCESS_QUERY_INFORMATION | PROCESS_VM_READ`; it did not attach a debugger,
request write access, change protection, load code, or install a hook. Captured
modes were playable first person, movement/look, zoom enter/exit, death and
respawn, clean level unload/reload, a stock cutscene, and vehicle entry/exit.

The live object map independently corroborated the `0x2810` stride: only array
slot 0 carried an active camera, each following constructed vtable was exactly
one stride apart, and the active tail at `+0x27F0..+0x280C` was
`[0,1,0,0,0,0,0,0]` (slot 0, count 1, context 0, user 0, remaining state zero).
Level unload zeroed the active camera blocks and nested source pointer cleanly.

The next stock capture must use only `PROCESS_QUERY_INFORMATION | PROCESS_VM_READ`
and must record complete `0x90` compact-camera blocks, the owning view address,
view index, user index, resolution/split-screen state, and the camera transition
being observed. No debug attach, breakpoint, memory write, DLL load, or hook is
permitted. The minimum correlation set is:

- at least one split-screen or changed-resolution sample for rectangles
  `+0x54` and `+0x5C`;
- any stock mode that sets `+0x7C`, to validate `+0x80..+0x8C` and observe the
  currently unobserved `+0x7D..+0x7F` bytes;
- a mode that takes the matrix builder's optional `+0x6C` path;
- alternate camera modes to confirm `+0x25..+0x27` remain padding and to name
  the `+0x24` mirrored/mode flag conclusively.

**Gate result: passed for the single-user stock camera path.** Every byte in the
`0x90` compact camera is now assigned a statically proven purpose or padding
role, with live correlations for pose, zoom/FOV normalization, first-person
blend, pixel/title-safe bounds, near/far depth, nested rebuilds, cutscene and
vehicle transitions, level unload, user index, and object stride. Remaining
uncertainty is limited to original engine member spelling and stock modes that
activate mirrored/custom/oblique projection; their field sizes, formulas,
enable conditions, and required stock-preservation behavior are statically
known.

This pass does **not** authorize broad ODST enablement. The next phase may attempt
only the minimal ODST camera/stereo/6DOF bring-up, must preserve unmodified opaque
and optional fields, must fall back atomically to the stock camera on any failed
validation, must retain the observed clean zeroed-camera level unload, and must
independently validate clean title exit.
No runtime source, game memory, hook, or game file was changed by this evidence
pass.
