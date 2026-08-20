# MCC editing-kit evidence policy

The official editing kits are essential reverse-engineering evidence for every
HaloMCCVR title adapter. They are not optional references and are never treated
as interchangeable just because two games use related engines.

## Installed evidence sources

| Title adapter | Required kit | Local installation |
|---|---|---|
| Halo: CE Anniversary/classic | HCEEK | `N:\SteamLibrary\steamapps\common\HCEEK` |
| Halo 2 Anniversary/classic | H2EK | `N:\SteamLibrary\steamapps\common\H2EK` |
| Halo 3 | H3EK | `N:\SteamLibrary\steamapps\common\H3EK` |
| Halo 3: ODST | H3ODSTEK | `N:\SteamLibrary\steamapps\common\H3ODSTEK` |
| Halo: Reach | HREK | `N:\SteamLibrary\steamapps\common\HREK` |
| Halo 4 | H4EK | `N:\SteamLibrary\steamapps\common\H4EK` |

## Required use

For each title, use its own kit to establish tag schemas, render models, bones,
markers, weapon behavior, HUD classes, seats/mounts, and controlled Sapien or
tag-tool experiments. Record the evidence in a title-specific reverse-
engineering note and signature manifest before adding a runtime hook.

An offset, byte signature, bone index, marker, or tag interpretation proven for
Halo 3 is evidence only for Halo 3. It must not be copied into ODST or another
MCC title without independent validation against that title's editing kit,
installed module, and a disposable runtime probe. Copyrighted kit/game files
must never be committed, packaged, uploaded, or used as CI fixtures.

## Halo 4 mod-tools-only rule (2026-08-06)

New Halo 4 feature evidence must come from the official H4EK executables,
symbols/source names, assert text, tag schemas (ManagedBlam/Corinth — H4EK
has no guerilla.exe; Foundation.exe is the tag editor), and `tool.exe`
exports. The kit corpus is pinned in `docs/HALO4-EVIDENCE-MANIFEST.json`
(`H4EK.7z` hashed before extraction).

The loaded `halo4.dll` may be used only to match and verify an H4EK-derived
identity, with unique-match, executable-range, boundary, ABI, and layout
checks — never to discover a binding. The Reach script-table retail-derivation
chain is proven for Reach only and is inadmissible for Halo 4 until re-proved
on two harmless Halo 4 script functions. Halo 3, ODST, and Reach facts are
not Halo 4 evidence. Halo 4 has no CHUD tag classes at all
(`docs/HALO4-CUI-EVIDENCE.md`); no CHUD-derived construct may be assumed.

## Halo 2 mod-tools-first rule (2026-08-19)

New Halo 2 bindings begin with the official H2EK executables, retained source
paths/asserts, and tags. Retail `halo2.dll` is only the cross-architecture match
and uniqueness target. The pinned kit/retail identities, game-time lifecycle,
render transaction, camera layout, and native asymmetric-frustum path are in
`docs/HALO2-SIGNATURE-EVIDENCE.md` and
`docs/HALO2-EVIDENCE-MANIFEST.json`.

`groundhog.dll` (H2A multiplayer) is a separate scope. A `halo2.dll` offset or
layout is not evidence for it, and it must not be added as a second descriptor
aliasing the existing Halo 2 runtime slot.

## Reach mod-tools-only rule (2026-07-26)

New Halo: Reach feature evidence must come from the official HREK executables,
symbols/source names, tag schemas, and `tool.exe` exports. Reclaimer and archived
retail/console-derived repository notes are quarantined historical context: they
may explain why an old experiment existed, but they cannot select a new hook,
field, class, marker, constant, or runtime behavior.

The loaded MCC title module may be used only as the runtime match target for an
HREK-derived identity, with unique-match, executable-range, boundary, ABI, and
layout checks. It is not a source from which to infer a new Reach binding. A
missing or mismatched HREK identity rejects the complete affected transaction;
it never authorizes a retail-derived substitute, copied cross-title offset,
widget-name heuristic, or procedural approximation.
