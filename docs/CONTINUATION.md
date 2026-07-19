# Session handoff

Start with CURRENT-STATE.md; it contains the current behavior, recovery commits, safety rules, failure ledger, limitations, and test sequence. Read RE-notes.md only when changing a verified engine boundary.

Current protected baseline: 330a568 on recovery/best-working-20260719-1300.

Current work branch: cleanup/production-baseline-20260719. This branch removes diagnostic hot-loop code, abandoned experiments, stale config/menu switches, and contradictory notes without intentionally changing gameplay behavior.

Before coding:

1. Run git status --short and identify every existing change.
2. Create a branch/checkpoint for risky work.
3. State one testable hypothesis.
4. Inspect the relevant source and verified disassembly/signature.
5. Build Release.
6. Deploy only with deploy.bat auto.
7. Have the user test the exact scenario in the headset.
8. Record the result in CURRENT-STATE.md only if it changes authoritative status.

Do not resurrect code from old commits merely because it once compiled. Git history is evidence, not a menu of fallbacks.