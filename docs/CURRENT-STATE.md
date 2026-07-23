# Current state

Authoritative as of 2026-07-23. This file is the only active accepted-build
pointer. Detailed pre-cleanup experiments remain available in Git history; they
are evidence, not instructions.

## Accepted cumulative release

The current known-good product is the public
[`MCC_VR_ALPHA_0.2.2`](https://github.com/pancreations/Halo-MCC-VR/releases/tag/MCC_VR_ALPHA_0.2.2)
Halo 3 + ODST release. It supersedes `MCC_VR_ALPHA_0.2.1`.

| Identity | Value |
| --- | --- |
| Release tag commit | `e2c049e5c3b98ce466f6072da4e0aa55ccc88e10` |
| Headset-tested runtime source | `3a2a11bfc66b36e70f60282e91c9d5436f2e18d1` |
| Build | Release x64, `HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP=ON` |
| Release ZIP SHA-256 | `43E52AEF5A2D1647A8F3AE6AEFDB6C22F0C67C7AA06FD70D327FB3E00ACF5DCC` |
| `halo3xr.dll` SHA-256 | `1E3F0F7E1D67DB7F322FF0B2C0236CA8708E4C9EC204EDE83484DBD6BBAF3BD6` |
| `halo3xr_launcher.exe` SHA-256 | `FA95B264630D42594581E4D2F8E1103FE4DB2D0711714DA4F62AA6175155C534` |

The 0.2.2 runtime source `3a2a11b` builds directly on the accepted 0.2.1 line
(`034c4a6`) with three evidence-backed ODST fixes (GitHub issue #18 -
head-relative movement, cinematic FOV parity, and steady rumble) and one XInput
connection-stability fix (the in-game menu no longer dies after Save & Quit).
The tag adds only release documentation and packaging on top of that tested
runtime; `src/` and `tests/` are otherwise the accepted line. The launcher was
rebuilt from the same source, so its bytes differ from 0.2.1 while its behavior
does not.

### Previous accepted release (rollback baseline)

`MCC_VR_ALPHA_0.2.1` remains a protected rollback baseline. Its runtime is
byte-identical to `034c4a6`; the tag added only documentation and packaging.

| Identity | Value |
| --- | --- |
| Release tag commit | `3d7989e1a8e0cb34747a91801c4525ef70b29866` |
| Headset-tested runtime source | `034c4a68e362b334d7994aa9e694243abf2aade5` |
| Release ZIP SHA-256 | `C5AE012BC379CBC7A909652D297DC0E8059CDBF41D26260771B385F8F729B124` |
| `halo3xr.dll` SHA-256 | `B7363F79650E42A04D4CED6A3F51F57A6B4C2F376FF00298A6173A8287752CEF` |
| `halo3xr_launcher.exe` SHA-256 | `BDC0A20F56DF72CDDE68E5D0AB621321FBDE91DA427B6C24142B38336D33EA6D` |

Protected rollback copies of the 0.2.1 ZIP:

- the official GitHub release asset;
- `dist/HaloMCCVR-odst-menu-fix-034c4a6.zip`;
- the user's external safe-folder copy.

An artifact is evidence, not an automatic deployment source. Never install it
unless the user explicitly asks.

### 0.2.2 headset confirmation - 2026-07-23

- Source commit: `3a2a11bfc66b36e70f60282e91c9d5436f2e18d1` (branch
  `cleanup/release-0.2.1`), built Release x64 with
  `HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP=ON`.
- Candidate package: `out/candidates/3a2a11b-20260723-142432262Z`.
- `halo3xr.dll` SHA-256
  `1E3F0F7E1D67DB7F322FF0B2C0236CA8708E4C9EC204EDE83484DBD6BBAF3BD6`; the
  installed file's hash was verified separately after the manual copy and
  matched. The log does not contain the hash.
- Log source: the first line reported `source 3a2a11b...`, ODST ON, compiled
  `Jul 23 2026 09:24:22`, from the canonical `Halo_MCC_VR\halo3xr.log`.
- Title coverage: Halo 3: ODST (stereo, head-relative movement, cutscenes,
  rumble, Save & Quit to menu, and cross-title re-entry). Halo 3 shares the
  touched XInput controller path.
- Result: the user confirmed all three issue #18 fixes still feel right and that
  the in-game menu no longer goes dead after Save & Quit, from both plain
  gameplay and a cutscene.
- Runtime evidence: the retained `M3 DIAG` line held `gateIdle=0` through normal
  play, then rose to `100` and `106` across a Save & Quit / title-teardown
  window while `reads`/`padValid`/`merged` kept climbing. The mod answered the
  slot-0 polls in that brief gated window as connected and idle, so MCC never
  latched a false controller disconnect and the pad stayed live.

## Desktop stale-version audit

This audit predates the 0.2.2 hotfix and describes the earlier 0.2.1 install
(`halo3xr.dll` `B7363F79...`), which was accepted at the time. As of 0.2.2 the
accepted `halo3xr.dll` is `1E3F0F7E...`.

The 2026-07-23 desktop installation is not running an older binary:

- installed DLL hash is the accepted `B7363F79...`;
- installed launcher hash is the accepted `BDC0A20F...`;
- the first log line reports embedded build `Jul 22 2026 12:59:32`;
- the launcher log resolves the canonical `Halo_MCC_VR` folder;
- the desktop shortcut points to that launcher;
- no second mod DLL or launcher exists under the MCC installation.

The old repository build trees contained unaccepted DLLs, and the legacy
scripts could either build ODST support OFF or restore an older vibration DLL.
Those build trees and scripts are not part of the clean baseline. If behavior
still differs from the laptop, investigate configuration, OpenXR/runtime state,
MCC title-module state, and the exact log; do not assume a source rollback.

The audited desktop log also showed the known multi-module ambiguity during
title switching and an ODST camera-readiness tail toggling before the user
paused. Those are runtime observations from the accepted binary, not proof of a
stale install.

### Desktop ODST headset confirmation - 2026-07-23

- Runtime source: `034c4a68e362b334d7994aa9e694243abf2aade5`.
- Installed artifact:
  `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR\halo3xr.dll`,
  SHA-256 `B7363F79650E42A04D4CED6A3F51F57A6B4C2F376FF00298A6173A8287752CEF`.
- Result: the user explicitly confirmed that ODST hooked, worked, and remained
  playable during a brief headset session.
- Runtime evidence: ODST was detected at `06:27:14.962`, its core hooks finished
  installing at `06:27:25.848`, and stereo, head tracking, 6DOF, controller aim,
  VRIK, authored crosshair, and distinct per-eye output armed at `06:27:36.205`.
  The delay covered title/camera loading, an OpenXR visible-but-unfocused
  interval, and the required one-second fresh-camera safety interval.
- A read-only 250-sample layout capture found only slot 0 active; it made no
  game-memory writes. No new build was installed, and no Halo 3 regression was
  run in this brief ODST-only session. The accepted-build pointer is unchanged.

## Headset-confirmed coverage

Halo 3:

- stereo, 6DOF, controller input and aim, head-relative movement;
- articulated arms, support hand, floating hands, and tested dual wield;
- native HUD, authored VR crosshair, HUD controls, scopes, and resolution scale;
- cutscene facing, pause/resume, death/respawn, mission exit/re-entry;
- smooth turn, recoil suppression, haptics, and shared configuration.

ODST on the accepted build:

- stereo, 6DOF, head-relative movement, snap/smooth turn;
- controller-driven gun/hands, arm IK, two-hand and floating-hand options;
- native HUD, authored floating crosshair, HUD controls, and vibration;
- stereo cutscenes with head look and authored-shot facing;
- death/respawn recovery, one tested drivable car, and cross-title re-entry;
- in-game menu stick fix from GitHub issue 9.

## Known limitations

- ODST's first captioned opening cutscene can be black; skip that first scene
  once.
- MCC can retain multiple title modules and return a level load to the menu.
  Fully restart MCC as the release workaround.
- ODST brightness must remain at the game default; the attempted brightness
  hook hid the entire HUD and was reverted.
- Only one ODST car is headset-confirmed. Broader weapon, vehicle, turret,
  passenger-gun, co-op, headset, and long-session coverage remains open.
- Full-body legs/torso are not implemented; VRIK covers first-person arms.
- Projectile direction follows the controller, but Halo still owns the actual
  fire origin.
- A local rebuild is not byte-reproducible because compile date/time and
  toolchain output are embedded. Use the release ZIP for exact accepted bytes.

## Rules that survive cleanup

- One cumulative multi-title line: every accepted build retains Halo 3 and ODST.
- Halo 3 is the player-facing parity foundation for new titles.
- Per-title offsets, signatures, layouts, bones, markers, tags, and calibration
  require per-title evidence.
- Render world, first-person weapon, native CHUD, and capture for each eye as one
  lifecycle transaction after the one-second fresh-camera safety interval.
- Never hook `halo3+0x120DF8`.
- Never write guessed camera, animation, model-root, or CHUD offsets.
- Unique signatures only; fail open to stock rendering.
- Never patch game files or interact with Easy Anti-Cheat.
- No automatic deploy, restore, install, uninstall, or launch scripts.

## Candidate and acceptance workflow

1. Start from this accepted source line.
2. Make one behavioral change and give it a unique commit.
3. Build and test the cumulative Release preset from `BUILDING.md`.
4. Use the safe package command to create a unique candidate under `out/`;
   never overwrite the accepted ZIP or reuse a candidate directory.
5. Deploy only after the user explicitly requests that exact candidate.
6. Record source commit, DLL hash, unique package path, embedded log
   source/configuration, title coverage, and headset result. Verify the installed
   hash separately because the log does not contain it.
7. Advance this pointer only after explicit acceptance. A failed or untested
   candidate is reverted and does not advance the line.
8. Run a Halo 3 regression whenever shared code or cross-title lifecycle state
   changes.

## Evidence map

- `docs/RE-notes.md`: verified Halo 3 reverse-engineering facts.
- `docs/EDITING-KIT-EVIDENCE.md`: evidence policy.
- `docs/ODST-SIGNATURE-EVIDENCE.md`: ODST signatures and HUD evidence.
- `docs/ODST-CAMERA-LAYOUT.md`: ODST camera/view layouts.
- `docs/ODST-WEAPON-IK-EVIDENCE.md`: ODST weapon and skeleton evidence.
- `docs/HISTORY.md`: how to retrieve the full pre-cleanup ledger.
- `releases/0.2.1/manifest.json`: machine-readable release identity.
