HALO MCC VR - QUICK MANUAL SETUP
================================

This is the whole setup. There is no installer script - you copy two files in
yourself, and you remove the mod by deleting the folder you made.
ALPHA-README.txt has the longer version with requirements and known limits.

INSTALL
-------
1. In Steam, right-click Halo: The Master Chief Collection and choose:
   Manage > Browse local files.

2. In the folder that opens, create this folder exactly:

   Halo_MCC_VR

3. Copy these two files from this package into that folder:

   halo3xr.dll
   halo3xr_launcher.exe

   Your final launcher path must be:

   Halo The Master Chief Collection\Halo_MCC_VR\halo3xr_launcher.exe

4. Start Steam and your OpenXR headset runtime, then double-click
   halo3xr_launcher.exe. You can right-click it and choose Send to > Desktop
   (create shortcut) if you want a desktop shortcut.

DO NOT copy the files loose into the main MCC folder. The mod folder must stay
separate so it is easy to remove and never overwrites game files.

IF YOU USED AN OLDER VERSION
----------------------------
If you already have an old halo3xr folder, close MCC and rename the whole folder
to Halo_MCC_VR first. Do not merge two folders. If Halo_MCC_VR already exists,
keep the folder with the settings/logs you want and move the other one somewhere
safe before continuing.

MOD SETTINGS - NO INSTALLER NEEDED
---------------------------------
The first time you launch the mod it writes a plain text settings file next to
the two files you copied:

   Halo The Master Chief Collection\Halo_MCC_VR\halomccvr.cfg

Open it in Notepad with MCC closed. Every setting has a short description, its
default value, and the range it accepts, so you can always put one back without
reinstalling anything. The F1 menu in game edits the same file live.

If you make a mess of it: close MCC, DELETE halomccvr.cfg, and start the game.
A fresh file with all the defaults is written for you.

Two things worth knowing. The F1 menu rewrites the whole file when it saves, so
any notes you type in yourself will vanish (your values are kept). And a line
the mod does not recognize is ignored and noted in halo3xr.log - a typo cannot
break the mod, and an out-of-range number is pulled back into range.

PICTURE QUALITY - PICK YOUR OWN
------------------------------
The setting is resolution_scale in halomccvr.cfg. It takes ANY value from 0.35
to 2.00, so you are not stuck with a fixed preset list. 0.90 means
exactly 90%, not "rounded down to 80". Close MCC completely and relaunch after
changing it.

   0.50   1456 x 1050   very weak PCs and laptops
   0.67   1952 x 1408   safest first try, the most tested setting
   0.80   2330 x 1680   mid-range gaming PC
   1.00   2912 x 2100   full size, strong graphics card
   1.10   3204 x 2310   high-end graphics card
   1.50   4368 x 3150   "Keith David". Absurdly sharp.

YES, YOU CAN GO OVER 100%. Anything above 1.00 supersamples - the game renders
larger than the headset needs and the detail is squeezed down, which is the
cleanest image you can get. The ceiling is 2.00 (5824 x 4200), well past the
1.50 tier, and it will melt anything but a top-end card. A larger number is
pulled back to 2.00 rather than accepted, so a typo cannot leave you with a
game that will not start.

   1.25   3640 x 2626   between the 1.10 and 1.50 tiers
   1.75   5096 x 3676   past 1.50
   2.00   5824 x 4200   the ceiling

MCC SETTINGS
------------
- Video > Max Frame Rate: 120
- Video > V-Sync: Off
- Halo 3 > Settings > Field of View: 120
- Do not enable FSR in MCC's video menu.

IF WINDOWS WARNS ABOUT THE FILES (FALSE ALARM)
----------------------------------------------
Windows or your antivirus may warn about the two files, or quarantine one. They
are safe - almost every VR mod gets this, because the mod loads itself into the
game to turn on VR. If it is blocked, choose Allow (or restore it), and you are
set. You do not need to turn your antivirus off. Because each release is a new
file, the warning may come back after an update; that is normal. Just get your
download from the official page, https://github.com/pancreations/Halo-MCC-VR.

REMOVE MANUALLY
---------------
Close MCC completely, then delete only this folder:

   Halo The Master Chief Collection\Halo_MCC_VR

Do not delete the main Halo The Master Chief Collection folder.

SAFETY
------
Use only the launcher above. It starts MCC without anti-cheat; never use this
mod in anti-cheat-enabled matchmaking.
