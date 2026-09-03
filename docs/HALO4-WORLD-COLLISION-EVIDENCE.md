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
