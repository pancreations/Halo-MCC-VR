### REACH/3/ODST Now Playable in VR
### Work on HALO 4 will begin SOON after these hotfixes release. Yes Halo 1/2 will be in the collection eventually!
### GAMEPASS IS NOT SUPPORTED ATM, A HOTFIX IS OTW FOR THAT! 
### ALVR Support and default Steam VR branch is being looked into


## 🚧 What I'm working on RN
| Feature                                            | Status         |
| -------------------------------------------------- | -------------- |
| ALVR Support (double vision fix)                   | 🟡 In Progress |
| Halo: Reach Visual/UI Fixes                        | 🟡 In Progress |
| Space-Sky Bloom Bug Fix (Halo Reach)               | 🟡 In Progress |
| Xbox App / Game Pass Support                       | 🟡 In Progress |
| SMAA T2X Support                                   | 🟡 In Progress |
| Optional 2D Theater Mode for Cutscenes             | 🟡 In Progress |
| Gamepad support/Head-Aiming Mode                   | 🟡 In Progress |
|  Scopes and weapon zoom fixes                                                  | 🟡 In Progress |
| Complete F1 Menu UI Restructure and Reorganization | 🟡 In Progress |

> **Note:** The optional 2D theater mode will be enabled by default for cutscenes, but players will be able to disable it.

> **DO NOT ACCEPT FIXES FOR THIS MOD FROM OUTSIDE SOURCES. WE DON'T KNOW WHAT'S
> IN THEM.**
>
> **Two things are required:**
>
> 1. **The SteamVR Beta.** A user reported that it fixes the double vision.
> 2. **SteamVR set as your default OpenXR runtime.**
>
> Compatibility across headsets may vary. If it doesn't work, try a different
> way to connect if you can — but follow the two requirements above and you
> should be good.
>
> **Let me know in the [issues](https://github.com/pancreations/Halo-MCC-VR/issues)
> if your headset is not working** and someone or I can help you.
>
> For a GPU performance boost, try
> [Quad-Views-Foveated](https://github.com/mbucchia/Quad-Views-Foveated).
>
> **Please list your specs if you're having issues.** It's a guessing game over
> here.

# Halo MCC VR

> **Hi, I'm [pancreations](https://www.instagram.com/pancreations/)** — a 3D
> animator. I really don't like AI art, but I also really want to play MCC in
> VR, so I'm taking one for the team. Follow me on Instagram if you like silly
> animations made by humans in Blender. ([Expect me to quit at any
> moment.](https://www.youtube.com/watch?v=GFl3_wPFvdA))
>
> Living Fray is starting his MCC VR mod back up — if you'd rather run unsigned
> code made by a human, wait for his.

A native OpenXR VR mod for Halo 3, Halo 3: ODST and Halo: Reach in the Steam
edition of Halo: The Master Chief Collection.

The current known-good release is
[MCC VR Alpha 0.3.0](https://github.com/pancreations/Halo-MCC-VR/releases/tag/MCC_VR_ALPHA_0.3.0),
which adds **Halo: Reach**. It is an alpha: use it at your own risk, launch only
without anti-cheat, and expect incomplete hardware and gameplay coverage.

## What works

- Per-eye stereo and 6DOF head tracking.
- Motion-controller input, weapon aim, arm IK, snap/smooth turning, melee,
  grenades, and menu control.
- Native HUD, authored floating weapon crosshair, scopes, resolution scaling,
  comfort controls, and a shared F1 configuration menu.
- Halo 3 campaign behavior, including cutscenes, pause/resume, death/respawn,
  and mission transitions.
- ODST stereo, controls, weapons/hands, native HUD, cutscenes, vibration,
  death/respawn recovery, and one tested drivable car.
- Halo: Reach stereo, controls, weapon aim, hands/gun, HUD, cutscenes and
  vibration. Reach is new in 0.3.0 and is the earliest of the three titles.

On Reach the left trigger and X are swapped compared to Halo 3 and ODST, so
grenades sit on X.

Known limitations:

- ODST's first captioned opening cutscene can be black. Skip that first scene
  once; do not repeatedly skip or you will miss the working drop sequence.
- MCC can retain multiple title modules after switching games. If a level
  returns to the menu, fully close and restart MCC.
- ODST brightness stays at the game default.
- On Reach, character tags and navpoints are misplaced in 3D, and the
  `hud_curvature` and `hud_vertical_offset` settings have no effect.
- Broader ODST and Reach weapon, turret, passenger-gun, vehicle, co-op, headset,
  and long-session coverage is still needed.

The exact accepted source and artifact hashes are in
[docs/CURRENT-STATE.md](docs/CURRENT-STATE.md).

## Install

There is no installer script.

1. Download the binary asset
   `MCC_VR_ALPHA_0.3.0.zip` from the official `0.3.0` release page.
2. In Steam, open MCC's **Manage > Browse local files** folder.
3. Create a folder named exactly `Halo_MCC_VR` in the main MCC folder.
4. Copy `halo3xr.dll`, `halo3xr_launcher.exe` and `halomccvr.cfg` into that
   folder.
5. Make SteamVR the default OpenXR runtime, start Steam and SteamVR, then run
   `halo3xr_launcher.exe`.

The final path must end in:

```text
Halo The Master Chief Collection\Halo_MCC_VR\halo3xr_launcher.exe
```

Do not place the files loose in the MCC root. To uninstall, close MCC and delete
only the dedicated `Halo_MCC_VR` folder.

### Updating from 0.2.2 — replace your config

**Replace `halomccvr.cfg` with the one in the ZIP. Do not keep your old one.**
This is different from previous updates, which told you to keep it.

0.3.0 adds settings that older config files do not contain, and there is no
migration step: any setting your old file is missing silently falls back to a
built-in default rather than the shipped value. The most visible casualty is
`fit_desktop_window`, whose built-in default is off while the shipped config
turns it on — keeping an old config can therefore cap your headset frame rate.
Sharpening, HUD and weapon-alignment values regress the same way.

The shipped config is a tuned, tested configuration rather than bare defaults.
If you want your own tuning back, copy your old `halomccvr.cfg` somewhere safe
first, install the new one, then re-apply your preferences through the F1 menu.

If one PC behaves differently, first confirm SteamVR is still the default
OpenXR runtime, fully close every MCC process before relaunching, compare the
installed hashes below, and compare `halomccvr.cfg`. Do not use a repository
build folder as an installation source.

Release `0.3.0` hashes:

```text
ZIP      BE1C084F3F2D40CA95A22B66DF4644DF4A3576F7D2D70E001FB11B50AB4C6922
DLL      CE43FC67A72D14B6D1D9508C4BB6D8461A7733A303CC94B5784BA0274CE64E9F
Launcher 0433A47883AAA9516C25F1830F8DC33EB15098CABDC04EDC223250B1EFBF25F0
```

Windows security software may flag or quarantine unsigned injection-based VR
mods. Download only from the official release, verify the hashes, inspect the
source if desired, and allow only the two release binaries rather than disabling
security software globally.

## Required MCC settings

| Setting | Value |
| --- | --- |
| Video > Max Frame Rate | 120 |
| Video > V-Sync | Off |
| Halo 3 > Field of View | 120 |
| ODST > Look Sensitivity | Maximum |
| ODST > Look Acceleration | Off |
| MCC FSR | Off |

ODST's look settings control how quickly bullet direction catches the
motion-controller crosshair. If shots trail, confirm sensitivity is at maximum
and acceleration is off.

Settings live in `Halo_MCC_VR\halomccvr.cfg`, shipped in the ZIP as a tuned
configuration. The F1 menu edits the same values, and the game regenerates the
file if it is missing — but a regenerated file contains bare built-in defaults,
not the shipped tuning, so keep a copy of the shipped one.

Optional simulated weapon/hand weight is disabled by default. Enable and tune
it live in the F1 **Aim & Weapons** tab. **Weapon weight** controls normal
handling (0% is exact tracking); **Fast-movement catch-up** progressively pulls
harder as the controller gap grows. Catch-up reaches full strength around 15 cm
or 20 degrees, but neither value is a hard pose limit:

```text
weapon_inertia = 0
weapon_position_follow = 14.0
weapon_rotation_follow = 17.0
weapon_catchup_speed = 0.75
```

## Build from source


See [BUILDING.md](BUILDING.md). A clean build uses the accepted source and
configuration, but produces a new, unaccepted candidate and file hash. Only the
published hashes remain accepted until that rebuilt candidate passes a headset
test. Use the released ZIP when you need the exact accepted binaries.

## Development evidence

- [Current accepted baseline](docs/CURRENT-STATE.md)
- [Halo 3 reverse-engineering facts](docs/RE-notes.md)
- [ODST signatures](docs/ODST-SIGNATURE-EVIDENCE.md)
- [ODST camera layout](docs/ODST-CAMERA-LAYOUT.md)
- [ODST weapon/IK evidence](docs/ODST-WEAPON-IK-EVIDENCE.md)
- [Reach evidence manifest](docs/REACH-EVIDENCE-MANIFEST.json)
- [Reach signature evidence](docs/REACH-SIGNATURE-EVIDENCE.md)
- [Shared title-runtime ownership candidate](docs/TITLE-RUNTIME-OWNERSHIP.md)
- [Per-title evidence policy](docs/EDITING-KIT-EVIDENCE.md)

The code was written by Claude and Codex under the direction of a human modder
who made the product and reverse-engineering decisions and performed the headset
tests. No human reviewed every line. The project is licensed under the
[MIT License](LICENSE).

Inspired by HaloCEVR by LivingFray and ReclaimerVR by Nibre. Halo is a Microsoft
trademark; this project is not affiliated with Microsoft or Halo Studios.
