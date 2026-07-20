HALO MCC VR - ALPHA 0.1
=======================

This is an early test build, not a finished public release. Halo 3 campaign is
the currently tested path. ODST, online play, custom games, Forge, every weapon,
vehicles, scopes, and long play sessions have not all been validated yet.

This mod's code was written by AI - Anthropic's Claude and OpenAI's Codex -
directed by a human modder who is not a programmer. He made the decisions, ran
the builds, and confirmed each result in a headset, but no human reviewed every
line of code. The full source is public so anyone can read it before running it:

  https://github.com/pancreations/Halo-MCC-VR

It is MIT licensed. Use it, learn from it, fork it, take it apart.

WHAT THE TEST LAPTOP NEEDS
--------------------------
- Windows 10 or 11 (64-bit)
- The Steam version of Halo: The Master Chief Collection, with Halo 3 installed
- A working OpenXR headset/runtime (PSVR2 and Quest 3 through SteamVR have been
  used during development; Quest Pro has limited early testing, and other
  headset/controller combinations may not work out of the box)
- Steam and the headset runtime running before launch

No compiler, CMake, source code, or Visual C++ redistributable is required.

REQUIRED MCC SETTINGS
---------------------
Set these three in MCC's own menus. They are not optional and the mod does not
change them for you. You can do it with the headset on, from inside the VR
game - changing them will not crash the mod, and you do not need to launch MCC
without VR first.

  1. Settings > Video > Max Frame Rate  ->  120
  2. Settings > Video > V-Sync          ->  Off
  3. Halo 3 > Settings > Field of View  ->  120

  Do NOT turn on FSR in the MCC video menu. It breaks the VR image scale.
  Use the picture quality setting below instead.

The Field of View one matters most. At the default FOV the game culls (stops
drawing) anything outside the flat-screen view, so in the headset you see
objects and scenery pop in and out at the edges of your vision. FOV 120 pushes
the culling out past what the headset shows and the popping goes away.

Leaving V-Sync on or the frame rate limit at 60 will cap the headset at that
rate and feel bad regardless of how fast your PC is.

AUTOMATIC INSTALL
-----------------
1. Unzip the entire HaloMCCVR-alpha-0.1 folder. Do not run files from inside the
   ZIP.
2. Close MCC if it is running, then double-click install.bat.
3. If MCC is not found automatically, paste the game's main install folder when
   asked. It is the folder containing the "MCC" folder.
4. Pick a picture quality when asked. Low is the safest first try; you can
   change it any time with F1 in game (it applies after a restart).
5. The installer creates a "Halo MCC VR" desktop shortcut.

Installing over an older copy of the mod is fine: run install.bat again and it
updates the mod folder in place, keeping the settings you tuned with F1. Game
files are never modified.

MANUAL INSTALL (NO SCRIPTS)
---------------------------
You do not have to run install.bat or uninstall.bat.

1. In Steam, open MCC's Manage > Browse local files folder.
2. Inside the main "Halo The Master Chief Collection" folder, create a folder
   named exactly "halo3xr".
3. From this unzipped package, copy only halo3xr.dll and
   halo3xr_launcher.exe into that new halo3xr folder.
4. Start Steam and your OpenXR headset runtime, then double-click
   halo3xr_launcher.exe.
5. Optional: right-click halo3xr_launcher.exe and choose
   Send to > Desktop (create shortcut).

The final path must be:

  Halo The Master Chief Collection\halo3xr\halo3xr_launcher.exe

Do not place the files loose in the main MCC folder. To uninstall manually,
close MCC and delete only the dedicated halo3xr folder you created. The mod
does not patch game files.

PICTURE QUALITY
---------------
This is how sharp the game renders in your headset. Higher looks better and
costs more graphics card. It only changes after a full game restart.

  Potato       50%   1456 x 1050   very weak PCs and laptops
  Low          67%   1952 x 1408   safest first try, the most tested setting
  Medium       80%   2330 x 1680   mid-range gaming PC
  High        100%   2912 x 2100   strong graphics card
  Ultra       110%   3204 x 2310   high-end graphics card
  Keith David 150%   4368 x 3150   top-end only. Absurdly sharp.

Only Low has been confirmed in a headset so far. If a higher setting stutters
or will not start, run install.bat again and pick a lower one, or drop it in
the F1 menu and restart the game.

PLAY
----
1. Confirm the three required MCC settings above (frame rate 120, V-Sync off,
   Halo 3 Field of View 120).
2. Start Steam.
3. Connect the headset and start SteamVR (or your active OpenXR runtime).
4. Double-click the "Halo MCC VR" desktop shortcut.
5. Test Halo 3 campaign first.

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
known mod-owned files and its desktop shortcut. It validates that the target is
literally a "halo3xr" folder beneath a real MCC install, never recursively
deletes a directory, and leaves unknown files untouched.

For a no-script uninstall, close MCC and manually delete only the dedicated
"halo3xr" folder. Steam game files elsewhere are not modified by the mod.

SAFETY WARNING: uninstall.bat from packages exported before the 2026-07-20
safety fix must not be used. If an old package was extracted directly into the
main MCC folder, its old uninstaller could mistake that folder for the mod and
delete the game installation. Restore MCC with Steam's file verification, then
use only an updated package.

IF IT FAILS
-----------
Send the tester's description of what appeared in the headset plus these logs:

  <MCC install folder>\halo3xr\halo3xr_launcher.log
  <MCC install folder>\halo3xr\halo3xr.log

Also include the BUILD-INFO.txt from the unzipped test package and say which
headset, GPU, and OpenXR runtime the laptop used.

Quest Pro and additional headset/controller playtesters are specifically
needed. Quest Pro headset timing has an initial positive report, and a reported
skyward-aim problem was the tester's MCC inverted-Y setting. The latest shared
weapon-calibration path still needs confirmation on that headset.
