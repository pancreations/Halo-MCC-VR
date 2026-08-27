# Stage 3M Windows build handoff

This tree is intended for a **normal full Windows/MSVC rebuild** of HaloMCCVR.
Use the project's existing Visual Studio 2022 / Windows SDK / FXC toolchain and
its normal x64 Release configuration described in `BUILDING.md`.

Do **not** run the historical Stage 3H, Stage 3J, Stage 3K, or Stage 3L post-link
builders on the newly compiled DLL. Those scripts are hash-pinned recovery/test
tools for older binaries. Stage 3M already carries the Stage 3L-safe Halo 4
reticle semantics in source.

Required build features are the same as the current 518aebe/C50 line: ODST,
Reach/ReachRender, Halo 4, and Halo 2 stereo/observer support enabled exactly as
in the current project configuration.

## Headset smoke test after building

1. Open F1 > Weapon calibration and confirm `Gun right offset (m)` and
   `Gun up offset (m)` exist, each with a Center button.
2. Test +0.05 m and -0.05 m in each direction on Halo 3, ODST, Reach, Halo 2,
   and Halo 4. The visible right-hand gun should move on its own post-rotation
   right/up axes.
3. Confirm the reticle/bullet ray does not move with these visual offsets.
4. Confirm the left/support hand does not receive these offsets.
5. Return both sliders to 0.00 and confirm prior weapon placement is restored.
6. Halo 2 smoke test: Classic + Anniversary stereo, native crosshair and HUD
   sliders must remain at the accepted Stage 3I behavior.
7. Halo 4 smoke test: it must remain at Stage 3L behavior (stable procedural
   reticle; no native-capture/native-position experiment; muzzle/HUD issues are
   intentionally not addressed in this pass).

No headset acceptance is claimed until this Windows build is tested.
