HALO MCC VR - ALPHA 0.1.3
=========================

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

QUEST USERS: USE STEAM LINK
---------------------------
Testers report the best performance on Quest with Steam Link rather than
Virtual Desktop. If the image stutters or feels behind your head on a Quest,
switch to Steam Link before changing any mod setting - it has done more for
frame pacing than any resolution change.

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

INSTALL
-------
Installing is copying two files. There is no installer script: you place them
yourself, so you can see exactly what was added and remove it just as easily.

1. Unzip the entire HaloMCCVR-alpha-0.1.3 folder. Do not run files from inside the
   ZIP.
2. In Steam, open MCC's Manage > Browse local files folder.
3. Inside the main "Halo The Master Chief Collection" folder, create a folder
   named exactly "Halo_MCC_VR".
4. From this unzipped package, copy only halo3xr.dll and
   halo3xr_launcher.exe into that new Halo_MCC_VR folder.
5. Start Steam and your OpenXR headset runtime, then double-click
   halo3xr_launcher.exe.
6. Optional: right-click halo3xr_launcher.exe and choose
   Send to > Desktop (create shortcut).

The final path must be:

  Halo The Master Chief Collection\Halo_MCC_VR\halo3xr_launcher.exe

Do not place the files loose in the main MCC folder. The mod does not patch game
files.

UPDATING
--------
Close MCC, then copy the two new files over the old ones in Halo_MCC_VR. Your
halomccvr.cfg settings are left alone. If your folder is still named "halo3xr"
from an older build, close MCC and rename the whole folder to "Halo_MCC_VR" -
do not merge two folders. If both names exist, keep the one with the settings
and logs you want and move the other somewhere safe first.

SETTINGS FILE
-------------
Every setting the F1 menu has, and a few it does not, live in a plain text file
you can open in Notepad:

  Halo The Master Chief Collection\Halo_MCC_VR\halomccvr.cfg

It is created for you the first time the mod runs, so you do not have to install
anything to get one. Each setting is listed with a short description, its
default value, and the range it accepts, so you can always put one back the way
it was without reinstalling.

  - Edit it with MCC closed. Press F1 in game for the same settings live.
  - Lost? Close MCC, DELETE the file, and start the game. A fresh one with all
    the defaults is written for you.
  - The F1 menu rewrites the whole file when it saves, so notes you type in
    yourself will disappear. Your values are always kept.
  - A line the mod does not understand is ignored and noted in halo3xr.log. It
    cannot break the mod, and out-of-range numbers are pulled back into range.

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

Those six are only shortcuts. The real setting is resolution_scale in
halomccvr.cfg, and it accepts ANY value from 0.35 to 2.00 - set it to 0.90 and
you get exactly 90%, not "rounded down to 80". The F1 menu has the same free
slider. 1.00 means 2912 x 2100; your number scales both sides together.

YES, YOU CAN GO OVER 100%. Anything above 1.00 is supersampling: the game
renders bigger than the headset needs and the extra detail is squeezed down,
which is the cleanest-looking image you can get. The ceiling is 2.00, which is
5824 x 4200 - noticeably past Keith David's 150%, and it will melt anything but
a top-end card. Values above 2.00 are pulled back to 2.00 rather than accepted,
because nothing renders that today and a typo (20 instead of 2.0) would leave
you with a game that never starts.

  1.25   3640 x 2626   between Ultra and Keith David
  1.75   5096 x 3676   past Keith David
  2.00   5824 x 4200   the ceiling

Only Low has been confirmed in a headset so far. If a higher setting stutters
or will not start, lower resolution_scale in the file, or drop it in the F1
menu, and restart the game.

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
Close MCC completely and delete the "Halo_MCC_VR" folder you created, plus the
desktop shortcut if you made one. That is the whole uninstall. The mod never
patches game files, so nothing else has to be undone.

  Delete:  Halo The Master Chief Collection\Halo_MCC_VR

Delete only that folder. Do not delete the main "Halo The Master Chief
Collection" folder - that is the game itself.

SAFETY WARNING: earlier packages shipped an uninstall.bat script. It has been
removed from the project. If you still have an old one, delete it rather than
run it: a version from before 2026-07-20 could mistake the main MCC folder for
the mod folder and delete your game installation. If that already happened,
restore MCC with Steam's file verification.

IF IT FAILS
-----------
Send the tester's description of what appeared in the headset plus these logs:

  <MCC install folder>\Halo_MCC_VR\halo3xr_launcher.log
  <MCC install folder>\Halo_MCC_VR\halo3xr.log

Also include the BUILD-INFO.txt from the unzipped test package and say which
headset, GPU, and OpenXR runtime the laptop used.

Quest Pro and additional headset/controller playtesters are specifically
needed. Quest Pro headset timing has an initial positive report, and a reported
skyward-aim problem was the tester's MCC inverted-Y setting. The latest shared
weapon-calibration path still needs confirmation on that headset.
