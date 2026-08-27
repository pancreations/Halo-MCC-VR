Stage 3AL headset candidate

Baseline: accepted Stage 3AK.

Change: ODST teardown lifetime only. The exact halo3odst.dll mapping is now pinned only while the
private ODST camera core owns MinHook/native patch addresses, then released after verified cleanup.
This targets the menu-stutter/input-loss retry storm seen at the end of the Stage 3AK validation log.

UNCHANGED: Halo 2 Classic muzzle suppression, Halo 2 Anniversary, Halo 4 C50 effect fix, Reach,
Halo 3, HUDs, reticles, pause behavior, hands/two-hand, gun-stock, OpenXR and configuration.

Reach pause/re-entry is intentionally not modified in this pass; the log shows native Reach pause
exits themselves restoring stereo promptly, while the longer wait belongs to the existing level-load
safety proof after a title/loading transition.
