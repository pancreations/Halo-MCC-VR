# Reverse-engineering notes — halo3.dll camera (M1)

**Game build:** MCC `1.3528.0.0`, `halo3.dll` (~10.6 MB on disk, ~71 MB mapped image).
**All addresses below are RVAs (offset from halo3.dll's runtime base).** They are specific to
this build and WILL move on an MCC update — they must be turned into AOB signature scans before
shipping (see the `sig` module). They're recorded here so we don't have to re-derive them each
session.

## How these were found

Without Cheat Engine (user's AV blocks it), we built our own read-only external finder,
`tools/camscan.cpp`, plus `tools/disasm.py` (capstone). Workflow used:
`attach` → `first halo3` (baseline) → `narrow inc/dec/changed/unchanged` (user looks up/down on
cue) → `clusters` → `watch` (confirm it tracks the view) → `poke`/`spin` (write-test) →
`xref` / `findwrite` (hardware watchpoint) → `hex` + `disasm.py` (read the code).

## Findings

- **`halo3.dll+0xA84258` — a camera/observer struct.**
  - `+0x0C..+0x14`: position, roughly `(x≈176.9, y≈5.4, z≈-5.5)` on the test level.
  - `+0x18` (`0xA84270`): **forward unit vector** (i,j,k). Tracks the view perfectly when read.
  - **This forward is a computed READOUT, not an input:** writing/spinning it externally did
    nothing, and there are **no direct RIP-relative references** to it (the game reaches it via a
    pointer). So the render does not read this copy.

- **Writer of that forward:** `halo3.dll+0x2A86CD` `movdqu [rbx+0x18], xmm0` (rbx = struct base
  `0xA84258`). The enclosing function computes the forward into a stack local `[rdi]`, optionally
  negates it (hemisphere flip via a dot product), then copies it in. It reads engine globals
  `rdx/r10 = 0x2D2F680` and `r11 = 0xA67630`. (Disassembly around `0x2A8600–0x2A8720`.)

- **`halo3.dll+0x2D2F680` — a camera object** (vtable/ptr at `+0x00`):
  - `+0x08`: position `(≈-5.5, 176.9, 5.4)`
  - `+0x14` (`0x2D2F694`): **forward** unit vector
  - `+0x24` (`0x2D2F6A0`): **up** unit vector
  - `+0x30`: looks like projection/frustum params (`0.858, 0.874, 1.0`)
  - Spinning `+0x14` externally made **the first-person gun + HUD flash** (not the world) → this
    is (at least) the **weapon/overlay camera**, and external writes only flicker because they
    race the game's per-frame recompute.
  - It is the **first element of a 4-slot array** (0x2820 bytes per object). The builder function
    at `halo3.dll+0x68BC` does `lea rbx,[rip+disp] → 0x2D2F680; mov edi,4;` then loops the ctor
    advancing rbx by 0x2820. On-disk scan found 8 RIP-relative refs into `0x2D2F680..+0x40`
    (others at RVAs 0x152486/0x15249A/0x1524B0 read floats `+0x20/+0x24/+0x28`; 0x185745,
    0x24CA71, 0x24CC28, 0x2B7A12 reference the base). `kGunCamRefSig` in `game.cpp` matches the
    builder prologue uniquely (an otherwise identical builder for another array uses stride
    0x1630, so the stride bytes are part of the signature); the gun camera address is
    `match+17 + int32 at match+13`. In stereo, `RenderViewHook` rewrites `+0x30/+0x34` to the
    world-raster tangents (× user scale, Home/End) so the weapon/HUD aren't magnified ~2x by the
    widened world projection.

- **World render camera** lives around `~0x468xxxx` (position `≈176,5,-8`, orthonormal basis) but
  it's a **sliding array/ring** (render history for temporal effects), so no single fixed address
  is a stable write target there.

- **Projection values pass through the M1 camera-copy hook.** Live disassembly of the original
  function at `halo3.dll+0x2A628C` shows `src+0x68/+0x6C` copied (after a common scale factor) to
  compact render-camera `dst+0x28/+0x2C`. This is a much cleaner M2 entry point than chasing the
  sliding render-history array. A short startup diagnostic now logs the first 24 copies so we can
  identify whether this function receives world, weapon/HUD, or multiple camera passes.
  - Headset test confirmed a single stable destination and two alternating source buffers:
    `dst=...BEBEF688`, sources `...2544A4A8` / `...24F4A4A8`.
  - Both sources carried position `(-9.14, 175.69, 5.38)` and projection values
    `(1.091595, 1.114286)` during the test. Calls arrived at render cadence. This identifies the
    hook as the world-camera path and gives M2 a place to apply alternating left/right position
    offsets and, later, per-eye projection overrides.

## View struct + render entry points (offline, via `tools/pedis.py`, build 1.3528)

- `0x286A14` — **inner per-view renderer**, `rcx` = prepared view struct. Hooked as
  `RenderViewHook`; called exactly ONCE per frame (verified: `view renders 60/sec` vs `fps 60`).
- `0x1854C8` — **PrepareView**, a thin wrapper: publishes the view ptr to global `0x46BB978`,
  calls the real setup at `0x286620`, clears the global on exit.
- `0x286620` — **real per-view setup**. Consumes the view's camera/matrix pair exactly once, at
  `0x2866E2`: `lea rdx,[r14+0x98]; lea rcx,[r14+8]; call 0x2770F0` (r14 = view).
- **The view struct has a SECOND camera/matrix pair at `{camera@+0x158, derived@+0x1E8}`** —
  same 0x90 sizes, same +0x150 stride as `{+0x08, +0x98}`. It has a *different* consumer:
  `call 0x295DC0(view+0x158, <word>, view+0x1E8, view+0x27F4, ...)`, from exactly two sites
  (`0x2833A8`, `0x2A5B4D`); `0x295DC0` forwards to `0x2B8124` then `0x1B8148`. Site `0x2833A8`
  also reads `view+0x27E8/+0x27F4` — the same fields PrepareView reads — which **proves its
  `rbx` is the view struct**, so this is genuinely a second view-state slot.
  - **OPEN:** is it previous-frame state (temporal reprojection) or a secondary view? Settle by
    finding the engine's own writer of `view+0x158`; a per-frame current→prev copy proves it.
    `RenderViewHook` currently sets prev := current for both eyes (unjustified, from `bd2254e`).
  - Ruled out: `0x46A9E0` writes qwords to `+0x158`/`+0x1E8` but with unrelated fields
    interleaved at `+0x160..+0x174`/`+0x1D8` — a different struct, coincidental offsets.
- `0x2AF8C1` — the single call site every shader-constant upload funnels through (~100k/s), so
  caller-RVA cannot discriminate between effects. Do not build another constant census on it.

## Key lesson / next step

## First-person weapon path research (2026-07-14, offline)

### What HaloCEVR actually does

HaloCEVR is useful as an architectural reference, not as a source of reusable offsets (Halo CE is
a different 32-bit engine build). Its important choice is to hook/replace the **first-person view
model skeleton update**, rather than rotating the general camera or the finished render:

- `WeaponHandler::UpdateViewModel` receives the animation tag, root position/facing/up, local
  bone quaternions, and the output bone-transform array. It rebuilds the hierarchy itself.
- It identifies the wrist, gun, and display bones by name/tag metadata. In 6DOF mode it hides the
  original arm/root branches, moves the wrist to a calibrated controller transform, and preserves
  each weapon's authored wrist-to-gun relationship instead of assuming one universal gun offset.
- While walking the hierarchy it caches the gun/muzzle translation and rotation relative to the
  tracked hand (`fireOffset`, `gunOffset`, `fireRotation`). The crosshair and scope use that same
  derived muzzle ray, so the visible gun and aim stay coherent.
- Projectile direction is a separate problem. HaloCEVR temporarily replaces the player's
  position/aim immediately around the original weapon-fire function, then restores the real
  values. Moving the model alone is deliberately not expected to move bullets.

The lesson for Halo 3 is: find the function that consumes the first-person animation bone array
and writes final model-space/world-space transforms. The existing camera-copy hook and the static
`0x2D2F680` gun/overlay camera are too early and too broad for gun-in-hand placement.

Reference studied: https://github.com/LivingFray/HaloCEVR/blob/master/HaloCEVR/WeaponHandler.cpp

### THE ACTUAL PATTERN (read from HaloCEVR source, 2026-07-15) — set the ROOT, don't post-edit

The summary above under-sold the single most important line. `WeaponHandler::UpdateViewModel`
places the weapon by building a **root transform that it feeds INTO** the engine's composition:

```c++
Transform root;
Helpers::MakeTransformFromXZ(up, facing, &root);
root.translation = *pos;
```

It anchors that root to the camera position and applies the controller offset in camera-local
space (`pos->x = camPos.x; ...` then `MetresToWorld()` scaling) — i.e. controller expressed
relative to the head, exactly the space our `GetControllerFirstPersonTransform` already produces.

**Halo 3 hands us that same root as the composers' `defaults` argument.** It is built at
`0x2C45B5-0x2C45DC` as {scale 1, rotation IDENTITY, translation ~0} and passed at `0x2C4626`
(`lea r9,[rbp-0x38]`, arg4 of `0x2320B8`) and `0x2C464D` (`[rsp+0x28]`, arg6 of `0x23200C`).

Every failure of 2026-07-14/15 came from editing the **composed output** instead — which is
upstream of nothing and downstream of everything the engine does next (weapon lag at `0x2C484B`,
the render-packet copy at `0x2C490F`). Writing the root instead:
- needs **no new hook** (the compose hooks already receive it) — and detouring engine functions is
  what crashed the game;
- lets the engine compose the authored hierarchy (recoil/reload/IK) at the controller;
- leaves the lag pass operating on a correctly-placed assembly rather than fighting our edits.

Guard: never write a non-finite root. A NaN here becomes a crash deep in the renderer instead of a
visible glitch.

### Halo 3 map so far

- The static `0x2D2F680` object is an array of four **overlay cameras**, not four weapon model
  transforms. At `0x185745`, Halo loads the array base and loops with stride `0x2820`, calling
  `0x282EC4(camera, player/view index, ...)` for each active split-screen view. This explains why
  changing its forward vector flashes the complete gun + HUD together: it controls their render
  camera, not an individual weapon skeleton.
- `0x24CA50` is another overlay/render setup path. It loads the same camera base at `0x24CA71`,
  copies a 0x90-byte camera block, then calls `0x2A63E4` and `0x2A6980`; the latter derives a
  camera basis/projection block. This is also camera plumbing, not the desired bone-transform
  writer.
- The binary contains strong naming breadcrumbs for the next search:
  `g_first_person_weapon_orientations_allocator`, `g_first_person_weapons_allocator`,
  `render_first_person`, `debug_first_person_weapons`, `debug_first_person_hide_ik`, and the
  animation channel/name `weapon_ik`. Those point toward a dedicated first-person animation
  allocation/update stage downstream of general camera setup.
- The transform path is now mapped. Engine TLS `+0x568` points to four player records of
  `0x2430` bytes; each has two `0x11BC` held-weapon slots. TLS `+0x560` holds two 0x1000
  orientation banks per player. Each bank is two 0x800 halves of 64 records; a record is
  quaternion (16 bytes), translation (12), scale (4).
- `0x2C3B28` evaluates a first-person weapon. It initializes/copies authored local bones, runs
  animation, then calls `0x23200C` or `0x2320B8` to compose global 0x34-byte matrices into
  weapon `+0x4A4`. The exact `real_matrix4x3` layout is scale at `+0x00`, three contiguous
  forward/left/up vectors at `+0x04`, xyz translation at `+0x28`. The first headset build
  mistakenly placed scale last; every rotation was shifted by one float, producing the reported
  separated weapon pieces. The corrected transform rotates each complete basis vector.
  The array has capacity 64 and the selected weapon/gun anchor index is at `+0x11A4`.
- The render-packet copies happen later inside `0x2C3B28`, before it returns. Therefore the
  proven intervention boundary is the return from `0x23200C`/`0x2320B8`, not the return from
  `0x2C3B28`. The implementation rigidly transforms the already-composed global matrices,
  preserving authored recoil/reload animation, and binds slot 0 to the right aim pose and slot 1
  to the left aim pose. All addresses are found by unique AOB signatures.

### Direct aim synchronization

The first controller implementation drove Halo's normal right-stick loop. That preserved game
behavior but imposed the controller turn-rate/acceleration delay, so the direct-tracked gun,
game-aim crosshair, and projectiles visibly disagreed. ElDewrito's public Halo-3-derived
`RawInput.cpp` confirms the engine's raw mouse path updates adjacent horizontal/vertical view
angle floats in a player-control allocation reached through engine TLS. MCC moved the offsets.

The attempted dynamic resolver found zero candidates in the headset log, proving MCC's player
control layout does not expose this pair in the scanned TLS ranges. It has been removed rather
than retained as dead/speculative code. The established stick path remains active until the
actual fire arguments are proven.

The same headset result separated the first-person consumers: changing the composed 0x34 matrices
moved the muzzle flash, but the visible gun stayed head-relative. Therefore mesh skinning consumes
the 0x20 local orientation bank. The corrected hook now performs a two-pass composition: compose
once to obtain the selected anchor, apply the controller delta only to the bank's root quaternion
and translation, then ask Halo to compose again. This preserves the entire hierarchy and feeds
both the mesh bank and marker/effect matrices.

**RETRACTED (2026-07-15).** A previous edit of this file claimed the "gun stayed head-relative"
observation was caused by the wrong r_hand string index (`0x0C0000A6` vs raw `0xA6`) and was
therefore already fixed. That was a **theory written up as a finding, and the headset test
falsified it**: with the index correct and the hook provably running, the mesh still follows the
head while the muzzle flash follows the hand. Nothing about the mesh consumer is established.
Do not re-assert it. (Process note: this is exactly the "do not ship theories" rule being
broken, and it cost a play session.)

**REINSTATED, NOW WITH PROOF (2026-07-15, 03:30).** The paragraph above's conclusion — "mesh
skinning consumes the 0x20 local orientation bank" — was CORRECT after all; only the string-index
explanation deserved retraction. The decisive experiment ran by accident: the 03:23 build wrote
the controller pose into the composers' `defaults` root and NOTHING else, and the headset showed
the muzzle flash on the hand with the mesh still head-glued. Combined with the composer
disassembly, the chain is now closed:

- `0x23200C` structure (read directly): `output[0] = defaultsRoot * sourceRecord[0]`
  (`0x23203B`), then `output[i] = output[parent[i]] * sourceRecord[i]` (`0x232099`, parent from
  the tag node table word at `+8` of each 0x20 record, source stride 0x20).
- Therefore the composed output carried the controller pose in that build, and everything that
  followed the hand (flash/markers) reads composed output; everything that did not (the mesh)
  does not read composed output — not even via the packet memcpy at `0x2C490F`, which is
  therefore markers/effects data, not skinning input.
- The mesh recomposes from the **bank** (`source`) with its own camera-derived root; the ONLY
  build that ever moved the visible gun in a headset was the 07-14 bank-root build.

~~Current lever: write **bank record 0**~~ **FALSIFIED IN HEADSET (03:4x): bank record 0 does NOT
move the mesh either, and it bleeds the wrist into the body/camera** — record 0 propagates into
`camera_control`, which the game reads back to drive the camera (the node's name meant what it
said). Reverted; bank-record writes are banned alongside `0x120DF8` detours.

Standing scoreboard for the mesh consumer (everything falsified in a headset):
| Lever | Flash | Mesh | Side effects |
|---|---|---|---|
| composed output edits (07-14/15) | hand | head | none seen |
| composers' `defaults` root (03:27) | hand | head | none seen |
| bank record 0 (03:4x) | hand | head | wrist bleeds into body/camera |

So the mesh pose comes from NONE of the compose inputs/outputs we can reach in these hooks.
**RESOLVED (04:00) — the mesh is an OBJECT, rebuilt by the object-node recomposer.** Xref of the
shared multiply `0x1205AC` (7 callers) found it: function `0x341768` (single caller `0x3424DD`)
dequantizes the object's compressed 0x60-stride animation into 0x20 records and roots the chain
via `call 0x3453DC` at `0x341A5B`. `0x3453DC` (56 callers — generic, DO NOT DETOUR) fabricates
the root from the object datum: `MakeTransformFromXZ(fwd@+0x5C, up@+0x68)` + `pos@+0x50`, plus a
parent-attachment multiply (`[obj+0x10]` handle, `[obj+0x14]` bone, parent's composed nodes at
`+0x138`). The FP arms/weapon are objects held exactly at the camera each frame — that IS the
head-glue, and none of the TLS-bank/compose levers could ever reach it.

**FALSIFIED (04:00 session): the object-recomposer path never renders the FP mesh.** The
`0x341A5B` patch installed and ran an entire session without its collocation branch matching
once — no object root ever sat at the camera. The patch is inert and left installed (harmless,
logged). With that eliminated, the only remaining head-injector between composition and skinning
is the post-compose camera pitch/turn loop at `0x2C484B` (skips exactly camera_control — the
observed flash/mesh partition). Second call-site patch shipped at `0x2C485B` (disp32 at
`0x2C485C`, 4-byte aligned, atomic): SwayShim forwards every bone except addresses inside an
assembly ApplyControllerToRoot re-rooted this frame (thread_local ranges, cleared at compose
entry, armed on success — the ranges are consumed by the sway loop that runs immediately after,
same thread, so the earlier detour's crash trap of suppressing composition itself cannot occur).

**Fix shipped: a call-site displacement patch, not a detour.** The disp32 of the one `call` at
`0x341A5C` is 4-byte aligned -> swapped atomically (InterlockedExchange) at DLL-load time to a
12-byte near trampoline -> `FpRootShim`, which calls the REAL getter and, only when the returned
root is camera-collocated (<0.15 wu), rewrites the STACK buffer with the controller world pose.
No engine function detoured, 55 other callers untouched, no sim state written, NaN-guarded,
signature-gated (miss -> log + vanilla behaviour). Technique note for future work: single-site
interception via aligned disp32 swap is the sanctioned alternative to detours on shared engine
functions.

The 07-14 report that the bank build "moved the gun with the hand" now needs re-reading: what
moved was likely the flash/markers plus the camera (via camera_control feedback), not the mesh.

### First-person packet flow after composition (2026-07-15, offline)

Read from `0x2C3B28` after the compose calls (`0x2C4633`/`0x2C4663`):

- `0x2C4668-0x2C478E`: the selected camera_control matrix (index `weapon+0x11A4`) is cached at
  `weapon+0x11A8`, its translation gets a position (xmm6/7/8) subtracted, and the result is
  published to a global per-player record: base `[halo3+0x798CDC]` + player*`0x1A23EF4` +
  `0x1A23D70` (valid flag byte at `+0x1A23D6C`). This is the marker/effects anchor.
- `0x2C47B0-0x2C486C`: a small time-based sway/bob matrix multiplies every composed bone EXCEPT
  the selected index — this runs AFTER our hook edits, so the hand-anchored pose keeps walk bob.
- `0x2C48F0-0x2C490F`: `memcpy` of the raw composed bone array (`count*0x34`, no transform) into
  a render packet: base + player*`0x1A23EF4` + view*`0x67AC` + slot*`0x33D4` + `0x1A09E8C`
  (count at `+0x1A09E88`).
- **`0x184B40` is the packet consumer and it is an INTERPOLATOR**: it validates two packets
  (two sim frames, indices from a double-buffer selector), then per bone calls `0x183DC0(prev,
  cur, out, t)` — the renderer lerps first-person bones between the last two sim ticks every
  displayed frame. Consequences: (a) hook edits at sim rate still look smooth at 120 fps;
  (b) fast head motion drags the hand-anchored gun for ~1 sim tick (interpolant lags the fresh
  camera) — expected, small, not the "follows the head" bug.
- The packet bones are consumed **verbatim, camera-space**, drawn through the gun/overlay camera
  (`0x2D2F680` array; CamCopy's `dst` = element 0 + 8, confirmed by live log addresses). So the
  bone frame = whatever pose CamCopy leaves in that camera — which is why the head pose must
  stay in the authoritative source camera (see CONTINUATION 07-15).

### PROVEN: the composed first-person bones are CAMERA-space (2026-07-15, offline)

The root transform passed to the composer is built at `0x2C45B5-0x2C45DC` and handed over as the
`defaults`/root argument (`lea r9,[rbp-0x38]` at `0x2C4626`; `[rsp+0x28]` at `0x2C464D`):

- `[rbp-0x38]` = `1.0` -> **scale = 1**
- `[rbp-0x34..-0x14]` = `(1,0,0) (0,1,0) (0,0,1)` -> **rotation = exact IDENTITY**
- `[rbp-0x10..-0x08]` = the accumulated offset vector -> **translation ~= 0**

The offset itself is accumulated at `0x2C4518-0x2C4587` from a callback's vec3 output divided by
the constant at `rva 0x847498` = **1000.0**, so it is a millimetre-scale nudge, and its Y term is
sign-flipped for the dual-wield mirror (`neg [rbp+0xD8]; sbb; and 2` at `0x2C453A`).

Therefore the bones live in a space anchored at the origin with identity orientation = the
first-person camera's own space, at world-unit scale. This validates the *space* our hook writes
in (head-relative, game camera axes x=forward/y=left/z=up). It does NOT explain the mesh.

### SOLVED: why the gun stuck to the head while the muzzle flash followed the hand

The first-person evaluator **undoes our pose one instruction after we write it**. At
`halo3+0x2C4790-0x2C486C`, immediately after the composer returns:

1. `0x2C4695` caches `bones[selectedIndex]` (camera_control) into `weapon+0x11A8`, which is
   published to the global effects anchor `+0x1A23D70`. **This happens BEFORE step 3.**
2. `0x2C4797-0x2C4809` builds a rotation matrix at `[rbp-0x38]`: scale `1.0`, translation
   **exactly zero** (constants at `rva 0x768FC0`), rotation = `0x1214CC(euler)` whose angles come
   from trig over `[rax+rbx+0x1A8]` (a per-weapon lag scale) and camera turn. This is Halo's
   first-person **weapon-lag / pitch-turn sway** (cf. `debug_first_person_hide_pitch_turn`).
3. `0x2C4843-0x2C486A` loops every bone and, **skipping only `selectedIndex`**
   (`cmp r9d,[rdi+0x11A4]; je`), calls `0x120DF8(sway, &bones[i])` — rotating it in place.

So the partition is exact and explains both headset reports verbatim:
- `camera_control` is cached pre-loop and skipped in-loop -> keeps our edit -> **muzzle flash and
  markers follow the hand**.
- Every other bone — i.e. **the entire visible mesh** — is rotated after our edit -> **the gun
  rides the camera**. In VR the head drives that camera continuously, so the lag rotation *is*
  the head-stick. This also explains the "follows both head and hand" intermediate report: the
  mesh was carrying our translation plus the camera-driven rotation.

**Fix shipped:** hook `0x120DF8` (unique AOB, verified single match on disk) and skip it for
exactly the bone address range we re-anchored, armed via a **thread-local** range set only on a
successful transform and cleared at every bail. `0x120DF8` is a shared helper with ~121 call
sites, so the hook must stay a range compare + tail call — never add work there (the constant-
upload census cost ~25% fps at a comparable frequency). The engine skipping the same rotation for
`camera_control` is proof that "unswayed" is a state this pipeline supports.

**ROOT CAUSE OF BOTH FATAL ERRORS — halo3.dll's LTCG register contracts.** The sway loop keeps
its counter (`r9d`), bone pointer (`r8`) and count (`r10d`) live in VOLATILE registers across
`call 0x120DF8` because whole-program optimization knows the callee never touches them. Any
compiled C/C++ interposition (MinHook detour OR a C++ shim behind a call-site patch) clobbers
them -> the caller's loop state is garbage -> wild writes -> fatal error on level load. This
retroactively explains the 03:05 detour crash AND the 04:0x call-site-shim crash: same cause.
RULE: before interposing ANY halo3.dll-internal call, read the caller's post-call code and list
which volatile registers stay live; a shim may clobber only provably-dead ones. The shipped
pitch/turn shim is hand-emitted machine code using ONLY rax (verified dead at 0x2C4860+); the
`0x341A5B` FpRootShim caller was verified to rely only on nonvolatile registers, so a C++ shim
is safe THERE specifically.

**DO NOT HOOK `0x120DF8` — it crashes the game on level load, on contact.** Tried 2026-07-15 and
proven by elimination: the skip range was never armed (the unconditional probe log at the top of
the transform never printed), so the detour was a pure pass-through, and the game still died the
moment a level loaded. Surviving 92 s beforehand proves nothing — halo3.dll's model pipeline does
not run in the menus, so **the first real call IS the level load**. Do not re-attempt a detour on
this function, and do not use "it ran for N seconds" as evidence a hook on halo3.dll is sound
unless a level was actually loaded.

**The right lever is the ROOT, not the output** (see the HaloCEVR section above): the composers'
`defaults` argument IS the root transform, we already receive it, and writing it needs no hook at
all. The two notes below are kept only because they are true of the code, not because output
editing is the approach.

**CRASH TRAP — the composer builds its bones with the very same applier.** `0x23200C`/`0x2320B8`
call `0x120DF8` at `0x23217E`, `0x2321FF`, `0x232212`, `0x23234C`. So an armed skip range that
survives into the next frame's composition **suppresses the composition itself**, leaving
uninitialised matrices -> MCC fatal error one frame after the range is first armed (observed
2026-07-15: hook installed and passing through harmlessly for 92 s, crashed the instant F2 armed
the range for the first time). **The range must be disarmed at the top of the compose hooks,
before the original runs** — clearing it inside `ApplyControllerToComposedBones` is too late,
because that runs *after* the composer. Arm it only across the window between our transform and
the weapon-lag loop.

Diagnostic value of the log here: the absence of the `complete first-person slot ... bound` line
(emitted at the END of the transform) plus a 92-second survival time is what localised the crash
to the first execution of the armed path, rather than to the detour mechanics.

Note this retroactively kills the "mesh consumes the local orientation bank" theory too: the mesh
consumes `weapon+0x4A4` exactly as the memcpy at `0x2C490F` implies — our edits were simply being
overwritten in between.

### Superseded: the contradiction that led here (kept for the reasoning trail)

All of the following are established, and together they should mean the mesh follows our edits:

- `0x23200C`/`0x2320B8` are the **only** two composers, with exactly **4 callers** binary-wide;
  the only two first-person ones are `0x2C4663`/`0x2C4633`, which we hook. There is no second,
  render-time recomposition.
- Both call sites' argument orders match our hook declarations (verified register-by-register),
  and `0x2320B8` calls `0x23200C` internally at `0x23211F`, which the `g_insideSpecialCompose`
  thread-local correctly suppresses.
- Our edits reach the effects/marker anchor (`weapon+0x11A8` -> global `+0x1A23D70`) — **observed
  in the headset**: the muzzle flash follows the hand.
- Our edits are `memcpy`'d verbatim into the mesh's render packet at `0x2C490F`
  (`src = weapon+0x4A4`, `size = [weapon+0x4A0]*0x34`), and that memcpy shares its gate
  (`TLS+0x88` non-null, `TLS+0x5D8` != 0) with the effects publish that demonstrably runs.

Yet the visible mesh does not move. So one of the "established" facts is false, or a consumer we
have not found re-derives the mesh pose. The disassembly cannot distinguish these. Shipped a
`weapon_probe` config/F1 flag that applies a fixed +0.3 left translation to every composed bone
with no controller input: if the **gun mesh moves**, it reads `weapon+0x4A4` and the bug is in our
head-relative math; if **only the muzzle flash moves**, it does not, and the real consumer must be
found (candidates: the interpolated render buffers `[0xA5D400]`/`[0xA5D408]` filled from the sim
buffer `[0xA5D358]`, and `enable_first_person_prediction` / `TLS+0x5D8`).

Note: `0x184B40` was earlier written up here as "the packet consumer / interpolator". That read
started mid-function (the `mov eax,[0x58]` decode is a misparse of `gs:[0x58]`), which is also
why it appears to have zero xrefs. The interpolation shape (`0x183DC0(prev, cur, out, t)` per
bone, two frame buffers selected via `TLS+0x88`) is real, but the function's identity and entry
point are NOT established.

### Weapon-fire / projectile-spawn hunt (open)

Goal: HaloCEVR-style hook that swaps aim origin+direction around the fire call so bullets leave
the muzzle. Status of the offline hunt in build 1.3528:

- `'proj'` fourcc immediates cluster at `0x8DE24-0x8FAE6` — that is the **network replication
  encoder/decoder** for projectile creation (bit-packing; placement struct: `+0x04` position,
  `+0x10`/`+0x20` vectors). Useful for struct layout, wrong path for local spawn.
- `0x12B460` (fourcc on stack) is a tag prefetch table, not creation.
- All five code references to the overlay camera array `0x2D2F680`: builder `0x68C6`, per-view
  loop `0x185745`/`0x1857E5`, overlay render setup `0x24CA71`, and scope-zoom FOV math
  `0x2B7A12`. No fire path reads it statically.
- The published camera_control record (`+0x1A23D70`) has **no statically-visible readers**
  (consumers reach it through allocator pointers), so the effects/fire consumers need either a
  live watchpoint (`camscan findwrite`-style, but for reads) or tracing `0x18A1F0`-adjacent
  render code.
- Sound-relevance strings ("projectile creation: relevance=%5.3f" at `0x766100`) are data-driven
  event tables — weak anchors.

Next candidates: xref the projectile update/impact cluster callers upward to find creation; or
live-trace with a hardware read watchpoint on the camera struct position while firing a single
shot (one game session, no headset needed).

### Critical stereo correction and successful raster redirect (2026-07-14)

The original F11 path did **not** prove two independent world images: calling the inner renderer
twice raised GPU work but reused shared targets. The corrected path now binds distinct final-scene
RTVs per eye, and the user confirmed the headset stereo image works beautifully. An objective
pixel-disparity capture remains useful validation, but the shared-backbuffer path is superseded.
One-time GPU readback subsequently measured mean RGB delta 6.262 and 48.8% changed samples
(11,631/23,842), objectively confirming the submitted eye textures are distinct.

Do not repeat: exaggerated OpenXR eye poses (double vision), modifying shader constant `0x270000`
(reconstruction warp/holes), or changing shared projection `view+0x98+0x78` (weapon/HUD damage).
See `CONTINUATION.md` for the mandatory proof sequence.

The camera-copy destination cannot be safely treated as weapon-only. A test that copied the original
stick-aim vectors to `dst` while leaving headlook in `src` disabled world head tracking. Therefore
weapon/HUD decoupling must be found after this shared camera copy, at the overlay render pass or a
weapon-specific transform; do not repeat the save/restore split in `CamCopyHook`.

The raster path hooks D3D11 `OMSetRenderTargets` at vtable index 33. Census showed Halo's inner pass
does not bind the game backbuffer. The correct final scene target is the unique full-resolution
`R8G8B8A8_TYPELESS` resource with RTV+SRV+UAV bind flags (`0xA8`), bound after the typed RGBA
intermediate. Redirect that final resource only. Redirecting the format-28 intermediate is black.
Fake post-render backbuffer copies remain disabled. Eye caches use the original RTV's typed view
format so Halo's sRGB conversion is preserved.

External poking can only ever *flash* a camera value because it races the game, which rewrites
these every frame right before rendering. **To hold the camera steady, the mod must write from
inside the game's frame** — i.e. hook the game's camera-update function (e.g. the one at
`0x2A86D2`) or the render-camera setup, and overwrite the orientation there each frame with the
head pose. That's the M1 mechanism to build next.

## Coordinate convention (from observed data)

Halo forward is a unit vector `(i, j, k)` with **k = vertical** (up/down look → k changes) and
`(i, j)` = horizontal heading. Engine appears **Z-up**. OpenXR head is Y-up, -Z forward, meters —
so mapping head→game forward needs an axis swap + a yaw recenter (to be calibrated in-headset).
