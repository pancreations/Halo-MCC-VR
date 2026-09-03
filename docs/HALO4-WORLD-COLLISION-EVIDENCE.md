# Halo 4 world-contact evidence

## Scope and parity statement

Halo 3 has no world-contact implementation in this repository. There is no
accepted player-experience reference to port and no cross-title address,
layout, flag, or physics behavior is reused. This document supports a
Halo-4-only experiment: sweep a bounded set selected from the actual transformed
Storm hand and held-model nodes with Halo 4's own clear-line collision filter,
move the common visible carrier before a hit, and provide gentle per-hand
OpenXR feedback. A separately verified native object-motion helper can give a
finite linear push to an object returned by that query. Physical melee and
damage remain explicitly outside this candidate.

The feature is optional. Missing proof or any runtime query failure
returns only the wrist-contact feature to the existing floating-hand behavior.
It does not disarm the Halo 4 camera, hands, HUD, reticle, effects, or OpenXR.

## Official H4EK evidence

Primary executable:

- `halo4_tag_test.exe`
- SHA-256 `B7468DB9FD160B035C329540EE0B0D47BCF609E1BA6E85AE4F204B70661113A6`
- embedded source path
  `c:\mcc\release\h4\shared\engine\source\blofeld\physics\collisions.cpp`

The official executable names `PhysicsRayCast`, `PhysicsRayCastGroup`, and
`PhysicsRayCastGroupFromSinglePoint`. The simple query is H4EK RVA `0x4E7E30`.
Its call graph and data flow establish this ABI:

- x64 `bool PhysicsRayCast(input*, result*)`;
- input profile/category dword at `+0x00`;
- collision flags at `+0x04` and an additional flags dword at `+0x08`;
- start xyz at `+0x0C`, end xyz at `+0x18`;
- 64 ignored object indices at `+0x24`, count byte at `+0x124`;
- optional result-detail bytes at `+0x128..+0x12A`;
- input allocation therefore covers `0x12C` bytes;
- result type at `+0x00`, fraction at `+0x04`, hit position at `+0x08`,
  normal at `+0x14`, object index at `+0x28`, and observed writes through
  `+0x68`; the implementation supplies a zeroed `0x70`-byte result;
- the function initializes fraction to exactly `1.0` and returns
  `fraction != 1.0` in `AL`.

The individual bit values below are title-specific findings, not enum-order
guesses:

- H4EK `object_test_vector` at RVA `0x4ECBD0` directly evaluates
  `(collision_flags & 1) == 0` immediately before the embedded assertion
  `TEST_FLAG(flags.collision_flags, _collision_test_structure_bit)`. Structure
  is therefore bit 0.
- H4EK filter callback RVA `0x4ED700` passes immediate `0x1B` to the engine's
  checked bit-test helper for `_collision_test_fixed_objects_only`. Fixed-only
  is therefore bit 27.
- Multiple ordinary H4EK callers pass profile/category `0x1A`.

The official assertion also states that a query during object movement is
allowed only when it is not on the main thread or the fixed-only flag is set.
Stage 4 therefore does not call from the cold title worker or render/palette
hook. It borrows a context only after the engine itself has completed a
dynamic-inclusive `PhysicsRayCast` in the hooked function.

### Official object-motion contract

H4EK registers two `object_set_velocity` script overloads. Their callbacks at
RVAs `0xED48F0` and `0xED4770` convert the requested local vector through the
object basis and both call native helper RVA `0xE539C0` with the object index,
linear xyz pointer, and null angular pointer. That helper:

- commits the supplied linear/angular values through `0xE53A80`;
- mirrors them to the object state through `0x14E600`;
- returns without waking for a zero linear and zero angular request;
- otherwise calls the same wake helper used by registered script
  `object_wake_physics`, then refreshes and commits motion.

This is velocity assignment and wake-up, not melee, damage, or a Havok contact
callback. Stage 4 supplies only a finite, world-scale-capped linear vector and
leaves angular velocity untouched.

### Official clear-line filter contract

H4EK RVA `0x17BF10` is a direct clear-line wrapper. It receives start/end
positions and an optional ignored object, builds profile `0x1A`, copies the
prebuilt qword at H4EK image RVA `0x512CD40` to the ray input's two filter
dwords, calls `PhysicsRayCast`, and returns the inverse hit result.

The official initializer chain proves the exact filter pair rather than merely
the meanings of two isolated bits:

- `0x512CCF8` receives qword `0x0000000000001009`;
- `0x512CD08` receives qword `0x0000518500000000`;
- `0x512CD38` combines those with low `0x0000C000` and high `0x00000001`;
- `0x512CD40` adds low `0x00020000`.

The resulting input dwords are collision flags `0x0002D009` and additional
flags `0x00005185`. Stage 2 copies that exact pair. It does not infer or rename
the remaining categories whose individual enum meanings have not been proven.

## Retail verification

Pinned retail `halo4.dll`:

- SHA-256 `7C53E7D5BC9848545A1B70E2768242479336FBA1B7630D7AB955F7FD0C34FA84`
- MD5 used by the local Ghidra/BSim database
  `d8ae540894767d98fbb3aef22f491026`

H4EK-to-retail BSim mapped the distinctive query neighborhood independently:

- H4EK filter constructor `0x4E5350` -> retail `0x1C0D94`;
- H4EK world-ray core `0x4DECE0` -> retail `0x2748AC`;
- H4EK collector-to-result conversion `0x4EA0D0` -> retail `0x1C15A8`;
- both group-ray functions map to the retail group-query neighborhood at
  `0x1C1EDC` and `0x1C24A8`.

The intervening retail simple query is RVA `0x1C1D4C`, extent
`0x1C1D4C..0x1C1ED9`. It has the same input/result initialization, calls the
mapped filter/world/conversion functions, compares result fraction to `1.0`,
and returns the comparison in `AL`.

The retail homolog of the official clear-line wrapper is RVA `0x0F4218`. It
has the same profile, position/ignored-object construction and inverse-hit
return. Its direct call at `+0x84` targets retail `PhysicsRayCast` above, and
its RIP-relative load at `+0x3E` resolves the engine-owned filter qword at
retail RVA `0x2FFB0B8`. The retail initializer chain independently produces
the same value:

- `0x01D6EC` initializes the source at `0x2E93D58` to qword `0x1009`;
- `0x01D718` initializes the source at `0x2ED29E8` to high dword `0x5185`;
- `0x01D820` combines them with low `0xC000`/high `1` at `0x2FFB0B0`;
- `0x01D888` adds low `0x20000` at `0x2FFB0B8`.

The wrapper's loaded-image signature must match exactly once at the pinned RVA:

```text
40 55 48 8D AC 24 30 FF FF FF 48 81 EC D0 01 00 00
48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 C0 00 00 00
8B 42 08 41 83 C9 FF F2 0F 10 02 66 83 A5 B8 00 00 00 00
```

At installation, Stage 2 also safely reads the live engine qword and requires
the exact value `0x000051850002D009`. A missing/multiple/moved wrapper, changed
call or load target, or different live qword returns only world contact to
`StockFallback`.

The wrapper is demonstrably live engine code, not an unreferenced helper. Its
three direct H4EK call sites and retail homologs are:

| H4EK caller function / call | Retail caller function / call |
|---|---|
| `0x17B2C0` / `0x17B4F6` | `0x0F42C0` / `0x0F4403` |
| `0x17B5D9` / `0x17B68C` | `0x0F4428` / `0x0F4540` |
| `0x17B5D9` / `0x17B6B3` | `0x0F4428` / `0x0F456B` |

Stage 3 hooks only this uniquely verified wrapper. Every admitted callback
invokes Halo 4's original trampoline first with the game's untouched arguments.
An experimental wrist query is eligible only after that original returns, so
the same engine-owned thread/TLS and collision-safe phase have just successfully
executed the exact wrapper/filter being reused.

The runtime does not trust that address alone. This loaded-image signature must
match exactly once and at the pinned RVA:

```text
48 8B C4 55 56 57 48 8D A8 ?? ?? ?? ?? 48 81 EC 10 0A 00 00
48 C7 44 24 30 FE FF FF FF 48 89 58 18 48 8B 05 ?? ?? ?? ??
48 33 C4 48 89 85 00 09 00 00 48 8B F2 48 8B F9
```

It then decodes and requires four direct call edges from that matched body:

| Query offset | Required retail target | Role |
|---:|---:|---|
| `+0x4B` | `0x1C12A8` | result initialization |
| `+0xE4` | `0x1C0D94` | collision filter constructor |
| `+0xFF` | `0x2748AC` | world raycast |
| `+0x10F` | `0x1C15A8` | collector-to-result conversion |

Zero/multiple signature matches, a moved RVA, a changed call edge, an unstable
module mapping, or an unavailable floating-hand palette produces
`StockFallback` for world contact only.

H4EK-to-retail BSim independently maps native object-motion helper H4EK RVA
`0xE539C0` to retail RVA `0x5D1580` with similarity `1.0` and significance
`57.2413835768`. The retail body reproduces the same zero-vector test and the
same five-call commit/wake sequence. Its loaded-image signature is unique at
the pinned RVA:

```text
48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 45 33 C9
49 8B F8 48 8B F2 8B D9 E8 ?? ?? ?? ?? 41 B1 01 4C 8B C7
48 8B D6 8B CB E8 ?? ?? ?? ?? F3 0F 10 1D ?? ?? ?? ??
```

Installation also decodes and requires its direct calls at `+0x1A`, `+0x2A`,
`+0x93`, `+0x9A`, and `+0xA1` to target retail RVAs `0x5D14D8`, `0x0F1ADC`,
`0x5D73BC`, `0x5D0F38`, and `0x5DB6D0`. If this independent proof fails,
hand/weapon world collision can remain live while object motion alone reports
`StockFallback`.

## Stage 3 runtime transaction

- The render thread publishes the pair-frozen final left/right target
  positions through bounded atomic sequence records. It never calls physics,
  logs, allocates, locks, or performs file I/O.
- The cold title worker never invokes collision. It only reports a failure
  atomically published by the wrapper detour.
- The detour first completes the engine's original clear-line request. A
  single atomic lease then permits at most one experimental batch per 50 ms,
  even if multiple engine threads enter the wrapper concurrently.
- The first wrist sample and any movement over 1.5 metres reseed rather than
  sweeping across a teleport/recenter.
- Before wrist corrections are trusted, the original wrapper trampoline casts
  a diagnostic-only line 20 metres downward from a tracked wrist.
  Until a hit validates that the live environment and published coordinate
  frame agree, hands stay on their unmodified floating targets. The calibration
  ray never produces correction or haptics. Telemetry reports its hit/query
  counts and `environment VALIDATED` versus `unproven`.
- The wrapper returns only clear/blocked. For a blocked wrist segment, six
  bounded clear-prefix tests locate the first blocked interval. The existing
  resolution math stops 1.5 cm before that interval and publishes only a
  finite, bounded translation correction. No orientation or engine state is
  written, and the rejected raw `PhysicsRayCast` result ABI is never consumed.
- Render accepts only a same-generation correction no older than 150 ms whose
  source target remains within 15 cm and whose magnitude is at most 75 cm.
  Otherwise it uses the unmodified existing target.
- A contact raises a `0.18` per-hand peak. The OpenXR frame path merges that
  peak with existing game rumble by maximum and applies the configured global
  haptic intensity. No engine worker calls OpenXR.
- An experimental wrapper exception atomically disables this optional feature;
  the cold worker emits one explicit fail-open log. Two-second telemetry
  separately reports engine wrapper callbacks, mod wrapper queries, contacts,
  visible corrections, resets, calibration, and failures.

## Stage 4 runtime transaction

- Rejected Stages 1-3 stay compiled dormant. Stage 4 hooks the uniquely pinned
  retail `PhysicsRayCast` itself. Every callback completes the untouched engine
  request through its trampoline first. The hook never changes the engine
  request, result, or return value.
- The hook counts every live engine call, but an experimental batch is eligible
  only if that just-completed engine input did not request H4EK's proven
  fixed-objects-only bit 27. This makes the borrowed context demonstrate that a
  dynamic-inclusive query is legal there. One atomic lease and a 33 ms interval
  bound concurrent callers and query rate.
- The exact 80-node Storm palette supplies each wrist root and its authored hand
  subtree. The immediately following held-model palette supplies its authored
  nodes. A deterministic selector retains the common root and up to six unique
  world-axis extrema from each model: at most seven left-hand samples and
  thirteen combined right-hand/weapon samples. No guessed capsule, box, bone,
  marker, or copied cross-title dimension is introduced.
- Render/palette code only constructs those bounded stack arrays, publishes
  atomics, and consumes a prior finite correction. It performs no engine query,
  OpenXR call, logging, allocation, lock, file I/O, or signature scan.
- Each sample sweeps from its last accepted position to the latest authored
  target. The strongest hit-derived common translation stops the whole visible
  hand/weapon carrier 1.5 cm before contact. First samples, sample-count changes,
  and movements over 1.5 metres reseed rather than drawing a false sweep across
  a weapon switch, load, teleport, or recenter.
- The Storm callback's object index is placed in the official ignored-object
  array, preventing the player's own first-person owner from being selected.
- A hit with a non-sentinel, non-player object index can call the independently
  verified native object-motion helper. Velocity comes only from that sample's
  measured displacement/time, is scaled to 65%, and is capped at 2 m/s in
  world-scaled units. The helper receives no angular velocity and no damage or
  melee call is made.
- Object motion has its own failure switch. An exception disables only pushes;
  hand/weapon clamping and haptics continue. A ray/query contract failure
  disables only Stage 4 world contact and pushes; the Halo 4 camera, hands,
  weapon, HUD, reticle, helmet, effects, pause repair, black-screen repair, and
  OpenXR session continue.
- Two-second telemetry distinguishes total/dynamic-safe/fixed-only engine
  callbacks, mod probes, left/right hand contacts, weapon contacts, visible
  corrections, reseeds, environment-hit proof, ray failures, and object-push
  attempts/completions/failures. These counters are the activation evidence;
  install success alone is not acceptance.

## Acceptance and limitations

### Stage 1 headset rejection (2026-09-03)

The user's Steam / SteamVR-Oculus / 120 Hz headset run loaded exact source
`6b301ad681f35765568cdbcb86743de49840327f`. The supplied 861-line log has
SHA-256 `06BC41C74142497E8B0D9267F3433A18911EEE3A6B096EB5759C81BE05B7657E`.
The binding installed and the worker completed 1,794 reported queries while
both floating-hand paths continued publishing normally. Every two-second
interval reported zero left
and right contacts, zero visible corrections, zero failures, and therefore no
contact haptics. The user confirmed that hands still passed through tested
objects with no visible or tactile response.

This rejects the Stage 1 behavior. It proves that deployment, target
publication, worker scheduling, and the non-throwing retail call edge were
live; it does **not** prove that the selected flag set described a useful
world-geometry query. The claim above that structure bit 0 plus fixed-only bit
27 was sufficient is now only a disproven lead. The Stage 1 install is compiled
dormant in source commit `9531abab7371691c637f8b0df8bd435d21bf308b`.

H4EK and the pinned retail wrapper subsequently proved the actual clear-line
filter pair `0x0002D009`/`0x00005185`. Stage 1 had used
`0x08000001`/`0x00000000`: it omitted the engine's actual filter categories and
added fixed-only by itself. Stage 1 also rejected a true hit when result type
was zero even though the official function's public contract is its boolean
return plus fraction. Stage 2 replaces both disproven assumptions; it does not
stack on the rejected Stage 1 behavior.

### Stage 2 headset rejection (2026-09-03)

The user's Steam / SteamVR-Oculus / 120 Hz run loaded exact source
`6488f06d40dbc686af71adc8472382ebcd86160d`. The supplied 395-line log has
SHA-256 `0CA809D4B4CEF65E171D007C80BB98AC2C7D78E1556DC31D30E0B142DE50AA2E`.
It establishes this sequence:

- `10:31:05.048`: Stage 2 installed with the raycast, wrapper and live filter
  qword all pinned;
- `10:31:05.770`: its first calibration call raised an engine exception and
  world contact logged `FAILED OPEN`;
- `10:31:06.225`: runtime mode changed from gameplay back to loading;
- `10:31:06.834`: the game had stopped presenting for one second;
- `10:31:08.396`: the Halo 4 core observed no camera heartbeat for 2,532 ms and
  retired after the level closed.

Telemetry confirms exactly one query, zero calibration hits, zero contacts and
one failure. The exception handler successfully stopped further collision
calls, but could not undo state already disturbed inside the engine function.
That violates feature isolation from the working Halo 4 path.

The exact filter pair remains valid evidence; the rejected fact is the call
context. A query that reaches Halo 4's real collision path is not safe from the
mod's arbitrary 50 ms worker thread. Stage 1 did not reveal that because its
ineffective flags returned without reaching the same path. Both Stage 1 and
Stage 2 execution are now compiled dormant. A successor must first prove an
engine-owned update/physics context from H4EK and its retail homolog. Moving
the call into a render or palette hot hook is not an acceptable substitute.

### Stage 3 headset rejection (2026-09-03)

The user's Steam / SteamVR-Oculus / 120 Hz headset run loaded exact source
`0a37cd47d69242ea9d0a6af74ec9c2ad7cd338a0`. The supplied log has SHA-256
`CF11AE88CCFB50131C7FE506B0F202861B219157A9E16268B49AE05A55E1B0E1`; the
matching video has SHA-256
`BCB03393D23AF5E6B3A9173C25759103F98BC7A9D56105936997E0905C23807C`.

The hook installed, Halo 4 remained stable, and the normal floating-hand paths
continued publishing. Every telemetry window nevertheless reported exactly
zero engine-owned clear-line callbacks, zero mod wrapper queries, an unproven
environment, zero contacts and zero visible corrections. The video and user
report confirm both hands passed through tested geometry with no haptics.

This rejects Stage 3's activation premise. Static call references prove only
that the wrapper exists; they do not prove that retail campaign gameplay uses
it. Stage 3 is compiled dormant. A replacement needs a function proven live in
ordinary Halo 4 campaign gameplay, not another merely referenced helper.

Stages 1-3 were wrist-point sweeps, not hand-volume collision, and could not
push a ragdoll. The requested successor must separately prove both world-volume
response and dynamic-object interaction while leaving physical melee disabled.

### Stage 4 partial headset acceptance (2026-09-03)

The user's Steam / SteamVR-Oculus / 120 Hz run loaded exact source
`7451e703eb78ddf381056e538a2a8388cf1d740a`. The supplied log has SHA-256
`4C66838A1F640483517A2911B2EA218C045C0F670F721C87A53E1D3E49BAE796`;
the matching video has SHA-256
`E8FB18CC48D84DD6F37A9BB2AFB378CDC3202BFA3C35BB701E8888F4F2D3BC96`.

Across 57 telemetry windows, Stage 4 observed 1,071,329 dynamic-safe engine
ray callbacks, 27,732 authored probes, 115 left-hand contacts, 62 right-hand
contacts, 36 weapon-extrema contacts, 251/218 applied left/right corrections,
56 successful object pushes with zero push failures, and zero ray failures.
The user explicitly confirmed that both rendered hands collided with world
geometry, contact haptics fired, and live ragdolls/body parts reacted to hand
contact. Those three portions are accepted and must remain intact. Physical
melee remained disabled as intended.

The held weapon did not visibly react and is rejected as a separate optional
sub-feature. Telemetry recorded 985 collision-state reseeds. Code inspection
shows the right target was published first with the hand extrema and then with
the combined hand/weapon extrema, so the engine-context consumer repeatedly
saw a changed sample count and deliberately reseeded instead of sweeping.
Weapon contact counts therefore prove that the authored volume reached the
query, not that its continuous response was stable. The alternating combined
publication is compiled dormant before a successor is attempted; the accepted
hand, haptic, and dynamic-object transactions remain live.

### Stage 5 stable held-volume candidate

Halo 3 has no world-contact behavior to match, so this remains an explicitly
Halo-4-native opt-in feature rather than a parity claim. Stage 5 changes only
the rejected right-side publication boundary: the Storm record caches the
authored right-hand extrema but publishes only the left hand immediately. The
same-eye held record then publishes the cached hand extrema and the held-model
extrema together, exactly once. Stable sample count and ordering let the
existing engine-context sweep carry accepted positions between ticks, so a
barrel contact produces the same bounded rigid carrier translation already
proven on the right hand. It does not add a second weapon transform, detach the
gun from the hand, increase ray cadence, change the 1.5 cm contact skin, or
touch damage/melee.

The shared `world_collision` setting ships off and appears in F1 under
Body (VRIK). Halo 4 mirrors it into an atomic used by the palette and raycast
hot paths. A live off/on transition invalidates prior worker state before any
old correction can be consumed. Other titles ignore the option until they
gain their own evidence-backed implementation. The Stage 4 hand-volume,
haptic, exact raycast/filter binding, dynamic-object helper, bounds, cadence,
and feature-local fail-open behavior are otherwise unchanged.

### Stage 5 partial headset acceptance / weapon rejection (2026-09-03)

The user's Steam / SteamVR-Oculus / 120 Hz run loaded exact source
`ae96c406dae08301579490be8f9042911de38b1a`. The supplied log has SHA-256
`0E61F973DA010D0BB95FCB49467A97CAD24129A344446825F9923EB1308019CF`.
The user confirmed that the toggle works, collision is materially more solid,
and the hand path is a good baseline. The assault-rifle barrel still passed
through a rock, so the weapon portion is rejected while the accepted hand,
haptic, and object-response portions remain protected.

Across 20 enabled telemetry windows the log totals 329,193 engine raycast
callbacks, 10,664 authored-volume probes, 20/24 left/right hand contacts, 66
nominal weapon contacts, 43/200 visible left/right corrections, 231 reseeds,
and zero ray or object-push failures. The held render-model identity is the
assault-rifle checksum `0x1814181C` with only five animated nodes. Contacts at
five node origins prove the Stage 5 publication ran; they cannot establish a
collision proxy over the visible barrel and stock between and beyond those
origins. Stage 5's animated-node weapon proxy is therefore compiled dormant in
its own revert commit. A successor must use title-authored model geometry or
bounds, keep unknown models hand-only, and leave physical melee disabled.

## Stage 6 authored render-model bounds evidence

Halo 3 has no world-contact feature to match. This remains a Halo-4-native,
opt-in implementation, and failure of weapon geometry must not affect the
already accepted hand transaction.

Official evidence comes from the installed H4EK 1.890 toolchain:

- `tool.exe` SHA-256
  `5E0AD8D03EC4B1C7F4C0C2A18C92CEC9F92F3EB7E7F0CBB7376BA9D866E3A758`;
- `bin/ManagedBlam.dll` version 1.890.0.0, SHA-256
  `D2048A593399DA022BFC930D52A5CBA22B9197663F6416C20522E2B5235810CC`;
- exported `storm_assault_rifle.render_model` XML SHA-256
  `2DB008E3DD869A88C4875438A9E5CD9108153E23C287CAA2F07580E393CA3BE2`.

The official assault-rifle `.weapon` tag references
`objects\\weapons\\rifle\\storm_assault_rifle\\storm_assault_rifle.render_model`.
Its render-model tag contains runtime import checksum `0x1814181C`, five nodes,
and compression-position bounds min
`{-0.0935352,-0.0122993,-0.0122084022}` / max
`{0.23352,0.0129804034,0.0776163}`. The user's Stage 5 runtime log independently
reports the same checksum and five-node count. The bounds span roughly 0.327
world units on the model's longitudinal axis and therefore describe the barrel
and stock that five animated node origins missed.

ManagedBlam reflection verifies the relevant official tag layout: runtime
checksum at render-model root `+0x08`; nodes block at `+0x30`; render-geometry
struct at `+0x64`; compression-info block at root `+0x80`; 52-byte compression
records with flags at `+0x00` and six packed min/max position floats at
`+0x04..+0x1B`. An exhaustive read-only scan of all 77 official `.weapon` tags
found 41 unique first-person render-model references. Thirty-nine referenced
tags exist and produced unique runtime-checksum/bounds records; the two absent
legacy/development references are `storm_sentinel_beam` and
`fp_sniper_rifle`. The exact 39-entry values are pinned in
`src/common/halo4_world_collision_logic.h`. No retail address, inferred tag
pointer, or cross-title dimension is used for these values.

Stage 6 selects a bounds record with the runtime checksum already read by the
proven render-model identity resolver. It transforms eight box corners and six
face centers by the carried model root, then publishes those fourteen fixed
weapon samples after seven fixed-semantic hand samples. A checksum `shapeId`
forces one safe reseed when the held model changes even though sample count is
constant. Coincident hand extrema retain their min/max slots instead of
changing publication size during animation. An unknown checksum or malformed
transform publishes the accepted right-hand volume alone and increments a
cold-reported fallback counter. Palette/render hot paths remain bounded,
allocation-free, lock-free, log-free, file-I/O-free, and physics-call-free.
The ray cadence, contact skin, shared carrier correction, haptics, object
motion, toggle default, camera, HUD, reticle, helmet, effects, and all other
titles are unchanged. Physical melee and damage remain explicitly disabled.

### Stage 6 headset acceptance (2026-09-03)

The user's Steam / SteamVR-Oculus / 120 Hz run loaded exact source
`58eaed451b86f870ece67be76423ba48f78f0862`. The supplied log has SHA-256
`29A71E488FE2299812AC19147729536305728D379022809210B383C403E1DBF9`.
The user explicitly confirmed that held guns now react to the environment and
that the hand/weapon collision, shared toggle, haptics, and dynamic-object
interaction are a good baseline.

Across 84 telemetry windows the log totals 88,142 authored probes, 139/2
left/right hand contacts, 304 weapon contacts, 322/649 applied left/right
corrections, 67,696 authored-bounds publications, zero hand-only fallbacks, 42
state resets, and 68/68 completed object pushes with zero push or ray failures.
Physical melee and damage remained disabled as intended. Stage 6 is therefore
the protected accepted collision baseline; a physical-melee experiment must be
an independently gated layer and must not change its geometry, filter, cadence,
contact skin, correction, haptic, or object-motion behavior.

## Stage 7 physical-melee evidence and candidate

Halo 3 has no motion-threshold melee feature to copy, so Stage 7 is not a
cross-title parity claim. The Halo 3 experience being preserved is narrower and
important: after the virtual controller requests melee, Halo itself owns target
selection, the weapon's authored melee damage, animation, sound, impulse, kill
credit, and networking. Stage 7 preserves that boundary for Halo 4 and does not
construct or write damage data.

The official H4EK 1.890 `halo4_tag_test.exe` (SHA-256
`B7468DB9FD160B035C329540EE0B0D47BCF609E1BA6E85AE4F204B70661113A6`)
contains the console command `magic_melee_attack` at string RVA `0x197C3F8`
with the description "causes player's unit to start a melee attack". Its
registration at H4EK RVA `0x2A64A9` names callback RVA `0xF6B390`. That callback
finds an active input user, resolves its player/unit, zero-initializes a 0x48
request whose first dword is action type `0x31`, and submits it through the
central unit request dispatcher at H4EK RVA `0xF3EF60`. Dispatcher table entry
`0x31` resolves through the official image to handler RVA `0xF9CC00`. These are
editing-kit facts explaining that melee is a native unit action, not evidence
for calling those kit addresses in retail.

Calling that dispatcher from the borrowed physics-ray callback would introduce
an unproved re-entrant simulation mutation, and calling it from XInput polling
would introduce an unproved thread contract. Stage 7 therefore uses no new
retail binding or engine call. The accepted headset configuration already
requests ordinary melee through the virtual pad's B route; the supplied Stage
6 log records the corresponding manual `controller edge: B` inputs during the
user's melee/ragdoll test. A qualified physical swing emits a 120 ms B pulse
through that same existing XInput merge, after which Halo 4 performs its normal
native melee transaction. This intentionally follows the user's tested control
mapping; it does not guess another button or bypass MCC's mapping.

Qualification reuses the accepted Stage 6 authored hand and exact weapon-bound
samples without changing their ray filter, cadence, skin, correction, haptics,
or object push. Speed is computed in metres per second from the prior raw
authored target to the current raw target, never from the collision-corrected
accepted point. Consequently holding a hand or barrel against a wall cannot
create repeated artificial swing velocity. A contact qualifies only when the
engine ray result carries a non-sentinel object index other than the ignored
player and its raw sample speed meets the default 1.20 m/s threshold. Static
world contact therefore never attacks; living units, active ragdolls, and
dynamic body fragments remain eligible through the same engine-owned object
identity already used by the accepted object-motion path. One 600 ms cooldown
prevents a single impact from retriggering every collision tick.

The shared `physical_melee` option defaults off, is displayed only when World
collision is enabled, and reveals a 0.30-3.00 m/s threshold slider only when it
is itself enabled. Halo 4 mirrors both settings into atomics on the cold title
worker. Disabling either option clears pending pulses; title teardown clears
them too. Input consumption additionally requires the current Halo 4 camera
generation to remain armed and the accepted collision transaction to remain
installed. Failure or non-qualification leaves native controls and Stage 6
collision untouched. Telemetry separately reports qualifying dynamic contacts,
emitted native-input pulses, cooldown suppressions, and peak qualifying speed,
so a headset result can distinguish threshold/filter failure from a downstream
control-mapping problem.

### Stage 7 headset rejection (2026-09-03)

The user's Steam / SteamVR-Oculus / 120 Hz run loaded exact source
`7cd745338cdbe0f0b9268250cee0f721bd3e9567`. The supplied log has SHA-256
`907E772D27897846B65A9C472E8CC279AFDBCB64F2BAE0192FEBB4A68945F466`.
The menu toggle and slider worked, but the user's hand passed through living
enemies and no physical melee occurred.

The telemetry makes the failure boundary conclusive. Across 37 windows, 35
with physical melee enabled, the user lowered the threshold from 1.20 through
0.61 to the minimum 0.30 m/s. The system ran 37,294 authored-volume probes and
reported 57 weapon/world contacts, but zero hand contacts, zero object-push
attempts, zero qualifying dynamic contacts, zero native-input pulses, and zero
cooldown suppressions. Consequently the B mapping and native melee action were
never reached. The accepted clear-line filter did exactly what Stage 6 needs
for scenery, but did not admit the living enemy body as a dynamic result for
this purpose. Lowering the threshold or changing the output button cannot fix
an absent target identity.

Stage 7 is compiled dormant before a successor. Its menu/config storage and
implementation remain inert for evidence, while the accepted Stage 6 world
collision transaction stays live. A successor must obtain a unit through Halo
4's own melee target/acquisition path or another official H4EK-proven unit
query; it must not guess collision-filter bits or write damage directly from
the scenery ray callback.

## Stage 8 OpenXR-velocity native-melee candidate

Stage 8 keeps Stage 7 disabled and removes the false requirement that the
scenery clear-line ray identify an enemy. It follows the source-level pattern
in LivingFray's HaloCEVR repository at commit
`66b511c0eb7ced20882de76aeab3e514df05bb1d`: `UpdateMelee()` reads the VR
runtime's controller velocity for each hand, converts it to metres per second,
and writes the game's ordinary `Controls.Melee` field when the configured
threshold is exceeded. Its shipped threshold is 2.5 m/s and its README states
that either controller can initiate melee by swinging. HaloCEVR is a different
engine and none of its addresses, structures, axes, or constants are copied;
the transferable design fact is that motion selects the native melee control,
while the game remains responsible for target proximity and impact.

The Khronos OpenXR 1.0 contract supplies the equivalent runtime measurement.
An `XrSpaceVelocity` chained to `XrSpaceLocation` during `xrLocateSpace`
returns `linearVelocity` in metres per second, relative to and expressed in the
chosen base space, only when `XR_SPACE_VELOCITY_LINEAR_VALID_BIT` is set. Stage
8 chains one structure to each already-existing controller locate and retains
the value only when the pose, velocity flag, and all three components are
valid and finite. No extra locate, action sync, allocation, file access, or
engine call is introduced. The published velocity lives under the same short
controller-state critical section as the corresponding pose and expires after
100 ms, so a retained tracking sample cannot fire across a pause or level
transition.

The input merge reads each valid tracking-space velocity and uses its 3D
magnitude, allowing natural horizontal, vertical, or diagonal swings. Because
the runtime measurement is relative to OpenXR local space, artificial Halo
locomotion cannot masquerade as a hand swing. A rising crossing of the shared
threshold emits the existing 120 ms B pulse; a latch rearms only below 55% of
the threshold and the retained 600 ms cooldown prevents two hands or runtime
jitter from duplicating one attack. Unlike Stage 7, no collision result or
object identity is required. Halo 4's native melee action determines whether a
living enemy or ragdoll is within its authored range and then owns damage,
stagger, animation, impact effects, sound, impulse, attribution, and network
behavior. Swinging in empty space may play the ordinary miss animation, which
is the same native behavior as pressing melee.

The feature remains nested under the default-off World collision and Physical
melee controls and retains the adjustable 0.30-3.00 m/s range with a 1.20 m/s
default. It does not require the optional world-ray hook to remain installed;
world collision and physical melee now fail independently. Teardown or either
toggle turning off clears the pulse and both velocity latches. Cold telemetry
reports valid OpenXR velocity samples, threshold crossings, native-input
pulses, cooldown suppressions, and peak runtime speed. A successful headset
test therefore requires nonzero velocity samples and crossings, the visible
native melee animation, and normal Halo 4 damage/stagger when an enemy is in
range.
