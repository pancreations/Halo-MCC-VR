# Stage 3AG — Halo 4 active-edge upstream first-person origin hide

Status: **unaccepted headset test candidate**. Built directly from Stage 3AF.

## Why Stage 3AF produced no visible change

The Stage 3AF DLL contained its +0x100EBC upstream-origin installer and the
`H4AF IN` / `H4AF HIT` / `AFBAD` runtime strings, but it chained that installer
behind HaloMCCVR RVA `0x2F3715`. The headset log from the Stage 3AF run contains
none of those markers even though the normal Halo 4 camera/hand/CUI paths are
active. Static disassembly shows the actual live title query edge is
HaloMCCVR RVA `0x30894`, which still called the Stage 3AD title wrapper at
`0x2F15BB`.

Stage 3AG fixes that execution-path mistake rather than changing the effect
classifier again.

## Stage 3AG change

Only 46 bytes differ from the exact Stage 3AF DLL:

- `0x30894`: the active title query now calls a new 42-byte wrapper at
  `0x2F27AC`.
- The new wrapper calls the complete Stage 3AD title wrapper first, preserving
  Stage 3AC/3Z/3X/3AD behavior. When the returned title is Halo 4 it loads the
  retained `halo4.dll` base from the existing module reference and calls the
  Stage 3AF upstream-origin installer at `0x17F680`.
- `0x2F3715`: Stage 3AF's mistaken dormant chain is restored to the exact
  Stage 3AE call target. Stage 3AF's upstream hook and trampoline remain intact.

The Stage 3AF installer is therefore reached from the real live title path. It
patches the proven historical `halo4.dll+0x100EBC` origin resolver and retains
its exact narrow first-person/negative-designator gate. A qualifying returned
matrix is moved by the existing finite hide helper.

## Runtime proof

A Halo 4 run must now produce at least one of:

- `H4AF IN` — the upstream hook installed from the active edge;
- `AFBAD` — the exact halo4.dll origin-site guard failed;
- and, when the targeted special first-person origin executes, `H4AF HIT`.

No success is claimed until the headset image is clean.

## Preserved

Halo 2 accepted Stage 3I behavior and every later retained H2 byte are untouched.
Halo 3, ODST, Reach, HUD, reticle, stereo, 6DOF, hands, two-hand, pause and
re-entry code are untouched relative to Stage 3AF outside the three exact
Stage 3AG edit ranges.
