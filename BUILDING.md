# Building Halo MCC VR

This builds the cumulative Halo 3 + ODST source. Every generated file stays
under ignored `out/`; nothing writes to an MCC installation.

## Requirements

- Windows x64.
- Visual Studio 2022 with **Desktop development with C++**.
- CMake 3.24 or newer.
- Git and network access for the first dependency download.

OpenXR, MinHook, and Dear ImGui are pinned to exact commits in
`CMakeLists.txt`. Fetches are shallow and shared under `out/deps`.

## Build and test

From a Developer PowerShell:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The preset always builds Release x64 with ODST enabled. `camscan` is excluded:
it is an opt-in diagnostic with process-memory write modes, not a product target.

## Create a test candidate

Commit the intended source first, then run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-candidate.ps1
```

The command refuses a dirty worktree, reconfigures, rebuilds, reruns tests, and
creates a new directory such as:

```text
out/candidates/1a2b3c4-20260723-120000Z/
```

It contains only the DLL, launcher, license, generic manual, and a
`CANDIDATE-MANIFEST.json` with the full commit, ODST state, exact file sizes,
and SHA-256 hashes. It never copies to MCC, never reuses a candidate directory,
and never labels rebuilt bytes as release 0.2.1.

Deployment is manual and requires explicit user approval for that exact
candidate. A rebuild uses the accepted source/configuration but remains
unaccepted until its exact hash passes a headset test.

## Inspect the published 0.2.1 source

Use a separate clean clone or worktree so historical outputs cannot mix with
active candidates. The tag predates the preset and defaults ODST off, so use a
distinct build directory and pass ON explicitly:

```powershell
git switch --detach MCC_VR_ALPHA_0.2.1
cmake -S . -B out/build/tag-0.2.1 -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_TESTING=ON `
  -DHALOMCCVR_EXPERIMENTAL_ODST_BRINGUP=ON
cmake --build out/build/tag-0.2.1 --config Release
ctest --test-dir out/build/tag-0.2.1 -C Release --output-on-failure
```

The tag is `3d7989e1a8e0cb34747a91801c4525ef70b29866`; its runtime source is
byte-identical to accepted source `034c4a68e362b334d7994aa9e694243abf2aade5`.
Exact dependency commits and published hashes are recorded in
`releases/0.2.1/manifest.json`.

## Exact bytes

The exact headset-accepted DLL and launcher come from the official binary asset
`MCC_VR_ALPHA_0.2.1.zip`. The identical preserved local rollback copy is named
`dist/HaloMCCVR-odst-menu-fix-034c4a6.zip`. A local build gets a new hash because
compile time and toolchain output affect its bytes. Never substitute a rebuild
for those accepted artifacts or overwrite the preserved ZIP.