# Halo 4 world-contact evidence

## Scope and parity statement

Halo 3 has no world-contact implementation in this repository. There is no
accepted player-experience reference to port and no cross-title address,
layout, flag, or physics behavior is reused. This document supports the first
Halo-4-only experiment: point-sweep the final visible wrist positions against
fixed structure, keep the held weapon rigidly attached to the corrected right
hand, and provide gentle per-hand OpenXR feedback. Dynamic objects, ragdolls,
physics impulses, and physical melee remain explicitly outside this candidate.

The feature is optional. Missing proof or any runtime query-contract failure
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

The bit values are title-specific findings, not enum-order guesses:

- H4EK `object_test_vector` at RVA `0x4ECBD0` directly evaluates
  `(collision_flags & 1) == 0` immediately before the embedded assertion
  `TEST_FLAG(flags.collision_flags, _collision_test_structure_bit)`. Structure
  is therefore bit 0.
- H4EK filter callback RVA `0x4ED700` passes immediate `0x1B` to the engine's
  checked bit-test helper for `_collision_test_fixed_objects_only`. Fixed-only
  is therefore bit 27.
- Multiple ordinary H4EK callers pass profile/category `0x1A`. Stage 1 uses
  that observed value and flags `(1 << 0) | (1 << 27)`. All ignored-object and
  optional-result fields remain zero.

The official assertion also states that a query during object movement is
allowed only when it is not on the main thread or the fixed-only flag is set.
Stage 1 satisfies both conservative conditions: it runs from the existing cold
title worker and sets fixed-only. It never queries from a render/palette hook.

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

## Runtime transaction

- The render thread publishes the pair-frozen final left/right target
  positions through bounded atomic sequence records. It never calls physics,
  logs, allocates, locks, or performs file I/O.
- The existing 50 ms title worker sweeps from the last accepted wrist point to
  the newest target. The first sample and any movement over 1.5 metres reseed
  rather than sweeping across a teleport/recenter.
- A hit stops 1.5 cm before the reported fraction. The worker publishes only a
  finite, bounded translation correction. No orientation or engine state is
  written.
- Render accepts only a same-generation correction no older than 150 ms whose
  source target remains within 15 cm and whose magnitude is at most 75 cm.
  Otherwise it uses the unmodified existing target.
- A contact raises a `0.18` per-hand peak. The OpenXR frame path merges that
  peak with existing game rumble by maximum and applies the configured global
  haptic intensity. No engine worker calls OpenXR.
- Engine-query exceptions or invalid result contracts disable this optional
  feature and emit one explicit fail-open log. Two-second telemetry reports
  query, contact, visible-correction, reset, and failure counts.

## Acceptance and limitations

### Stage 1 headset rejection (2026-09-03)

The user's Steam / SteamVR-Oculus / 120 Hz headset run loaded exact source
`6b301ad681f35765568cdbcb86743de49840327f`. The binding installed and the
worker completed approximately 1,300 queries while both floating-hand paths
continued publishing normally. Every two-second interval reported zero left
and right contacts, zero visible corrections, zero failures, and therefore no
contact haptics. The user confirmed that hands still passed through tested
objects with no visible or tactile response.

This rejects the Stage 1 behavior. It proves that deployment, target
publication, worker scheduling, and the non-throwing retail call edge were
live; it does **not** prove that the selected flag set described a useful
world-geometry query. The claim above that structure bit 0 plus fixed-only bit
27 was sufficient is now only a disproven lead. The Stage 1 install is compiled
dormant until the official H4EK input/filter contract is re-established. Do
not use this candidate as a base for another behavioral collision experiment.

Build/tests validate math, layout, and integration but not headset behavior.
Acceptance requires a Halo 4 headset run with the log line
`experimental fixed-world contact: LIVE`, non-zero query/correction counts when
touching level geometry, correct left/right gentle feedback, and no regression
to existing Halo 4 camera, hand/weapon alignment, HUD, native reticle, helmet,
effects, pause, or black-screen fixes. `docs/CURRENT-STATE.md` must not advance
until the user reports that result.

This is a wrist-point sweep, not hand-volume collision. It cannot yet push a
ragdoll and cannot deal melee damage. Those require separately proven Halo 4
dynamic-object/impulse and damage transactions on an engine-safe thread; they
must not be inferred from the fixed-structure query established here.
