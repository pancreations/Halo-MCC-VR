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

ODST remains registered with `runtimeSupported=false` and capabilities `None`.
In the normal build, `HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP` defaults to `OFF` and
ODST receives no hook plan. This remains the required safe public state until
the ODST adapter passes its own headset acceptance gates.

The ODST signature and camera/view layout evidence gates are complete. Read
`docs/ODST-SIGNATURE-EVIDENCE.md`, `docs/ODST-CAMERA-LAYOUT.md`, and the exact
next-step contract in `docs/ODST-MINIMAL-BRINGUP-HANDOFF.md`. Read-only stock
captures covered movement, zoom, death/respawn, level unload/reload, cutscenes,
and vehicle entry/exit. The single-user stock camera layout gate passed; no
runtime hook or support flag was changed during that historical evidence pass.

The private camera/stereo/6DOF core is now implemented as a desk-side candidate.
An explicit option-ON build performs exact retail-build, ten-signature, camera-
array, and ODST-layout preflight before installing only four hooks: camera copy,
inner view render, FP camera rebuild, and FP driver. Installation, teardown,
stock fallback, and reload rearm are atomic and fail closed. Controls/aim,
reticle, HUD/VISR, scope, pause, brightness, motion blur, weapons, bones, arms,
VRIK, and gameplay patches remain off for ODST. The normal deploy and export
scripts reject any cache that is not explicitly option-OFF.

The worker publishes camera-array readiness only while retaining the ODST module;
the render detour rechecks all four slots immediately before mutation.
Presentation detach request/completion generations are serviced on Present even
when XR startup/session state would otherwise return early, and a completed
detach forces a fresh debounce interval before stereo can re-arm.

The first private candidate from `bccf4c7`
(`533CE571B6AD0E955F1722DFF1341EE77A02184C1705D2616630C577BF34B103`)
failed the initial headset smoke: menu VR controls did not merge, ODST stayed
stock 2D because its camera-readiness gate never installed detours, and Halo 3
performance regressed. MCC was closed and the dedicated restore mode restored
the exact baseline
`0BD0233CD28975CADFCE7E03F9B9CA353CD533CD37D257FDCA362983D00B11BA`.
The sealed backup record and `stash@{0}` remain untouched. The next isolated
checkpoint corrects the private frontend controller policy, relaxes only the
over-strict ODST constructor/publication assumptions with one-shot diagnostics,
and removes the measured Halo 3 reticle/palette hot paths. Both clean OFF/ON
Release builds, CTest suites, and private wrapper unwind entries pass. No
replacement candidate has been deployed. The current `1 / 3.048` positional
conversion remains only a calibration hypothesis.

Title-module activation is polled every 50 ms and is not an atomic transition
signal. When retained modules make ownership `Unknown`, Halo 3 shared gameplay
features require a post-transition Halo 3 camera heartbeat less than 100 ms old;
explicit ODST/private camera-only ownership blocks them.

The user asked on 2026-07-21 to prepare the next chat so they can perform the
private ODST test. The reviewed core is `7c25a1a`. The dedicated
`deploy-odst-private.bat` requires its explicit opt-in token, a clean descendant
of that commit on `feature/odst-bringup`, exact x64 OFF/ON caches, fresh Release
builds/tests, closed MCC/launcher, the evidenced retail ODST hash, exact
installed-baseline backup, byte comparison, and hash reporting. It deploys and
restores only the DLL and never launches MCC. The exact installed baseline
`0BD0233CD28975CADFCE7E03F9B9CA353CD533CD37D257FDCA362983D00B11BA`
must be restored from backup after the test; do not substitute a new OFF build.
The launcher remains untouched. Public deploy/export remain OFF-only. The next
chat may deploy through this private path, report the identities, and let the
user launch for the staged smoke test.

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
4. **Desk implementation complete; headset acceptance pending:** the private
   option-ON build brings up only camera/stereo/6DOF and clean title exit/rearm.
   Any failed build, signature, layout, or runtime invariant leaves ODST stock
   or retains teardown ownership until removal can be proved safe.
5. **Next, only after explicit user approval:** establish the reviewed private
   deployment path and run the narrow camera/6DOF headset checklist. The public
   `deploy.bat`/`export-alpha.bat` paths must remain option-OFF only.
6. After that checkpoint is accepted, add controls/aim/reticle, then ODST
   weapon/arm/VRIK calibration, then HUD/VISR and broader gameplay. Make one
   evidence-backed behavioral change per headset build.
7. Cover scopes, vehicles, turrets, cutscenes, death/respawn, mission
   transitions, long sessions, and performance before enabling public support.
8. Set ODST `runtimeSupported=true` only after ODST acceptance and a complete
   Halo 3 shared-system regression both pass.

## Workflow invariants

- Start clean and checkpoint before risky work.
- Never ship guessed addresses or reuse an unverified Halo 3 offset in ODST.
- Verify every AOB is unique; zero or multiple matches must fail safely.
- Keep file I/O, allocation, locks, COM, and logging bursts out of hot hooks.
- Build Release and run CTest in both option-OFF and explicitly option-ON trees.
  The public `deploy.bat auto` path is option-OFF only and must continue to
  reject private caches. Do not create or run a private deployment path without
  explicit user approval; once approved, match hashes/build stamp and record
  the exact headset result before advancing.
- Revert failed experiments instead of retaining dormant switches or fallbacks.
- Never patch game files or interact with Easy Anti-Cheat.
