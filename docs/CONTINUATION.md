# Session handoff

Start with CURRENT-STATE.md; it contains the current behavior, recovery commits, safety rules, failure ledger, limitations, and test sequence. Read RE-notes.md only when changing a verified engine boundary.

Current protected baseline: 330a568 on recovery/best-working-20260719-1300.

Current work branch: feature/resolution-scale at `1fc56c8`. The five restart-applied resolution presets are Potato 50%, Low 67%, Medium 80%, High 100%, and Ultra 110%; Low is headset-confirmed, while the other four tiers still need Quest 3 and PSVR2 coverage after a full restart. Halo's internal 2912x2100 raster is scaled uniformly, but the OpenXR swapchain and imageRect stay at the full runtime size.

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
