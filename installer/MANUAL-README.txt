HALO MCC VR - ALPHA 0.3.5 MANUAL SETUP
=======================================

Halo MCC VR is a native OpenXR mod for Halo: The Master Chief Collection.
It supports the Steam and Microsoft Store / Xbox app editions.

SUPPORTED GAMES
---------------
Halo 3:                 Full stereo, 6DOF, motion controls, hands/weapons,
                        HUD, cutscenes, and first-person vehicles.
Halo 3: ODST:           Full stereo, 6DOF, motion controls, hands/weapons,
                        HUD, cutscenes, and first-person vehicles.
Halo: Reach:            Full stereo, 6DOF, motion controls, hands/weapons,
                        HUD, cutscenes, and first-person vehicles.
Halo 4:                 Stereo, 6DOF, motion controls, floating hands/weapons,
                        HUD, cutscenes, and haptics.
Halo 2 Anniversary:     Anniversary and Classic stereo, 6DOF, motion controls,
                        hands/weapons, and haptics. No VR HUD yet.
Halo: CE Anniversary:   Not supported; remains flat-screen.

This is an alpha pre-release. Start MCC without anti-cheat and never use the
mod in matchmaking.

INSTALL
-------
1. Open MCC's main installation folder - the folder containing the MCC folder.

   Steam:            right-click Halo: The Master Chief Collection and choose
                     Manage > Browse local files.

   Microsoft Store:  in the Xbox app, open ... > Manage > Files > Browse,
                     then open the Content folder next to MicrosoftGame.config.

2. Create a folder named exactly:

   Halo_MCC_VR

3. Copy these three files into it:

   HaloMCCVR.dll
   HaloMCCVRLauncher.exe
   halomccvr.cfg

4. Set SteamVR as the current OpenXR runtime and start SteamVR.

5. Run HaloMCCVRLauncher.exe from the Halo_MCC_VR folder.

On Steam, Steam must also be running. For the Microsoft Store edition, be
signed in to the Xbox app and let the included launcher start MCC. Do not start
MCC first.

The final launcher path should look like one of these:

   Halo The Master Chief Collection\Halo_MCC_VR\HaloMCCVRLauncher.exe
   Halo- The Master Chief Collection\Content\Halo_MCC_VR\HaloMCCVRLauncher.exe

Do not put the mod files loose in MCC's main folder. If upgrading, remove the
obsolete halo3xr.dll and halo3xr_launcher.exe files. Replace all three current
files, including halomccvr.cfg. Back up your old config first if you want to
copy personal settings into the new version.

REQUIRED MCC SETTINGS
---------------------
Video > Max Frame Rate:       120
Video > V-Sync:               Off
Halo 3 > Field of View:       120
ODST > Look Sensitivity:      Maximum
ODST > Look Acceleration:     Off
MCC FSR:                      Off

CONTROLS AND CONFIGURATION
--------------------------
F1:         Open the VR settings menu.
L3 + R3:    Recenter VR space and toggle the settings menu.
Y + B:      Pause in ODST and Reach.

In Reach, left trigger and X are swapped compared with Halo 3 and ODST, so
grenades are on X.

Settings are saved in Halo_MCC_VR\halomccvr.cfg. If performance falls to half
the headset refresh rate, disable motion smoothing / ASW or lower
resolution_scale in the F1 menu.

VEHICLE SEATS
-------------
First-person vehicle positions in Halo 3, ODST, and Reach are saved per seat.
While sitting in the driver, passenger, gunner, or turret position, open
F1 > Vehicles and adjust:

   Seat forward
   Seat height
   Seat left / right

The position saves automatically for that seat.

KNOWN LIMITATIONS
-----------------
- Halo CE is not supported.
- Halo 2 Anniversary does not yet have a VR HUD.
- Halo 4 uses floating hands rather than full arm IK.
- Reach passenger seats do not draw floating hands, although aiming and firing
  work.
- Reach character tags and navpoints can be misplaced in 3D.
- ODST's first captioned opening cutscene can appear black. Skip that scene
  once to continue to the working drop sequence.
- The Microsoft Store edition can pause on its first loading screen for about
  nine seconds. Wait for it to continue.
- Co-op, headset, and long-session coverage is incomplete.

Windows security software may warn about unsigned injection-based VR mods.
Download only from the official GitHub release and allow only the release DLL
and launcher rather than disabling security software globally.

REMOVE
------
Close MCC, then delete only the dedicated Halo_MCC_VR folder. The mod does not
modify original game files.

SUPPORT
-------
https://github.com/pancreations/Halo-MCC-VR/issues

Include your MCC edition, headset, OpenXR runtime, connection method, and
Halo_MCC_VR\HaloMCCVR.log when reporting a problem.
