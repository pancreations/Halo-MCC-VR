# Stage 3AZ — Halo 4 reticle-difference ABI correction

Status: **HEADSET PENDING**. Stage 3AY failed in the headset and is not an
accepted build.

Stage 3AY reached Halo 4 gameplay, armed the camera, and crashed on the first
gameplay CUI work. Windows recorded `0xc0000005` at `HaloMCCVR.dll+0x20A25`.
That instruction is an aligned `movaps` stack store. Inspection of the new
capture wrapper found the direct cause: it called compiled code without the
Windows x64 32-byte shadow space or restoring 16-byte call alignment.

Stage 3AZ disables that failed wrapper shape while retaining the same reticle
separation behavior:

- both capture-begin branches tail-jump to the original compiled capture
  functions, preserving the caller's exact ABI frame;
- the only other nested compiled call reserves and releases `0x28` bytes around
  the call;
- the differential shader selector preserves nonvolatile `RBX`;
- the title's pixel-resource slot 1 remains saved, restored, and reference
  balanced;
- the accepted pause payload is byte-identical;
- ODST's optional post-preflight scan remains feature-isolated.

Static verification pins the complete 2,072-byte payload and explicitly checks
both tail transfers, the balanced shadow-space pair, and nonvolatile-register
preservation.

- Candidate SHA-256: `76727356f3ce4b053b09eccde6783a82ea5249d7ebfb9df9bc949b8822affb10`
- Payload SHA-256: `a4c2f2ee62cc237b0887fd260b89001caf13ba10a617f11d3c25583a25016316`
- Failed AY log: `out/test-runs/stage3ay-crash-h4-first-cui-stack-alignment-steam-20260828-1305/HaloMCCVR.log`
- Failed AY WER: `out/test-runs/stage3ay-crash-h4-first-cui-stack-alignment-steam-20260828-1305/Report.wer`

Required headset checks remain: Halo 4 reaches gameplay without crashing, the
VR crosshair contains the actual weapon reticle and no visor fragment, pause
remains fixed, and ODST hooks and presents in 3D.
