# Session handoff

Read `docs/CURRENT-STATE.md` first. It is the authority for recovery commits,
headset-confirmed behavior, failure history, and safety rules. `docs/PLAN.md`
contains the active order of work; `docs/RE-notes.md` contains verified engine
facts only.

## Current direction (2026-07-21)

The user considers Halo 3 to be in a great state and approved beginning the
Halo 3: ODST port. The frozen Halo 3 alpha line is `v0.1.3-alpha` at `6f8236b`;
its runtime source is the headset-confirmed crosshair-fallback checkpoint
`bb4bb6f`, preserved at `recovery/best-working-20260721-crosshair` (`c58db2e`).
ODST work must start on its own named branch and must not destabilize or broadly
refactor the proven Halo 3 path.

A user-authored `README.md` edit predates ODST work. Preserve it (including if
it has been placed in a named stash) and do not include it accidentally in ODST
commits.

ODST remains registered with `runtimeSupported=false`, and game-hook dispatch
remains gated on `GameTitle::Halo3`. This is the required safe shipping state
until the ODST adapter passes its own headset acceptance gates.

The ODST signature and camera/view layout evidence gates are now complete. Read
`docs/ODST-SIGNATURE-EVIDENCE.md`, `docs/ODST-CAMERA-LAYOUT.md`, and the exact
next-step contract in `docs/ODST-MINIMAL-BRINGUP-HANDOFF.md`. Read-only stock
captures covered movement, zoom, death/respawn, level unload/reload, cutscenes,
and vehicle entry/exit. The single-user stock camera layout gate passed; no
runtime hook or support flag was changed.

The immediate next checkpoint is a private, build-gated ODST
camera/stereo/6DOF core with atomic all-or-stock fallback and clean
unload/title-exit/reload handling. Keep the experimental option off by default,
keep `runtimeSupported=false`, preserve Halo 3 exactly, and do not port any
controls, reticle, HUD/VISR, pause, scope, weapon, bone, arm, or VRIK behavior in
that checkpoint.

## First checkpoint: universal configuration

The config-only implementation is `148f971` on `feature/odst-bringup`. Release
and CTest pass; no ODST hooks or support flags changed. It was deployed with
byte-matched SHA-256 values `0BD0233C...B11BA` (DLL) and
`BDC0A20F...EA6D` (launcher). The user reported "seem like it" after the Halo 3
headset check. Record that as a positive initial result, not an exhaustive
matrix. The config checkpoint is accepted; the subsequent non-hook signature
and camera-layout gates also passed. The full Halo 3 matrix remains a
merge/release safeguard:

- Keep one `halomccvr.cfg` and one consistent F1 menu for every supported MCC
  title. Never introduce a separate ODST user config.
- Keep all existing keys and values backward compatible. Saving may reorganize
  the generated file under clear OpenXR/comfort, controls, reticle/aiming,
  weapons/hands, HUD/presentation, performance, and diagnostics headings, but
  it must not reset existing Halo 3 tuning.
- Keep `resolution_scale` readable by the launcher from the same file.
- Treat settings as portable user preferences. Title-specific camera scale,
  weapon mount, skeleton, hand/shoulder, HUD, reticle, brightness, and blur
  calibration belongs in the title adapter. Apply universal user trims on top
  of the active title's verified base calibration.
- Keep menu names and intent consistent. Use title capabilities to disable or
  omit unavailable behavior while preserving its saved value.
- Add migration/round-trip/launcher tests, build Release, run CTest, deploy with
  `deploy.bat auto`, and require a Halo 3 headset regression before ODST runtime
  work begins.

## ODST bring-up order

1. **Complete:** use H3ODSTEK at `N:\SteamLibrary\steamapps\common\H3ODSTEK` and
   `halo3odst.dll` as the primary evidence for ODST. Halo 3 evidence is not
   portable proof.
2. **Complete:** confirm the eight byte-identical signatures land in equivalent functions,
   then re-derive and uniqueness-check the twelve failed production signatures.
3. **Complete for camera/view/first-person:** use read-only scanning and
   narrowly scoped live probes to prove the shifted camera, view, and
   first-person layouts. The camera-object stride is `0x2810` in ODST versus
   `0x2820` in Halo 3. HUD, weapon, bone, and marker layouts remain later gates.
4. **Next:** bring up only camera/stereo/6DOF and clean title exit first. Any missing or
   ambiguous signature must leave ODST stock and running.
5. Add controls/aim/reticle, then ODST weapon/arm/VRIK calibration, then
   HUD/VISR and broader gameplay. Make one evidence-backed behavioral change
   per headset build.
6. Cover scopes, vehicles, turrets, cutscenes, death/respawn, mission
   transitions, long sessions, and performance before enabling public support.
7. Set ODST `runtimeSupported=true` only after ODST acceptance and a complete
   Halo 3 shared-system regression both pass.

## Workflow invariants

- Start clean and checkpoint before risky work.
- Never ship guessed addresses or reuse an unverified Halo 3 offset in ODST.
- Verify every AOB is unique; zero or multiple matches must fail safely.
- Keep file I/O, allocation, locks, COM, and logging bursts out of hot hooks.
- Build Release, deploy only through `deploy.bat auto`, match hashes/build stamp,
  and record the exact headset result before advancing.
- Revert failed experiments instead of retaining dormant switches or fallbacks.
- Never patch game files or interact with Easy Anti-Cheat.
