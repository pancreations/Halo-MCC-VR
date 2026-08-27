# Stage 3N — Halo 2 Classic barrel alignment calibration

Status: headset test candidate, not yet accepted.

Base binary: exact Stage 3M DLL SHA-256
`e56e29524a2c4e778536a437b7c00b9505ea58e3e468eb148c24f60598c9fbf9`.

Stage 3N is a guarded post-link layer. It adds `h2_classic_gun_yaw_deg` and
`h2_classic_gun_pitch_deg`, both default 0 and clamped to +/-30 degrees.

Only the Halo 2 packet-builder visible carrier call at RVA `0x95B16` is wrapped.
The wrapper calls the original `BuildStableFirstPersonCarriers` first, then:

- returns untouched for failure;
- returns untouched for `publishToRenderer != 0` (Anniversary/Saber);
- returns untouched when both Classic trims are exactly zero;
- otherwise rotates only the Classic right carrier forward/up basis.

The independent H2 native-aim call at RVA `0x9781D` remains a direct call to the
original carrier builder at RVA `0x92DD0`. Therefore the reticle/bullet direction
cannot inherit the Classic visual trim.

The Stage 3M base remains the protected fallback until headset testing accepts
Stage 3N.

Reproduce with GNU x86-64 binutils (`as`, `ld`, `objcopy`, `nm`) and Python 3:

```
python tools/build_stage3n_h2_classic_barrel.py Stage3M-HaloMCCVR.dll HaloMCCVR.dll
```
