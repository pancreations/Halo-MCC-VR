# Stage 3AL — ODST title-exit teardown pin

Baseline: exact headset-accepted Stage 3AK DLL, SHA-256
`4ce06fc1e33040990b4db630748d07688803a7216c74cd9d89484c9403510389`.

## Headset evidence behind this pass

Stage 3AK fixed the requested Halo 2 Classic muzzle flash and preserved the other title behavior.
The same validation run exposed a separate ODST exit failure: after the ODST camera heartbeat was
stale and the title poll had genuinely left ODST, teardown retried approximately every 250 ms with
`ODST VRIK cleanup: weapon-IK branch has unknown bytes` and retained the title-module pointers.
The retry storm continued into the MCC shell/menu while frame time and prediction error degraded.

The root lifetime mismatch is explicit in the Stage 3AK source. `InstallOdstCameraCore` obtained an identity-only `halo3odst.dll` handle with
`GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT` and stored that non-owning handle as if it were the
core lifetime reference. The camera core nevertheless owns MinHook targets and two native byte
patches inside that image until verified teardown. MCC could therefore unmap the image before those
restoration pointers were consumed.

## Stage 3AL fix

ODST now mirrors the already-proven Reach lifetime policy:

1. Keep the existing early identity-only probe unchanged, so failed preflight never pins an image.
2. Only after title/liveness/static preflight and prior-ownership checks succeed, acquire one real
   `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS` loader reference to the exact current mapping.
3. Keep that reference while ODST's hooks and native weapon-IK/crosshair bytes are owned.
4. On successful teardown or install rollback, snapshot the retained HMODULE, run the exact existing
   pointer-clear routine, then `FreeLibrary` that one owned reference exactly once.

No ODST camera, render, IK, HUD, pause, weapon, or gameplay policy is changed. This is lifetime
containment only.

## Reach intentionally unchanged

The validation log's actual Reach `native pause exited` events restored the stereo presentation in
roughly 0.2 seconds. The longer non-VR interval occurred after Reach's camera core was removed during
a title/loading transition and its conservative level-load proof intentionally held reinstall.
Changing that proof in this pass would widen the regression surface for no evidence-backed pause fix,
so Stage 3AL leaves all Reach bytes and logic identical to Stage 3AK.

## Binary containment

The binary candidate is reproduced from the exact Stage 3AK DLL rather than recompiling the lineage:

- Stage 3AK's existing `.s3qd` 0x5000 bytes are byte-for-byte untouched.
- The post-preflight ODST module-reference store is redirected to an appended acquire wrapper; the
  earlier identity-only `GetModuleHandleExW(... flags=6 ...)` is byte-identical to Stage 3AK.
- Two existing ODST release-call rel32 operands now call the appended release wrapper.
- One new 0x1000 code page is appended at RVA `0x2F8000`.
- PE size/checksum metadata is updated accordingly.
- Halo 2 Stage 3AK muzzle suppression, Halo 4 Stage 3AI C50 effect coverage, Reach, Halo 3 and every
  other accepted payload remain byte-identical.

## Expected headset result

Leaving ODST for the MCC shell should produce one normal
`ODST camera teardown complete (title exit); stock renderer owns the title` and should **not** enter a
repeating `ODST VRIK cleanup ... unknown bytes` / `retaining ... for retry` loop. The shell/menu should
remain responsive with normal audio/frame pacing.
