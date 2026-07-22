# Release antivirus checklist

Every public release of `halo3xr.dll` / `halo3xr_launcher.exe` will be flagged by
Windows Defender, SmartScreen, and some third-party antivirus. It is a false
positive caused by two unavoidable facts about this mod:

- the launcher injects a DLL into `MCC-Win64-Shipping.exe`
  (`CreateRemoteThread` + `LoadLibraryW`), which matches the generic
  "process injection" heuristic; and
- the binaries are **unsigned**, and antivirus reputation is keyed to the file
  hash — so **every new build is an unknown file with zero reputation** and gets
  re-flagged even if the previous release was cleared.

The goal of this checklist is to reduce those false positives through
**legitimacy and transparency**, not evasion. Run it for every public release.

## Per-release steps

1. **Publish the hashes.** `export-alpha.bat` already writes `BUILD-INFO.txt` with
   the SHA-256 of both files. Copy those two hashes into the GitHub release notes
   so users can verify what they downloaded.

2. **Submit to Microsoft as a false positive** (free, highest-leverage action):
   - Go to <https://www.microsoft.com/en-us/wdsi/filesubmission>.
   - Sign in and choose the **Software developer** submission type.
   - Category: **Incorrectly detected as malware/malicious** (false positive).
   - Attach the release ZIP (or both files individually).
   - In the notes: state it is an open-source MIT VR mod that loads its own DLL
     into Halo: MCC's anti-cheat-disabled process, link
     <https://github.com/pancreations/Halo-MCC-VR>, and note it is unsigned by
     design for a free alpha.
   - Record the submission ID. Turnaround is usually 1–3 days and it clears that
     exact hash cloud-wide for all Defender users.
   - **Re-submit for every release** — a cleared hash does not help the next build.

3. **Submit to VirusTotal** (<https://www.virustotal.com>) and paste the result
   link into the release notes. It lets testers see the detection is sparse and
   which no-name engines are the noisy ones.

4. **Point users at the "how to allow" instructions.** The README section
   "Windows Defender / antivirus flags this" and the installer README files
   explain the verify-by-SHA-256 and allow-the-two-files steps. Link them from the
   release notes rather than telling anyone to disable protection.

## Do not

- **Do not** pack, obfuscate, encrypt, or otherwise try to hide the injection.
  Packers *raise* the heuristic score, destroy the reproducible-build transparency
  that is our best defense, and are the wrong direction for a legitimate open tool.
- **Do not** tell users to disable real-time protection or add whole-drive/
  whole-system exclusions. Only ever allow the two specific files or the
  `Halo_MCC_VR` folder.

## Longer-term option: code signing

An Authenticode code-signing certificate is the only thing that gives reputation
that carries *across* releases (which is exactly the "every update gets flagged"
problem):

- **OV (Organization Validation)** cert: ~$200–400/yr. Signs the binaries, but
  SmartScreen reputation still builds up over downloads/time.
- **EV (Extended Validation)** cert: more expensive and requires a hardware token,
  but grants immediate SmartScreen reputation.

For a free MIT alpha this may not be worth the cost yet — the free path above
(published hashes + Microsoft submission + public source) is a reasonable
substitute. Revisit if/when the mod has a wider public release.
