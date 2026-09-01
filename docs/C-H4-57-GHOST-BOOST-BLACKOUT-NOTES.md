# C-H4-57 Halo 4 boosted-Ghost blackout correction

Status: **Halo 4 target accepted in headset; cumulative acceptance pending the
required Halo 3 regression**.

C-H4-57 is one isolated Halo-4-only correction above the headset-accepted
C-H4-56 source `271f6dffb8cf2e13dc4feafd85b9b4c61440ff25`. It does not change the
accepted native reticle, adjustable HUD, helmet/visor, pause, first-person
effects, camera, hands, or OpenXR transaction.

## Observed failure

In mission `m30_cryptum` / Forerunner, Cortana supercharges the player's
Ghost. Using boost paints the headset view black. The supplied Steam,
SteamVR/OpenXR 2.17.7, Oculus, 120 Hz C-H4-56 log proves that stereo geometry
and OpenXR continue normally during the report: focused session, completed
current-eye pairs, geometry `TAKING`, and zero recoverable XR drops. The fault
is therefore inside the submitted scene image.

## Evidence-backed change

The official H4EK mission tag selects an opaque `motion_suck` screen material
only for the radial suction/blur half of the scripted boost. The official
shader source makes seven scene-history samples. Its complete PC DXBC blob is
byte-identical in the pinned retail mission map and hashes to
`0x47668A1953271934` under the project's established shader identity function.
The full evidence chain and artifact hashes are recorded in
`docs/HALO4-SIGNATURE-EVIDENCE.md`.

C-H4-57 suppresses only a bind of that exact shader, and only while Halo 4 is
the active title with VR stereo enabled. The separate authored speed-line and
tint presentation remains native. This narrow identity boundary also covers
other Halo 4 black flickers only if they are produced by the same proven
shader; no blanket post-process suppression is introduced.

If the optional shader hook or identity is unavailable, the screen effect
stays stock and every working VR feature remains armed. Runtime telemetry
reports `screen-fx=exact-bridge-LIVE`, the number of exact shaders registered,
and suppressed binds per interval.

## Required headset test

1. Load Halo 4 mission Forerunner at the supercharged-Ghost escape and boost
   repeatedly. The view must remain visible while the native corner speed
   lines/tint remain present.
2. Confirm the status line reports one registered `motion-suck` shader and
   non-zero suppressed binds during boost.
3. Confirm the native reticle, adjustable HUD, visible-by-default helmet visor
   and its toggle, pause/resume, controller aim, hands, and weapon effects have
   not regressed.
4. Briefly exercise Halo 3 gameplay and confirm stereo, HUD/reticle, head
   tracking, hands, and weapons remain normal. This is required because the
   optional implementation shares the D3D pixel-shader hook even though its
   runtime gate is Halo-4-only.

Do not advance `docs/CURRENT-STATE.md` until both the target Halo 4 result and
the Halo 3 regression result are reported from the headset.

## Headset result (2026-09-01)

The user explicitly reported that C-H4-57 fixed the supercharged-Ghost boost
black screen. The supplied log identifies source
`bccc14f2e2d6fcc38d69dde02f7dc538672277da`, Steam edition,
SteamVR/OpenXR 2.17.7, Oculus headset, 120 Hz. This accepts the Halo 4 target
behavior only; no Halo 3 gameplay regression result was supplied, so C-H4-56
remains the authoritative cumulative accepted pointer.

The ordinary Ghost boost trail was not globally removed. C-H4-57 gates only
the exact `motion_suck` pixel shader hash `0x47668A1953271934`. H4EK's
mission-specific `non_suck` speed-line/tint material and the ordinary Storm
Ghost boost effect path do not use that shader and remain untouched. This is a
code/tag-path verification, not a second visual headset claim.
