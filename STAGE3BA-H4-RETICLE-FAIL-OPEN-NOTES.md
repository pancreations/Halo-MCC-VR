# Stage 3BA — Halo 4 reticle feature fail-open

Status: **HEADSET PENDING**.

Stage 3AZ corrected the DLL ABI fault, then proved the two-pass CUI replay
itself unsafe. Its headset log records five gameplay CUI replay attempts, zero
valid capture-begin markers, four guarded transform-write failures, and a
recoverable camera transaction exception. Windows then records `0xc0000005` in
`halo4.dll+0x5FB95`.

Stage 3BA disables both call sites that install or retry the optional Halo 4
CUI reticle hook. This is the required per-feature fail-open result: Halo 4's
camera, stereo, OpenXR, pause implementation, and all other title features are
unchanged. The safe procedural weapon-ray crosshair remains visible. The
differential replay code stays dormant for evidence and is not deleted.

ODST's post-preflight optional-scan isolation and the accepted ODST teardown
protections remain byte-identical.

- Candidate SHA-256: `03c5ff6e384428a1757008a1d2ae3b30c539267096b1f8a8619e955edefb48d8`
- Failure log: `out/test-runs/stage3az-fatal-h4-replay-engine-fault-steam-20260828-1319/HaloMCCVR.log`
- WER report: `out/test-runs/stage3az-fatal-h4-replay-engine-fault-steam-20260828-1319/Report.wer`

This candidate is a safety baseline, not acceptance of the Halo 4 reticle task.
Native reticle work must continue through a proven single-pass extraction path,
not another CUI replay.
