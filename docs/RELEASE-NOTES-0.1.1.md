# Halo MCC VR — Alpha 0.1.1

A native OpenXR VR mod for Halo 3 in Halo: The Master Chief Collection (Steam).

A small follow-up to alpha 0.1. Same VR core; this release is about giving you
control over the settings instead of locking them behind an installer prompt.

**This is an early alpha.** Halo 3 campaign is the tested path. ODST, online play,
custom games, Forge, every weapon, vehicles, scopes, and long sessions have not all
been validated.

**This code was written by AI.** Every line was written by Anthropic's Claude and
OpenAI's Codex, directed by a human modder who is not a programmer — he made the
decisions, ran the builds, and confirmed each result in a headset, but no human
reviewed every line. Treat it as unaudited source: the code is public precisely so
you can read it before you run it. Licensed MIT — fork it, learn from it, do whatever
is useful.

## What changed since 0.1

**Pick your own resolution.** `resolution_scale` accepts any value from 0.35 to
2.00 and is honored exactly — type 0.90 and you get 90%, where before it silently
rounded down to the nearest of six presets. Anything above 1.00 supersamples; the
2.00 ceiling is 5824x4200. The named tiers (Potato/Low/Medium/High/Ultra/Keith
David) survive as one-click shortcuts in the F1 menu.

**The settings file is now documented and hand-editable.**
`Halo_MCC_VR\halomccvr.cfg` is written on first run with a plain-English
description, default value, and accepted range above every setting, so you can put
anything back the way it was without reinstalling. Delete the file and it
regenerates with defaults. F1 → Advanced also has a two-click "Reset ALL settings
to defaults".

**The left hand can be resized.** It never could before: the size trim was applying
the right wrist's bone list to the left hand, so no left-hand size value ever
reached a bone. There is now a separate `left_hand_scale` (F1: "Left hand size",
with a Match weapon button) covering the support hand and the second gun when
dual-wielding.

**No more installer.** `install.bat` and `uninstall.bat` are gone. Install is
copying two files into a folder you make; uninstall is deleting that folder. If you
still have an `uninstall.bat` from an older package, delete it rather than run it —
a version from before 2026-07-20 could delete your game installation.

**Tuned defaults**, from headset testing: weapon size 0.97 (was 0.85) and left hand
forward offset -0.093 (was 0.120).

## Quest users: use Steam Link

Testers report the best performance on Quest with Steam Link rather than Virtual
Desktop. If the image stutters or feels behind your head, change that before you
change any mod setting.

## Install

1. Download the ZIP below and unzip the whole folder. Do not run files from inside
   the ZIP.
2. In Steam, open MCC's **Manage > Browse local files** and create a folder named
   exactly `Halo_MCC_VR` inside it.
3. Copy `halo3xr.dll` and `halo3xr_launcher.exe` into that folder.
4. Start Steam and your OpenXR runtime, then run `halo3xr_launcher.exe`.

Updating from 0.1: close MCC and copy the two new files over the old ones. Your
`halomccvr.cfg` is left alone. Delete any `install.bat`/`uninstall.bat` still
sitting in the folder.

## Required MCC settings

Set these in MCC's own menus. The mod does not change them for you, and you can
change them with the headset on from inside the VR session.

| Setting | Value |
| --- | --- |
| Settings > Video > Max Frame Rate | 120 |
| Settings > Video > V-Sync | Off |
| Halo 3 > Settings > Field of View | 120 |

Do **not** enable FSR in MCC's video menu. Field of View 120 is a rendering
requirement, not a comfort preference: at the default FOV the engine culls geometry
outside the flat-screen view and scenery pops in and out at the edges of the
headset image.

## Safety

- The launcher starts MCC through its official anti-cheat-disabled mode. **Do not
  use this in anti-cheat-enabled matchmaking.**
- No game files are patched or redistributed. Normal Steam launches stay unmodded.
- The files are unsigned, so Windows or antivirus may flag them. Do not disable your
  security software globally — only allow these files if you trust the source.
- To uninstall, close MCC and delete only the `Halo_MCC_VR` folder you created.
