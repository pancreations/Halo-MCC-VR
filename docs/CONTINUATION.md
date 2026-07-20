# Session handoff

Start with CURRENT-STATE.md; it contains the current behavior, recovery commits, safety rules, failure ledger, limitations, and test sequence. Read RE-notes.md only when changing a verified engine boundary.

Current protected recovery baseline: 330a568 on recovery/best-working-20260719-1300.

Current best-working runtime checkpoint: `recovery/best-working-20260720`, also
the `master` tip. Deployed DLL build 2026-07-20 12:35 AM, SHA-256
`a9da286ddd983306010a812608aaefce41f912115bd5eb6b9cc7a8bfbfc61459`. It adds the
headset-confirmed smooth-turn jitter fix and the optional floating-hands mode on
top of the dual-wield build (`c2e6a27`, DLL SHA-256
`4D7FE27DD501AD9110DF9905DB825C9CA545021431ED4BE910CBF46D76064E5A`), which the
user described as almost perfect. Preserve both before any new work.

The current build includes L3+R3 F1 access, a working VR pointer, controller
vibration, separate F2 head-tracking and F11 stereo controls in the Status tab,
and a recenter action that resets both Halo and OpenXR. Commit `73f81f1`
replaces the non-working H3EK/HaloScript `game_paused` developer variable with
a signature-resolved native Halo pause byte. The focused Restart Level return-
to-3D sequence still needs an explicit recorded headset result before it is
called complete.

Dual wield now gives the left hand ownership of the secondary weapon: the left
hand tracks the left controller, while the gun and its marker/muzzle descendants
inherit the solved hand delta. Do not return to independently anchoring the gun
to the controller, and do not modify the working slot-0/two-handed path while
tuning dual wield.

Both previously queued isolated tasks are DONE and headset-confirmed on
2026-07-20: the smooth-turn jitter fix ("smooth as butter") and the optional
floating-hands mode ("works beautifully"). Leg/hip hiding was then CLOSED by
user decision. See PLAN.md for what remains open; there are no further
weapon-placement offset tasks currently requested.

The six restart-applied resolution presets are Potato 50%, Low 67%, Medium 80%,
High 100%, Ultra 110%, and Keith David 150% (4368x3150). Low is headset-
confirmed; the other five still need Quest 3 and PSVR2 coverage after a full
restart, and Keith David has never been run in a headset. Halo's internal
2912x2100 raster is scaled uniformly, but the OpenXR swapchain and imageRect
stay at the full runtime size.

There is no installer: install.bat/uninstall.bat and their fake-tree tests were
deleted on 2026-07-20 by user directive. Setup is copying the two binaries into
`<MCC>\Halo_MCC_VR`; uninstall is deleting that folder. Picture quality is
`resolution_scale` in `halomccvr.cfg`, free-form from 0.35 to 2.00 (it used to
snap to six tiers), editable by hand or from the F1 slider.

The current native-crosshair solution is `c923842`, confirmed in-headset at `8aa45d7`. It uses Halo's validated CHUD scripting-class-2 gate and preserves the VR reticle plus the rest of the HUD. Do not restore the `f0d5a88` runtime tag-table classifier; it caused a black headset view. Do not use `0x62C`, `0xF70`, or `0x1A90` as defaults: they are runtime chud_definition tag indices, not portable reticle ids.

HUD aspect correction at `1b53139` is confirmed on Quest 3 and PSVR2 with OpenXR Toolkit disabled; the remaining mild squeeze is accepted. Built-in FSR and an FOV slider are current non-goals. OpenXR Toolkit FSR showed a tiled/overlapping VR View in the observed setup, so the supported performance option is the restart-applied internal resolution preset.

Commit `42a1276` attempted a broad runtime cleanup and caused a headset fatal error during the first level transition. It is preserved only as failure evidence and must not be deployed.

The portable alpha workflow is `export-alpha.bat`. Its verified output is `dist\Halo3VR-alpha.zip`; the 2026-07-19 18:10 package DLL SHA-256 is `20B3CB0CA2995C48224CFE22A93AEED54704CFD9A09D59E9EB679FD5E8DA2D04`. The first separate-machine clean install and launch passed on an RTX 4060 laptop. Reinstall, update, uninstall, and non-default Steam-library tests remain pending. Do not infer that dormant code is safe to remove merely because it looks unused. Isolate one candidate per branch, preserve the exact runtime baseline, and require a headset level-load test before keeping it.

Before coding:

1. Run git status --short and identify every existing change.
2. Create a branch/checkpoint for risky work.
3. State one testable hypothesis.
4. Inspect the relevant source and verified disassembly/signature.
5. Build Release.
6. Deploy only with `deploy.bat auto`; verify both the DLL and launcher whenever the launcher or resolution path changes.
7. Have the user test the exact scenario in the headset.
8. Record the result in CURRENT-STATE.md only if it changes authoritative status.

Do not resurrect code from old commits merely because it once compiled. Git history is evidence, not a menu of fallbacks.
