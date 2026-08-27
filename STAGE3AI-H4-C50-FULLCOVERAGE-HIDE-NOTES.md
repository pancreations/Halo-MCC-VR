# Stage 3AI — Halo 4 public-C50 full-coverage effect hide

Status: **headset test candidate; not yet user-accepted**.

## Why this is different from Stage 3AD–3AH

Stage 3AI does not extend any of the speculative AD/AE/AF/AH effect classifiers.
The current executable's one active Halo 4 title-install edge is redirected past
those experimental installers.

Instead, Stage 3AI recovers the effect-routing sites from the public C50/V6
binary layer itself:

- `halo4.dll+0x1059A2` — C50 negative-designator effect-location route;
- `halo4.dll+0x100EE8` — C50 helper route into the same effect-location bridge;
- C50 reserved runtime cave `halo4.dll+0xB79C10` / `+0xB79C80`;
- `halo4.dll+0x1012D5` — C50 transient positive-designator local-FP route.

The two runtime cave payload byte templates are byte-for-byte the public C50
payloads before their bridge pointer is relocated to this DLL.

C50 originally used these routes to re-parent selected first-person weapon
location matrices. The current project policy is to hide local weapon flashes / 
detail effects, so Stage 3AI keeps C50's broader selection/routing but applies
the current finite-hide action: matrix origin X/Y/Z are moved to +10000.0 while
the matrix remains valid. This specifically covers the negative-designator
family the later `+0x1012D5`-only passes did not cover.

## Preserved behavior

- The accepted Stage 3Z **camera-mode-1** particle deny is reproduced locally,
  preserving the already-working normal/default muzzle-flash suppression.
- Stage 3AH's broader camera-mode-2 contrast test is bypassed; its bytes remain
  in the binary only as unreachable historical code.
- Current Halo 4 HUD size/aspect/height/curvature installers are called unchanged.
- Halo 2, Halo 3, ODST, and Reach feature code is not rewritten. The only
  pre-existing executable control edges changed are the shared active-title call
  (which acts only when active title == Halo 4) and the Halo 4 teardown restore
  slot.
- Current Stage 3AH launcher/config are preserved byte-for-byte.

## Ground truth used

Public C50 DLL SHA-256:
`4aa0467453591430762d4f3201b5eb1f32affee7308b15bbf6ab069c819b027a`

Original V6 donor SHA-256 recorded in C50 provenance:
`419f2ca425a41f3fe42a2f27cfd0ce55123f71c5ef34c1c45604018285efea82`

Stage 3AH input SHA-256:
`7ceb1b741f94286c9766e777ec253208301fb97c8e3976b61d4fc6b141a8a402`

Stage 3AI output SHA-256:
`36232bc077d1ca4f5080bf514f93fb5b70f746ec3baaba5e75c46a66e3a2d0a8`

## Headset test

Use the exact Suppressor and Boltshot examples from the failed Stage 3AG/3AH
runs. The normal/default muzzle flash should remain suppressed, and the detached
orange Promethean hard-light/detail pieces are the Stage 3AI target.
