HALO 3 VR - ALPHA TEST BUILD
============================

This is an early test build, not a finished public release. Halo 3 campaign is
the currently tested path. ODST, online play, custom games, Forge, every weapon,
vehicles, scopes, and long play sessions have not all been validated yet.

WHAT THE TEST LAPTOP NEEDS
--------------------------
- Windows 10 or 11 (64-bit)
- The Steam version of Halo: The Master Chief Collection, with Halo 3 installed
- A working OpenXR headset/runtime (PSVR2 and Quest 3 through SteamVR have been
  used during development; other setups may behave differently)
- Steam and the headset runtime running before launch

No compiler, CMake, source code, or Visual C++ redistributable is required.

INSTALL
-------
1. Unzip the entire Halo3VR-alpha folder. Do not run files from inside the ZIP.
2. Double-click install.bat.
3. If MCC is not found automatically, paste the game's main install folder when
   asked. It is the folder containing the "MCC" folder.
4. The installer creates a "Halo 3 VR" desktop shortcut.

PLAY
----
1. Start Steam.
2. Connect the headset and start SteamVR (or your active OpenXR runtime).
3. Double-click the "Halo 3 VR" desktop shortcut.
4. Test Halo 3 campaign first.

The shortcut launches MCC without Easy Anti-Cheat and loads the VR DLL. Do not
use this build for anti-cheat-enabled matchmaking. Normal MCC launches made
through Steam remain unmodded.

Because this alpha launcher loads a DLL into the anti-cheat-disabled game
process, Windows or antivirus software may scrutinize the unsigned files. Do
not disable security software globally. Only allow the files if you trust the
person who supplied this exact build.

UNINSTALL
---------
Run uninstall.bat from the installed game's "halo3xr" folder. It removes only
the mod folder and its desktop shortcut; it does not remove or patch MCC files.

IF IT FAILS
-----------
Send the tester's description of what appeared in the headset plus these logs:

  <MCC install folder>\halo3xr\halo3xr_launcher.log
  <MCC install folder>\halo3xr\halo3xr.log

Also include the BUILD-INFO.txt from the unzipped test package and say which
headset, GPU, and OpenXR runtime the laptop used.
