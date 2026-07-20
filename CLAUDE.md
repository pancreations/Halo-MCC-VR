# Project instructions

Halo 3 OpenXR is a native C++20 OpenXR VR mod for Halo 3 in MCC Steam. Before changing code, read docs/CURRENT-STATE.md. It is the only authoritative status and failure ledger. docs/RE-notes.md contains only verified reverse-engineering facts.

## User and testing

The user is a game modder, not a programmer. Explain test steps in plain language. Headset results outrank theories, logs, and desktop appearance. Do not claim a fix until the user has tested the exact deployed DLL.

## Non-negotiable workflow

1. Start from a clean Git branch and create a checkpoint before risky work.
2. Make one evidence-backed behavioral change per test build.
3. Build Release and stop on any compiler error.
4. Deploy only with deploy.bat auto; it checks that MCC is closed, builds, copies, and byte-compares the DLL.
5. Match the deployed DLL timestamp/hash to the first line of halo3xr.log.
6. Record the headset result. Revert failed experiments instead of leaving dormant switches, probes, or fallback paths.
7. Locate engine code with unique AOB signatures. Never ship a guessed hardcoded address.
8. Keep logging, file I/O, locks, COM, and allocation out of render and palette hot hooks.
9. Never patch game files on disk or interact with Easy Anti-Cheat. The mod runs only through MCC's official EAC-disabled mode.
10. Never hook halo3+0x120DF8; even a pass-through detour crashed on level load.

## Paths

- Source: N:\dev\halo3-openxr
- Game: N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection
- Host: MCC\Binaries\Win64\MCC-Win64-Shipping.exe
- Engine: halo3\halo3.dll
- Deployed mod: Halo_MCC_VR\halo3xr.dll
- Runtime log: Halo_MCC_VR\halo3xr.log

## Definition of done

A code change is complete only after a Release build succeeds, the diff contains no accidental behavior changes, deployment verifies byte-for-byte, and the requested behavior is confirmed in the headset. A failed signature must log clearly and leave the game running safely.
