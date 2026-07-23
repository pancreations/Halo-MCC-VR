# Release safety and antivirus checklist

Halo MCC VR launches MCC without anti-cheat and injects a DLL to implement VR.
Unsigned injection-based binaries may be warned on or quarantined by security
software. Never promise that every scanner will accept a build.

For every public candidate:

1. Build from a clean accepted-source descendant with the cumulative Release
   preset.
2. Require passing core tests and a clean diff review.
3. Use `tools/package-candidate.ps1` to create a unique ignored package under
   `out/candidates/`; never write directly to MCC or reuse a package directory.
4. Confirm `CANDIDATE-MANIFEST.json` says `UNTESTED_LOCAL_CANDIDATE` and records
   the source commit and exact DLL and launcher hashes.
5. After headset acceptance, create a release manifest and final ZIP without
   changing the tested binaries; record the final ZIP SHA-256.
6. Confirm the final package contains only the tested DLL and launcher, manual
   guide, license, and matching manifest.
7. Scan the package with the available local security software.
8. If a scanner flags it, publish the exact detection and affected hash. Do not
   dismiss a result without investigation.
9. State that no human reviewed every line and that users run the alpha at
   their own risk.
10. Tell users to download only from the official release, verify hashes, and
    allow only the release files rather than disabling security globally.
11. Launch only through the included launcher with anti-cheat disabled.

There are no installer or uninstaller scripts. Installation is manual inside a
dedicated `Halo_MCC_VR` folder, so a package cannot delete or alter game files.
