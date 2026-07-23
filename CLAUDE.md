# Project instructions

Halo MCC VR is a native C++20 OpenXR mod for the Steam edition of Halo: The
Master Chief Collection. The accepted cumulative release supports Halo 3 and
Halo 3: ODST.

Read and obey `AGENTS.md`, then read `docs/CURRENT-STATE.md` before changing
code. Use `docs/RE-notes.md` only for verified Halo 3 facts and the ODST evidence
documents for ODST-specific signatures and layouts.

## User and acceptance

The user is a game modder, not a programmer. Explain test steps in plain
language. A build or log is supporting evidence; the user's headset result is
the acceptance test.

## Workflow

1. Confirm the branch descends from the accepted source pointer.
2. Make one evidence-backed behavioral change per candidate.
3. Configure, build, and test the cumulative Release preset described in
   `BUILDING.md`. Stop on any compiler or test failure.
4. Package only into the repository's ignored `out/` directory.
5. Do not copy files into MCC or launch the game unless the user explicitly asks
   to test that exact candidate.
6. For a requested headset test, record the source commit, DLL SHA-256, unique
   package path, embedded log source/configuration, title coverage, and result.
   Verify the installed file's hash separately; the log does not contain it.
7. Advance `docs/CURRENT-STATE.md` only after explicit headset acceptance.
8. Revert failed behavior before making another candidate.

There are intentionally no deploy, restore, installer, or uninstaller scripts.
The old scripts could build ODST support off or restore an older DLL. Installation
is manual and confined to a dedicated `Halo_MCC_VR` folder.

## Non-negotiable implementation rules

- Locate engine code with unique signatures; never ship a guessed hardcoded
  address.
- Never hook `halo3+0x120DF8`.
- Keep hot hooks deterministic and allocation-free.
- Do not patch game files or run with anti-cheat enabled.
- Preserve one universal `halomccvr.cfg` and F1 experience.
- Use H3ODSTEK and `halo3odst.dll` as primary ODST evidence. Halo 3 offsets and
  semantics are not ODST proof.

Definition of done is: clean diff, Release build, passing tests, exact candidate
identity, requested headset confirmation, and any required Halo 3 regression.
