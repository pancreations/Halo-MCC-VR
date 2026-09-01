# Stage 3BB — Halo 4 native-flat hide with replay impossible

Status: **HEADSET PENDING**.

Stage 3BA proved the camera/lifecycle safety baseline: the user reports all
games working. It also exposed the expected feature-local regression: disabling
the whole Halo 4 CUI hook leaves Halo 4's native reticle face-centred while the
procedural weapon-ray crosshair remains separate.

Stage 3BB restores only the H4EK-proven type-`0x28` reticle transform hook. Both
optional install call sites are live again, but the capture predicate's success
edge is forced directly to the normal pass. Every AY/AZ replay, capture-kind,
differential-shader, and suppression-target patch is restored to the guarded AX
bytes. Therefore each CUI command executes exactly once and no replay can occur.

The normal dispatcher changes only the reticle-specific pushed transform to
move the flat native duplicate offscreen. The known-good procedural weapon-ray
quad remains the visible crosshair. Pause, ODST isolation, and all other title
paths are unchanged.

This fixes the face-centred duplicate but does **not** claim that procedural art
has become Halo 4's authored reticle. Actual-art replacement remains open and
must use a proven single-pass GPU extraction boundary.

- Candidate SHA-256: `10e39cf66862f4e88eba245fc22da750c0817c4684a1af114c466703722a8192`
