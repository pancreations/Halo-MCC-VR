# Historical ledger

The active handoff is intentionally compact. The full 1,525-line experiment,
headset-result, recovery, and failure ledger that shipped with release 0.2.1 is
preserved permanently in Git:

```powershell
git show MCC_VR_ALPHA_0.2.1:docs/CURRENT-STATE.md
```

Older public release notes remain on GitHub's release pages and in their tags.
Use history to understand a specific decision or failure; do not treat an old
"current", "baseline", deploy command, branch name, or DLL hash as an active
instruction. `docs/CURRENT-STATE.md` always wins.
