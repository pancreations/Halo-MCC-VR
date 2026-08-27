# 0.3.5 / 1939eabc + C50 Stage 1 source baseline

Base: exact accepted upstream source revision
`1939eabc21c1607ef93ccaec97de004271d70091`.

C50 carry-forward in this source tree is intentionally limited to the exact
three-file C-H4-50 fail-closed reticle source delta from the user's known-good
C50 line:

- `src/common/halo4_cui_reticle_logic.h`
- `src/dll/vr.cpp`
- `tests/core_tests.cpp`

No Halo 2 / H2A source file is modified by this C50 delta.

The accompanying Stage 1 runtime DLL is made directly from the exact accepted
0.3.5 DLL and uses the equivalent 5-byte H4-only runtime patch at raw offset
`0x51E2D` / VA `0x180052A2D`.

## Important V6 post-build limitation

The previous C50/7a binary also carried five sourceless post-link sections:
`.h4fx`, `.h4fd`, `.h4hs`, `.h4hp`, `.h4pb`. The accepted 0.3.5 DLL does not
contain those sections and its stock image layout overlaps the old RVAs. The
existing guarded recovery tool does not recognize the 1939 layout. Therefore
those sections are not transplanted into Stage 1. They require a separately
verified relocation/reconstruction pass.

Stage 1 exists to prove the accepted five-title 0.3.5 runtime plus C50 reticle
safety before adding any layout-sensitive V6 post-build behavior.
