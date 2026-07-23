HALO MCC VR - MANUAL SETUP
===========================

There is no installer, uninstaller, deploy, or restore script.

INSTALL
-------
1. In Steam, right-click Halo: The Master Chief Collection and choose:
   Manage > Browse local files.

2. In the main game folder, create a folder named exactly:

   Halo_MCC_VR

3. Copy these two release files into it:

   halo3xr.dll
   halo3xr_launcher.exe

4. Make SteamVR the default OpenXR runtime. Start Steam and SteamVR, then run
   halo3xr_launcher.exe.

The final path must be:

   Halo The Master Chief Collection\Halo_MCC_VR\halo3xr_launcher.exe

Do not put the files loose in the main MCC folder. Launch only through the
included launcher and never use the mod in anti-cheat-enabled matchmaking.

UPDATE
------
Close MCC completely. Replace only halo3xr.dll and halo3xr_launcher.exe in the
dedicated Halo_MCC_VR folder. Keep halomccvr.cfg if you want to retain settings.

SETTINGS
--------
The first launch creates halomccvr.cfg beside the DLL and launcher. Every value
has a description, default, and allowed range. Edit it with MCC closed or use
the in-game F1 menu. To restore release defaults, close MCC and delete only
halomccvr.cfg.

Required MCC settings:

   Video Max Frame Rate:            120
   Video V-Sync:                    Off
   Halo 3 Field of View:            120
   ODST Look Sensitivity:           Maximum
   ODST Look Acceleration:          Off
   MCC FSR:                         Off

VERIFY
------
For a published release, compare the DLL, launcher, and ZIP hashes with the
official GitHub release page. For a local build, use CANDIDATE-MANIFEST.json in
that unique package. A local candidate is not headset-accepted merely because
it was built from accepted source.

If two PCs behave differently, first compare their installed hashes and
halomccvr.cfg files, confirm SteamVR is the default OpenXR runtime on both, and
fully close every MCC process before relaunching.

Windows security software may warn on unsigned injection-based VR mods. Download
only from the official GitHub release, verify the hashes, and allow only these
two files rather than disabling security software globally.

REMOVE
------
Close MCC completely, then delete only the dedicated Halo_MCC_VR folder you
created. Never delete the main Halo The Master Chief Collection folder.
