# Stage 3CO - Halo 4 authored reticle + 3D cutscene theatre reconciliation

**Base:** exact Stage 3CB theatre candidate (`9ee60ca1...`).

## What is retained

- The complete Stage 3BT reticle chain carried by Stage 3BU: weapon-only CUI
  selection, alpha repair, typeless RTV support, 2.5x size, bounded animation,
  native-flat-copy suppression, procedural bootstrap until authored art exists,
  and rolling diagnostics.
- Stage 3BU scene write-back for the desktop mirror and damage effects.
- Stage 3BV/3BW/3BX cinematic classification, theatre publication, capability,
  and look-constraint rule.
- Stage 3CB's headset-working theatre camera: authored FOV/camera, parallel
  capture views, and the live theatre Depth setting.

No branch switch, source rollback, or wholesale binary replacement is used.
The candidate is rebuilt directly from the exact 3CB DLL.

The reconstruction also leaves 98 bytes of unreachable `H4ID` probe payload
from the 3CF/3CG diagnostic line zero. There is no inbound callsite to that
region; it is outside both transactions and is not imported into this candidate.

## Log-proven reason the exact old bytes are insufficient

Stage 3BU's preserved Steam run captured real art continuously (`art 281` to
`1492`) while its native CUI base was `-1033.578/336.549`. The current installed
3CB run captures `art 0` while the same live hook reports several layouts in one
session, including `-1304.277/675.936`, `-862.829/441.742`,
`-1344.409/697.226`, and `-1893.000/1064.517`. Its frozen viewport therefore
looks at empty pixels even though the authored CUI transaction is executing.

## The isolated reconciliation

Only two inputs to the existing capture chain become live:

1. The Stage 3BP selector subtracts the hide displacement published by the
   current type-`0x28` container, rather than the old `4134.312` constant.
2. The private viewport derives width and vertical bias from that container's
   live hide/base-Y pair, then enters Stage 3BR's unchanged 2.5x scale tail.

NaN, zero, missing backbuffer, or unpublished inputs fall back to the complete
accepted 3BR viewport. The original type-`0x28`, payload-size-`0x0c`
discriminator remains exact. Stage 3CN's unlabelled removal of that proof is
deliberately excluded.

## Verification

`tools/test_stage3co_h4_reticle_theatre_reconcile.py` proves:

- byte identity with 3CB outside the payload, gate target, and one selector
  displacement;
- byte identity with 3BT across the complete authored-reticle chain outside
  those two deliberate live-layout inputs;
- exact preservation of every 3BU/3BV/3BW/3BX/3CB theatre region;
- bounded reads, buffer-only writes, finite/positive guards, and accepted-3BR
  fallback; and
- the unsupported 3CN discriminator change is absent.

Headset acceptance still requires Halo 4 gameplay with changing weapons/target
colour and one in-engine cutscene. A shared-code regression was not introduced;
Halo 3 remains required before any cumulative public release.

## Deployment

Installed to both Steam and Microsoft Store editions on 2026-08-31. Both
installed DLLs independently verify as
`7EBFC2E137E81D34D913489A776CC244BADE74CD9F019BEC2D05A94C613E0EF9`.
The exact prior Stage 3CB DLL (`9EE60CA1...`) and each edition's launcher,
configuration, and available log were preserved under
`out/deploy-backups/2026-08-31-pre-3CO/{steam,xbox}`. Launchers and configs were
not changed, MCC was not launched, and the accepted pointer was not advanced.
