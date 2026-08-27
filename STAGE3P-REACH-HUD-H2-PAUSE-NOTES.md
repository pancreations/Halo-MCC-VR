# Stage 3P - Reach HUD + Halo 2 pause presentation + attribution

Stage 3P is a surgical post-link candidate built on the accepted-test Stage 3O DLL
(SHA-256 `90798fa17c73e9b3d3fa451480e9c44ab5cda5cb41b3dc912cf4b3ae62a5036c`).
It intentionally leaves the accepted Stage 3O Halo 3 stability gates and the Stage
3N/3O Halo 2 weapon/aim/stereo paths unchanged.

## Reach HUD

Stage 3O incorrectly narrowed Reach SAFEFRAME discovery to `MEM_MAPPED` only. The
previous headset log had already proven all six useful Reach HUD records as
`MEM_PRIVATE`, `PAGE_READWRITE` (`type 0x20000`, protection `0x4`) and showed
`hud_size` / `hud_aspect` writes working after those records were located.

Stage 3P therefore:

- accepts both `MEM_PRIVATE` and `MEM_MAPPED` for the Reach adapter;
- starts the first scan of each Reach title generation at
  `0x00007FF400000000`, the high allocation band containing the proven records;
- stops immediately after the complete six-record proof has been collected;
- if the high first pass misses, the next normal auto-rescan in the same title
  generation falls back to the original full application address range.

Reach curvature remains deliberately unavailable at runtime. The title folds its
curvature grid into a derived basis at tag load; this pass only targets live size
and horizontal/aspect adjustment.

## Halo 2 Y+B pause menu

The Stage 3O test log showed Y+B injecting Start but reporting
`presentation control=native engine flag`. Halo 2 has no independently proven
native pause boolean, so it must use the existing edge-driven pause presentation.

The cause was Halo 3's `g_enginePauseValidated` proof remaining true after MCC kept
`halo3.dll` resident and the active title changed. Stage 3P title-scopes that proof
and the two Halo 3 background pause-reconciliation paths. Halo 2 can therefore
request its already-existing head-locked stock-screen presentation again, while
the existing C-H2-72 stock pause screen and C-H2-73 resume-clock logic are left
unchanged.

No Start-pulse timing, Halo 2 stereo transaction, native HUD shader path, reticle,
weapon carrier, bullet aim, Classic yaw/pitch calibration, or Anniversary path is
changed.

## Menu attribution

The pass-on/community line now reads exactly:

`Pass-on/community build maintained by @MeWhenINameMyself.`

The Pancreations introduction and Flat2VR feedback line remain intact.

## Required headset validation

1. Reach: enter gameplay, open the VR menu, change HUD size and HUD width/aspect.
   The log should find verified SAFEFRAME anchors and log live slot writes. Curvature
   remains disabled.
2. Halo 2: press Y+B. The game should pause and the native pause menu should become
   visible as the head-locked 2D presentation. Press Y+B again and verify stereo,
   HUD and gameplay return normally.
3. Quick Halo 3 sanity check: no recurring gun snap/flicker.
4. Quick Halo 2 sanity check: Classic gun yaw/pitch and Anniversary remain exactly
   as Stage 3O.

Stage 3O remains the rollback base until Stage 3P passes headset validation.
