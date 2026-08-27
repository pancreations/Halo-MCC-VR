# Stage 3Q — General Cross-Title Re-entry

Base: accepted Stage 3P (`b5eeec1276e91197bade35b926d9f176437e0096a40b29a137338e4f2e22abe2`).

## User-visible goal

Fix MCC title switching when several game DLLs stay resident, so returning to a
previously visited supported title can begin its normal VR bring-up again. The
reported reproduction was Reach -> Halo 3 -> Reach, where the second Reach
entry remained flat. The fix is deliberately general for Halo 3, ODST, Reach,
Halo 4, and Halo 2 rather than a Reach-only exception.

## Root cause

`TitleAdapter_PollLoaded` used module residency as its raw selector. With more
than one game DLL resident it returned `Unknown` and waited for a title
heartbeat. But an unhooked/re-entered title could not publish the heartbeat
until it had first been selected and allowed through its normal liveness gate.
That made multi-resident re-entry a catch-22.

This is the exact limitation documented previously in
`docs/TITLE-RUNTIME-OWNERSHIP.md`: entry into an unhooked new title while an old
title DLL remained resident was intentionally not solved at that time.

## Stage 3Q selector

The new selector is intentionally weaker than runtime ownership. It only
chooses which adapter may run its existing cold/liveness proof. It never grants
stereo, aim, HUD, IK, haptics, room-scale, or runtime-mode capabilities.
`ResolveTitleRuntime` remains the final generation/lifecycle/heartbeat owner.

Priority:

1. **Fresh Present caller hint (250 ms).** The process-wide DXGI Present hooks
   classify the original caller by `VirtualQuery(...).AllocationBase`. A caller
   inside a supported resident title DLL is a strong short-lived hint.
2. **Read-only liveness fallback.** Under module ambiguity, sample only the
   exact title-owned records that are already pinned by the normal load-safety
   gates:
   - Halo 3 player-view array: `+0x02D2F680`, stride `0x2820`
   - ODST: `+0x02D73590`, stride `0x2810`
   - Reach: `+0x029F2B90`, stride `0x0A40`
   - Halo 4: `+0x030AD1C0`, stride `0x0AD0`
   - Halo 2: proven game-time singleton slot `+0x015FE008`

   A still/frozen sample followed by a change is immediately eligible. If the
   probe begins in an already-running level, 120 changing samples (about six
   seconds at the worker's 50 ms cadence) matches the existing conservative
   load-gate rule. Exactly one fresh new candidate can preempt the current raw
   adapter; multiple changing titles do not pick a new winner.
3. **Sticky raw-adapter retention.** If no different title uniquely proves
   itself on that sample, retain the already-selected supported title while its
   DLL remains resident. This prevents the raw selector from flapping to
   `Unknown` during a pause/loading/static camera frame. This does *not* retain
   runtime ownership: stale heartbeat/lifecycle state still removes all VR
   capabilities independently.

The Present hint timestamp is published last (and cleared first) so a worker
cannot pair a newly written title byte with an old title's still-fresh stamp.

## Binary implementation

Linux cannot perform the full MSVC/Windows-SDK link in this environment, so
Stage 3Q uses the same guarded post-link technique as the accepted Stage 3N,
3O, and 3P candidates.

Only three existing code sites are patched:

- `0x87FF1`: the 21-byte `detectedCount > 1 => Unknown` value-selection block
  redirects to the general selector.
- `0xDF60`: `PresentHook` entry redirects through the caller-classification
  trampoline, then replays its overwritten nonvolatile saves.
- `0xE060`: same for `Present1Hook`.

New code starts in the existing executable `.s3ic` section at RVA `0x2F2200`.
Runtime probe state is isolated in a new RW initialized-data section `.s3qd` at
RVA `0x2F3000`. The executable helper section remains non-writable.

## Explicitly protected Stage 3P behavior

The Stage 3Q builder snapshots and re-verifies all accepted title-specific
patches, including:

- Reach HUD high-first private+mapped locator and six-record completion.
- Halo 3 stale-Reach-cleanup stability gates.
- Halo 2 Y+B pause presentation title scoping and clean stereo restoration.
- Halo 2 Classic carrier + yaw/pitch calibration.
- Halo 3/ODST/Reach/Halo 4 gun stock calibration.
- All five protected Halo 4 rollback spans.
- `@MeWhenINameMyself` menu attribution.

H2 Classic muzzle flash is unchanged.


## Stage 3Q-R1 loader-layout correction

The first packaged Stage 3Q candidate placed `.s3qd` at RVA `0x2F4000`, leaving an unnecessary 0x1000 virtual gap after the expanded `.s3ic` section. Windows rejected that image at load time with `0xC000007B` before `DllMain`/mod initialization. R1 places `.s3qd` at the immediately adjacent aligned RVA `0x2F3000`, making `SizeOfImage` `0x2F4000`. The cross-title re-entry logic itself is unchanged. The audit now explicitly verifies virtual section adjacency so this PE-layout regression cannot silently pass again.
