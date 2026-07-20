# Halo 3 VR — Alpha 0.1

A native OpenXR VR mod for Halo 3 in Halo: The Master Chief Collection (Steam).

**This is an early alpha.** Halo 3 campaign is the tested path. ODST, online play,
custom games, Forge, every weapon, vehicles, scopes, and long sessions have not all
been validated.

**This code was written by AI.** Every line was written by Anthropic's Claude and
OpenAI's Codex, directed by a human modder who is not a programmer — he made the
decisions, ran the builds, and confirmed each result in a headset, but no human
reviewed every line. Treat it as unaudited source: the code is public precisely so
you can read it before you run it. Licensed MIT — fork it, learn from it, do whatever
is useful.

## What works

- True per-eye stereo and 6DOF head tracking
- Motion-controller input: snap/smooth turning, melee, grenades, menu control
- Controller-driven weapon aim with a floating VR reticle
- Articulated VR arms, with a free left support hand on the shotgun
- Native HUD with the centered flat reticle hidden
- Six picture-quality presets, from Potato (50%) to Keith David (150%)

## Required MCC settings

Set these in MCC's own menus. The mod does not change them for you, and you can
change them with the headset on from inside the VR session.

| Setting | Value |
| --- | --- |
| Settings > Video > Max Frame Rate | 120 |
| Settings > Video > V-Sync | Off |
| Halo 3 > Settings > Field of View | 120 |

**Do not enable FSR** in MCC's video menu — it breaks the VR image scale. Use the
mod's picture-quality presets instead.

Field of View is the one that visibly breaks the game if it is wrong. At the default
FOV the engine stops drawing geometry outside the flat-screen view, so in the headset
scenery pops in and out at the edges of your vision. FOV 120 pushes that boundary past
what the headset shows. V-Sync on, or a 60 FPS limit, caps the headset at that rate no
matter how fast your PC is.

## Install

1. Download `Halo3VR-alpha-0.1.zip` below and unzip the whole folder. Do not run files
   from inside the ZIP.
2. Close MCC, then run `install.bat`.
3. Pick a picture quality when asked. Low is the safest first try; F1 changes it in game.
4. Launch with the "Halo 3 VR" desktop shortcut it creates.

Requires Windows 10/11 64-bit, the Steam version of MCC with Halo 3 installed, and a
working OpenXR runtime. No compiler, CMake, or Visual C++ redistributable needed.

## Safety

- The shortcut launches MCC through its official anti-cheat-disabled mode. **Do not use
  this in anti-cheat-enabled matchmaking.**
- No game files are patched or redistributed. Normal Steam launches stay unmodded.
- The files are unsigned, so Windows or antivirus may flag them. Do not disable your
  security software globally — only allow these files if you trust the source.
- Uninstall with `uninstall.bat` in the game's `halo3xr` folder.

## Checksums

    halo3xr.dll          SHA-256: 7B33E98608CD5AD66600D479E5979E89D72F70CC972DB35FD857046A41F38F02
    halo3xr_launcher.exe SHA-256: 1778C5602C33869BE26774216530015A2E8F42928EB89C025572F86CA6DCFD05

## Reporting problems

Open an issue with what you saw in the headset, plus `halo3xr_launcher.log` and
`halo3xr.log` from the game's `halo3xr` folder, the `BUILD-INFO.txt` from the ZIP, and
your headset, GPU, and OpenXR runtime.

## Credits

Inspired by HaloCEVR by LivingFray and the proof of concept ReclaimerVR by Nibre.
Halo is a Microsoft trademark; this project is not affiliated with Microsoft or Halo Studios.
