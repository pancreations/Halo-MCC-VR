# Halo MCC VR agent contract

Every coding agent working in this repository must read `CLAUDE.md` and
`docs/CURRENT-STATE.md` before changing code. `docs/CURRENT-STATE.md` is the
authoritative evidence and headset-result ledger.

## Halo 3 parity foundation

Halo 3's headset-confirmed behavior is the player-facing reference for every
additional MCC title. When the user asks for a feature in ODST or another title,
"the same as Halo 3" means the end-user behavior must match Halo 3 across input,
camera ownership, stereo presentation, transitions, controls, HUD, weapons,
comfort, configuration, and lifecycle recovery.

- Reuse the proven shared Halo 3 behavior path whenever possible. Put only the
  engine-specific evidence, signatures, layouts, and calibration in the title
  adapter.
- Do not substitute a merely similar implementation, simplify the behavior, or
  silently choose a different control model.
- Never copy a Halo 3 offset, signature, structure member, bone, marker, or tag
  meaning into another title without title-specific evidence. Player-facing
  parity does not waive engine-safety proof.
- Before implementing a title feature, state the exact Halo 3 behavior being
  matched and compare the target title's current routing against it.
- Any unavoidable difference must be documented as a limitation and explicitly
  approved by the user. Do not describe an untested approximation as parity.
- Definition of done includes a headset test of the target behavior plus a Halo
  3 regression check whenever shared code or cross-title state can be affected.

This contract is non-negotiable unless the user explicitly changes the product
direction.
## Render-pipeline parity

Halo 3's frame lifecycle is the project-wide implementation assumption: arm at
the first eligible fresh camera boundary after the same one-second safety
interval, then render world, first-person/weapon, native CHUD, and capture for
each eye as one transaction. Target adapters may locate equivalent engine
stages with title-specific evidence, but must not add latency or replace native
CHUD with a panel/copy path without explicit user approval. Apply this rule to
every feature, investigation, and fix.