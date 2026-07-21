# Halo MCC VR — Alpha 0.1.3

Alpha 0.1.3 is a focused hotfix for players whose VR aiming crosshair did not
appear. The failure was first reported on Quest 3, but the affected fallback path
was shared by every OpenXR headset.

## Fixed

- Halo's native crosshair now remains visible if the authored floating-reticle
  capture or its OpenXR swapchain cannot start. The mod no longer permits a
  capture failure to leave the player with no aiming reference.
- `kill_reticle = 0` now works as an explicit emergency fallback to Halo's native,
  head-centered crosshair.
- The eye-order gate no longer risks leaving the native fallback in only one eye
  after a successful floating-reticle redirect.

The corrected build was tested in a headset and confirmed working before this
package was created. All Alpha 0.1.2 gameplay, campaign-transition, scope, HUD,
and controller changes remain included.

## Install or update

1. Download and unzip `HaloMCCVR-alpha-0.1.3.zip`.
2. Close MCC completely.
3. Copy `halo3xr.dll` and `halo3xr_launcher.exe` into your existing
   `Halo The Master Chief Collection\Halo_MCC_VR` folder, replacing the old files.
4. Start Steam and your OpenXR runtime, then run `halo3xr_launcher.exe`.

Your `halomccvr.cfg` settings remain in place. If the floating reticle is still
unavailable on a particular runtime, close MCC, set `kill_reticle = 0` in that
file, and relaunch to use Halo's native crosshair while collecting `halo3xr.log`.

Launch only through MCC's official anti-cheat-disabled mode. Do not use the mod in
anti-cheat-enabled matchmaking.

## Headset-confirmed artifact

- `halo3xr.dll` SHA-256:
  `BD5F8FB653163A5788BB6762B09EA929A81658A1267FB10280899F2751441412`
- `halo3xr_launcher.exe` SHA-256:
  `6F44B75CA1669DE224C192F13F71C27671E3ADBD993934E677210BB10AD28D70`
