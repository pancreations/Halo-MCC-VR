# Halo MCC VR agent contract

Before changing code, read `CLAUDE.md` and `docs/CURRENT-STATE.md` completely.
`docs/CURRENT-STATE.md` is the authoritative accepted-build pointer. Detailed
reverse-engineering facts live in the evidence documents under `docs/`.

## Baseline discipline

- This is one cumulative multi-title mod. Halo 3 and ODST are not separate
  development lines.
- The public `MCC_VR_ALPHA_0.2.1` release is the current known-good baseline.
  Its runtime source is commit `034c4a68e362b334d7994aa9e694243abf2aade5`.
- Begin each candidate from the newest headset-accepted source recorded in
  `docs/CURRENT-STATE.md`. Do not select an old branch, build directory, backup,
  DLL, or ZIP merely because it exists.
- Give every candidate a unique commit and artifact hash. Untested or failed
  candidates do not advance the accepted pointer.
- Revert a failed behavioral experiment. Do not leave it dormant behind a
  switch or stack it into the next candidate.
- Never bulk-remove or consolidate accepted dormant diagnostic/fallback paths.
  Cleanup commit `42a1276` built and launched, then fatally failed at the first
  level transition. Isolate one understood path per candidate and headset test.
- Never install or launch a build unless the user explicitly asks to test that
  exact candidate. Repository scripts must not write to an MCC installation.

## Halo 3 parity foundation

Halo 3's headset-confirmed player experience is the reference for every other
title: controls, camera ownership, stereo presentation, transitions, HUD,
weapons, comfort, configuration, and lifecycle recovery.

- Reuse shared behavior. Put only verified engine-specific signatures,
  layouts, skeleton facts, and calibration in a title adapter.
- Never copy a Halo 3 offset, structure member, bone, marker, tag meaning, or
  engine constant into another title without title-specific evidence.
- State the exact Halo 3 behavior being matched before implementing a title
  feature.
- Document any unavoidable player-visible difference and obtain explicit user
  approval. An untested approximation is not parity.
- A shared-code or lifecycle change requires a target-title headset result and
  a Halo 3 regression result.

## Render-pipeline parity

The accepted lifecycle arms at the first eligible fresh camera boundary after
the one-second safety interval. Each eye's world, first-person weapon, native
CHUD, and capture work then occur as one transaction. A title adapter may locate
equivalent engine stages with title-specific evidence, but must not add latency,
replace native CHUD with a panel/copy path, or reorder the transaction without
explicit approval.

## Safety

- Use unique AOB signatures and fail open to the stock game on zero or multiple
  matches.
- Never hook `halo3+0x120DF8`.
- Never patch game files on disk or interact with Easy Anti-Cheat.
- Keep logging, file I/O, locks, COM, allocation, and signature scanning out of
  render and palette hot hooks.
- Preserve finite-value, bounds, index, count, and teardown guards.
- Headset observation outranks desktop appearance and theories. Verify the
  installed DLL's SHA-256 separately and match the source/configuration in the
  first log line. Do not call a runtime fix complete until the user tests that
  exact hash in the headset.
- `camscan` is opt-in and has process-memory write modes. Never build or run a
  write mode without explicit user approval for that offline diagnostic.
