# MCC edition evidence — Steam vs Microsoft Store / Xbox app

Measured on the dev box, 2026-07-28. Both editions installed side by side:

| Edition | Install root used by the launcher |
| --- | --- |
| Steam | `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection` |
| Microsoft Store / Xbox app | `N:\XBOX\Halo- The Master Chief Collection\Content` |

Both report build tag `2025.08.16.178512.1-Release`.

## The game modules are the same code

`halo3.dll`, `halo3odst.dll`, `haloreach.dll`, `halo1.dll`, `halo2.dll`,
`halo4.dll` and `groundhog.dll` all have **identical file lengths** and
**different SHA-256** between the two editions. A full byte diff explains the
difference completely:

```
haloreach.dll   13229016 bytes   2109 differing bytes in 17 clusters
  0x000001C0..0x000001C1   PE OptionalHeader.CheckSum
  0x00C99E0B..0x00C9DBD6   inside the certificate table

halo3.dll       11127768 bytes   2099 differing bytes in 17 clusters
  0x000001B8..0x000001B9   PE OptionalHeader.CheckSum
  0x00A98E0B..0x00A9CBD6   inside the certificate table
```

The PE `IMAGE_DIRECTORY_ENTRY_SECURITY` entry of both `haloreach.dll` copies is
offset `0x00C99E00`, size `15832` — so every non-checksum differing byte lies
inside the Authenticode signature. `TimeDateStamp` is `0x68A0EFE1` in both.

**Consequence:** every AOB signature, offset, struct layout, bone, marker and
tag meaning verified against a Steam module is equally valid on the Store
module. One cumulative build serves both editions, and an edition difference is
never an explanation for a signature or offset failing to match.

### Halo 2 exact edition comparison (2026-08-19)

Both `halo2.dll` files are 15,807,960 bytes, share PE timestamp `0x68A0F0F2`,
SizeOfImage `0x02A38000`, and `.text` SHA-256
`973245E6898940B98BECC0F16BAB116B4A544B43DFAB041DB378279B8504C0DA`.
Steam SHA-256 is
`DE65B4F4FDBF3F0A5EAB7431FE530DA17DD815599182DFD6AE9B7E21CF171946`;
Store SHA-256 is
`81E5F41A7F8409D27A5454A28BFBECB8CD273E389366FB9865DD1D01E6BE689D`.
There are 835 differing bytes in 16 clusters and none outside the PE checksum
and Authenticode certificate. Complete mapped-image scans found the six C-H2-1
anchors exactly once at identical RVAs in both editions.

## What actually differs

Only the shipping executable and the surrounding install layout.

| | Steam | Microsoft Store |
| --- | --- | --- |
| Shipping exe | `MCC\Binaries\Win64\MCC-Win64-Shipping.exe` | `MCC\Binaries\Win64\MCCWinStore-Win64-Shipping.exe` |
| Exe size | 67179480 | 65333720 |
| Licensing | Steam | Xbox app / Gaming Services |
| Root marker | *(none)* | `MicrosoftGame.config`, `appxmanifest.xml` |

The DLL never scans the MCC executable with a signature. The only two places it
touches that image are structural and name-agnostic: `Input_ClaimXInputIat()`
walks the PE import table by DLL name/ordinal, and `InitExeRange()` computes the
image bounds from the PE headers. Neither is affected by the executable
differing between editions.

### Store package identity

`appxmanifest.xml` declares package family `Microsoft.Chelan_8wekyb3d8bbwe` with
two applications, both entry point `Windows.FullTrustApplication`:

- `HaloMCCShipping` — normal launch
- `HaloMCCShippingNoEAC` — *"Halo: MCC Anti-Cheat Disabled (Mods and Limited
  Services)"*

`MicrosoftGame.config` maps `HaloMCCShippingNoEAC` to
`MCC\Binaries\Win64\MCCWinStore-Win64-Shipping.exe`.

**`CreateProcess` on that executable can never work.** Measured 2026-07-28 with
no mod involved at all:

```
bare exe, no args, no injection   -> EXITED after 211 ms, exit code 0
our exact render args, no mod     -> EXITED after 223 ms, exit code 0
```

Exit code 0, no `Application Error` event, and MCC writes no log of its own: a
deliberate early self-exit, not a crash. The process has no package identity and
therefore no licence. There is **no** `CreateProcess` attribute that grants
identity — the full `PROC_THREAD_ATTRIBUTE_*` list in the Windows SDK
(`10.0.26100.0`, `WinBase.h`) contains nothing package-related, and
`PROC_THREAD_ATTRIBUTE_PACKAGE_FULL_NAME` does not exist. **Renaming the
executable to the Steam name therefore cannot fix this**, because the name was
never what failed.

The supported route is packaged activation, and it is what the launcher now
uses. Verified end to end on the installed candidate:

```
activation OK (helper pid 3692)          <- ActivateApplication returns S_OK
game process appeared, pid 31528         <- 4.2 s later
game loader ready; injecting             <- kernel32 present in module list
DLL injected OK into the running game    <- 22 ms after the readiness gate
game still running after 12s
```

Facts that follow, all measured:

- `IApplicationActivationManager::ActivateApplication` returns the pid of
  **`GameLaunchHelper.exe`**, not the game. The game process appears roughly
  4 seconds later and must be found by name.
- **Activation arguments are forwarded to the game verbatim.** The live process
  command line was
  `"...\MCCWinStore-Win64-Shipping.exe" -WINDOWED -ResX=3262 -ResY=2352`, so the
  Store edition gets the same VR render surface as Steam. (The Xbox app's own
  Play button passes **no** arguments — that command line is bare.)
- `OpenProcess(PROCESS_ALL_ACCESS)` on the packaged, full-trust game process
  succeeds from an ordinary medium-integrity process, so remote injection works.
- The process cannot be created suspended, so the launcher polls at 5 ms and
  gates injection on `kernel32.dll` appearing in the module list rather than
  injecting into the uninitialised-loader window.

Activating `HaloMCCShippingNoEAC` is the Store equivalent of running
`MCC-Win64-Shipping.exe` directly on Steam, so EasyAntiCheat is still never
started on either edition.

The running executable's real path is
`C:\Program Files\WindowsApps\Microsoft.Chelan_1.3528.0.0_x64__8wekyb3d8bbwe\MCC\Binaries\Win64\`,
**not** the `N:\XBOX\...\Content` path the launcher discovers. The content folder
is only a view; discovery and injection are independent, so this does not matter.

### Filesystem facts that matter for deployment

- The package content folder is **writable** — creating `Halo_MCC_VR` inside
  `...\Content\` succeeds (`Authenticated Users: Modify`).
- The two registered launchable executables (`MCCWinStore-Win64-Shipping.exe`
  and `mcclauncher.exe`) are **read-blocked** by Gaming Services even though
  their ACL grants `ReadAndExecute`; execute still works. Everything else,
  including all the game modules, reads normally. So tooling may `Test-Path`
  and execute the Store exe but must never try to hash or copy it.
- `HKLM\SOFTWARE\Microsoft\GamingServices\PackageRepository\Root\...` records the
  root as `\\?\N:\WindowsApps\Microsoft.Chelan_1.3528.0.0_x64__8wekyb3d8bbwe\`,
  which is **not readable**. Discovery must therefore keep walking up from the
  launcher's own folder rather than reading that path.

## Edition detection rule

`MicrosoftGame.config` sits beside the `MCC` folder in the Store package and in
no Steam install. The launcher treats an install as Store if the Store
executable name is present **or** that marker file is, so a Store install whose
executable was renamed to the Steam name — the community workaround this
replaces — is still detected as Store and is not asked to start Steam.

## Why Reach stayed stock on the Store edition — solved

Two logs from the **same DLL build** (`fdf6d6b`), one per edition:

```
Steam       Reach render cold preflight PASS: exact retail image, ...
Game Pass   Reach render cold preflight FAIL (backing-file-identity):
            main=0 inner=0 frustum=0; stock Reach remains active
```

Reach was detected fine on both (`detected supported title Halo: Reach
(haloreach.dll)`). What rejected it was `HashBackingFile()` in
`reach_render_preflight.cpp`: it SHA-256s the **whole file on disk** backing the
loaded module and compared it against a single pinned digest, the Steam one.

The Store copy the game actually loads is
`C:\Program Files\WindowsApps\Microsoft.Chelan_1.3528.0.0_x64__8wekyb3d8bbwe\haloreach\haloreach.dll`
— readable (only the launchable *executables* are read-blocked) and byte-diffed
against the Steam copy with the same result as every other module: **2109
differing bytes, all of them the PE checksum at `0x1C0` or inside the
certificate table at `0x00C99E00`. Zero code bytes.**

```
Steam           len=13229016  sha=738DD2D24EA3AEA1...  <- the pinned digest
Store (C: pkg)  len=13229016  sha=F9F39CF058FF28C2...  <- what the game loads
Store (N: view) len=13229016  sha=F9F39CF058FF28C2...  <- identical to it
```

So a whole-file hash is a hash of the *signature* as much as of the code, and it
rejected identical code purely for being signed by a different storefront.
`kReachRetailModuleSha256` is now the list of both signings of this one build.
That is not a loosening: image size, PE timestamp and every RVA in
`reach_render_logic.h` were already specific to this single MCC build, so an MCC
update invalidates the table either way.

Two diagnosis lessons are now built into the code:

- `BackingFileIdentity` used to mean both "file unreadable" and "wrong build".
  It is now split, with `BackingFileUnreadable` for the former.
- The failure log names the digest it actually read, so a rejected build
  identifies itself instead of only saying it did not match.

The reported "no controls" on Reach is consistent with this: the Store log shows
XInput hooked normally, so controls were not separately broken — Reach simply
stayed stock, which is what a failed render preflight does by design.

## Store loading stall and "half performance" — measured 2026-07-28

After Reach began hooking on Game Pass, the edition was reported to hang on a
loading screen and to feel roughly half as fast. Two logs, same DLL build
(`aa2f12a`), same titles in both sessions (Reach + ODST):

| | Game Pass | Steam |
| --- | --- | --- |
| `renderWindow p95` | **14.17 ms** | 14.51 ms |
| `frame interval p95` | 17.15 ms | 16.60 ms |
| fps peaks observed | 119, 120, 117 | 113, 120 |
| missed frames (cumulative) | 1565 | 386 |
| session length | 311 s | 147 s |

**Steady-state cost is the same or slightly better on Game Pass.** The frame
timings do not show halved performance. What Game Pass did show is one large
stall, which accounts for 1178 of its 1565 missed frames:

```
11:06:31  renderWindow p95 15.21ms  missed=84    HMD pose samples 56.9/sec
          (9.1 s with nothing logged)
11:06:40  fps 1 (stereo off)
11:06:41  renderWindow p95 15.24ms  missed=1178  HMD pose samples 6.1/sec
          xinput reads in the preceding 10 s window: 3
```

`renderWindow p95` is **unchanged across the stall** while HMD sampling and
XInput reads collapse. The VR render path was not the bottleneck: the game's own
main loop stalled for about nine seconds and then recovered.

### Ruled out by measurement

- **EasyAntiCheat.** Activating `HaloMCCShippingNoEAC` and listing the running
  game's 213 modules finds no EasyAntiCheat module and no anti-cheat process.
  (Careless regexes match `Oleacc.dll` and `halo**reach**.dll` — check the
  actual name.) EAC is not involved.
- **Encrypted / streamed Game Pass content.** Both editions' `haloreach\maps`
  hold the same 44 plain files totalling 10,518 MB, all directly readable, and
  the Store package path under `C:\Program Files\WindowsApps` is a mount-point
  junction to the same `N:` NVMe SSD that hosts the Steam copy. Same files, same
  disk. Do not resurrect the container-decryption theory.

### Difference that remains

Game Pass loads `xgameruntime.dll` and `gamingservicesproxy_13.dll`, which the
Steam build does not. That is the only platform-layer difference found so far.

### A/B result, 2026-07-28, build `0c42334` with stall timing

Same DLL, same runtime (SteamVR/OpenXR 2.17.6 via ALVR), same headset, back to
back:

| | Game Pass A | Game Pass B | Steam C |
| --- | --- | --- | --- |
| stalls | **1 x 9031 ms** | **1 x 9000 ms** | **0** |
| settled frame interval p95 | 15.7 ms | — | 15.5 ms |
| settled fps | **72** | — | **72** |
| Reach preflight | PASS | — | PASS |

**The stall is Game Pass-specific and reproducible; Steam records
`stalls=0 worstStall=0ms`.** Three measured stalls — 9074 ms, 9031 ms, 9000 ms —
all within 74 ms of exactly 9.0 s.

That constancy is the signature of a **timeout**, not disk I/O or asset
streaming: variable work does not land on the same number three times. Game Pass
is also the edition that loads `xgameruntime.dll` and `gamingservicesproxy_13.dll`.
A licence or services call giving up after ~9 s fits. **Stated as a hypothesis,
not a finding** — nothing has yet been instrumented inside that layer.

**Performance is not halved.** Both editions reach the same 72 fps ceiling with
comparable settled frame intervals (15.7 ms vs 15.5 ms). Game Pass is worse only
during the loading phase. An earlier comparison here that appeared to show a
large gap was comparing Game Pass's loading phase against Steam's settled phase;
that was not like-for-like.

**Consequence:** if the 9 s is a platform timeout, it is not the mod's to remove.
The correct response is to tell the player the game is loading rather than to
chase a fix we do not own.

### Log gap worth closing

Meta Link and ALVR both report `OpenXR runtime: SteamVR/OpenXR` with system name
`SteamVR/OpenXR : oculus`, so the log cannot distinguish them. A one-off
upside-down spawn was reported on Link and could not be attributed to a session
for exactly this reason, and the log had already rotated away.

### Correction 2026-07-28: every measured stall happened while NOT VISIBLE

Re-reading the preserved Store log line by line before building the in-headset
notice turned up something the stall numbers above do not say. The full session
state timeline of that capture:

```
[11:39:48.837] status: session=focused       shouldRender=1 layers=1
[11:40:07.320] status: session=synchronized  shouldRender=1 layers=1
[11:40:07.369] status: session=synchronized  shouldRender=0 layers=0
[11:40:20.285] STALL: the game has not presented for 1000ms
[11:40:28.271] STALL ENDED: the game resumed presenting after 9000ms
[11:40:34.215] Alt+F4 received
```

`XR_SESSION_STATE_SYNCHRONIZED` means the runtime is **not showing the app**. The
session left `focused` at 11:40:07 — **13 seconds before the stall** — and never
returned; `shouldRender=0 layers=0` for the whole remainder, including the entire
nine-second stall. Heartbeat totals for the two captures:

| | focused | visible | synchronized |
| --- | --- | --- | --- |
| Steam | 34 | 4 | **0** |
| Game Pass | 10 | 2 | **13** |

**The nine-second stall was measured while the headset was not being shown the
game at all.** `fit: menu cursor read` lines during and after it show mouse
movement in MCC's menus, i.e. desktop use. So that measurement is real as a
measurement of the GAME loop, and it is *not* evidence about what a player saw in
the headset. The reported in-headset loading hang has never been captured with
the session focused.

Two consequences, both load-bearing:

1. **The old STALL wording was wrong.** It asserted "the headset is showing a
   reprojected frame and the player sees a freeze". With `layers=0` nothing of
   ours was on the headset. The line now reports the session state and
   `shouldRender`, and says explicitly when nobody saw the stall.
2. **A render-thread notice cannot cover this stall.** Zero Presents crossed the
   nine seconds, and everything the mod draws happens inside the game's Present.
   The notice covers a crawling game; a hard-blocked game takes it with it. The
   only mechanism that could cover a hard block is the wait worker submitting a
   pre-rendered panel itself — a real change to the frame path — and it must not
   be built until one stall is captured with the session focused.

Next measurement needed: one Game Pass launch with the headset **on and worn
through the whole loading screen**, so a stall is recorded with `session=focused
shouldRender=1`. The build carries the logging to settle it in one run.
