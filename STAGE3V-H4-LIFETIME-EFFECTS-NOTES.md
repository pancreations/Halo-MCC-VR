# Stage 3V — Halo 4 loader lifetime + narrow local-FP effect fallback

Status: **HEADSET-PENDING candidate built directly from headset-accepted Stage 3U**.

Accepted input DLL SHA-256:
`ca2a939e24594292ee543f6498c780b1609c3dba53aabe07f65cfa720fd6a549`

Stage 3V DLL SHA-256:
`4200862ac38918d5c7c88c24e31e2cf0873e7c93313b8b79438236cd17db885e`

## Why this candidate exists

The accepted Stage 3U run proves Reach -> shell -> Halo 3 -> Reach is fixed and all accepted H2 behavior is intact. The same run then shows Halo 4's camera core can install and arm but lose all owned camera/CUI/Storm work immediately afterward. The Stage 3U H4 install still acquired `halo4.dll` with `GetModuleHandleExW(FROM_ADDRESS | UNCHANGED_REFCOUNT)`, so live MinHook targets had only a non-owning DLL identity. That is the same class of loader-lifetime defect already proven and fixed for Reach, but Stage 3V scopes the solution independently to Halo 4.

## Change 1 — H4 camera-core loader ownership

Only the installed H4 camera-core lifetime changes:

- install takes a real loader reference (`GetModuleHandleExW` flags `0x4` instead of `0x6`);
- any install failure that did not transfer that reference releases it only after hook cleanup;
- successful install transfers the owning HMODULE into the existing `g_halo4Camera.moduleReference`;
- successful teardown captures that HMODULE before the existing state clear, removes the hooks/state, logs the normal completion, then calls `FreeLibrary`;
- any teardown failure retains the pin, so a retry never touches an unmapped `halo4.dll`.

Cold/title detection remains non-owning. PE headers, imports, section geometry, image size, and the Q-R1 loader layout are byte-identical to Stage 3U.

## Change 2 — H4 local first-person weapon-effect fallback

The supplied official H4EK tables prove that muzzle/Promethean visuals are not one universal trigger-marker effect. Promethean weapons use weapon-specific first-person marker families (vents, plates, rails, barrel/source ports, `primary_trigger_muzzle`, etc.). The historical x64dbg Phase 2/3 captures also prove that the firing-specific local/location-0 matrix already changes with the hand before it enters the runtime queue; those captures explicitly did **not** justify adding another guessed re-parent transform.

Therefore Stage 3V does not invent a new effect-space transform. It activates the already-existing, exact-byte-guarded H4 local first-person effect splice and changes only its previously ineffective scale-zero action:

- the existing classifier remains byte-identical;
- world/enemy/environment paths remain outside that classifier;
- the selected local first-person matrix keeps its pointer/orientation valid;
- only translation Z is moved to `+10000.0f`, making that detached local weapon effect invisible rather than risking a bad re-parent.

This is the requested fallback behavior when exact restoration is not runtime-proven.

## Reticle behavior deliberately preserved

Halo 4 stays on the accepted C-H4-50 procedural controller/bullet-ray reticle. The rejected whole-CUI/native-reticle replay experiments remain disabled. Stage 3V wraps the existing active-title query only to opportunistically install the effect splice; the immediately following `cmp al,4` / Halo-4 fail-closed reticle branch is byte-identical.

The all-title gun-stock controls remain **visual gun only**. They do not translate the projectile/crosshair ray.

## Halo 2 deliberately unchanged in this candidate

The supplied H2EK data is valuable, but it proves why a generic trigger-marker redirect would be wrong: for example, the Battle Rifle has a center component on `primary_trigger` plus a long component on three distinct `muzzle_flash` transforms with authored rotations. Stage 3U currently has no H2-specific, H2EK-mapped runtime effect/marker resolver comparable to the proven Reach/H4 boundaries.

Stage 3V therefore changes **zero H2 bytes**. H2 Classic stereo, H2A, HUD/crosshair, authored weapon alignment, Y+B behavior, and the visual gun-stock calibration remain exactly the headset-accepted Stage 3U implementation. A future H2 muzzle pass should begin from an H2EK-derived retail ABI/boundary, then rigidly transfer each real authored marker matrix rather than collapse marker identities.

## Headset test order

1. Launch Halo 4. Confirm it progresses from camera-core install/arm into live stereo/6DOF instead of stalling with zero camera readbacks.
2. Confirm controller aim, hands/two-hand behavior, and the procedural reticle are live and aligned. Zoom/unzoom and switch weapons; no black square or stale native flat reticle should return.
3. Fire UNSC, Covenant and Promethean weapons. The selected detached local first-person muzzle/detail family should be invisible. Enemy/world/explosion/environment effects should remain stock.
4. Save/quit or switch out of H4, then enter H4 again without restarting MCC if convenient. The log should show a clean H4 camera-core removal followed by a later clean install/arm.
5. Smoke-test H2 Classic/H2A, especially the accepted Classic weapon alignment and gun-stock sliders. They should be unchanged.
6. Keep the log from the run. Do not label the H4 effect work accepted until headset behavior confirms the classifier catches the offending visuals without collateral suppression.
