# Stage 3BV - Halo 4 cinematic-globals probe (log-only)

**Cumulative DLL:** `built/Stage3BV-HaloMCCVR.dll`
**SHA-256:** `e98e9502516ea0166c7bcb528fca5e10ba2ec1db1275f8a62bae449001fe44d9`
**Chain:** 3BU `5905afce` -> 3BV. One behavioral change (a log-only probe).
**User request (2026-08-30):** add the 3D cutscene theatre to Halo 4, "just
like we did with the other three halos."

## Why a probe first

The theatre is title-blind: `docs/CUTSCENE-THEATRE-EVIDENCE.md` ("Future
titles") says a new title gets `TitleCapability_CutsceneTheater` only after
title-specific evidence distinguishes an authored locked camera from player
control. Halo 3/ODST use their TLS cinematic globals (+5 byte); Reach earned
its `+0x24` byte through the accepted log-only REACHCINE probe. Halo 4 had
"no cinematic evidence" (title_registry.cpp) - until now. This stage is Halo
4's REACHCINE equivalent: it changes nothing visible, publishes nothing, and
writes `H4CINE:` log lines only when the observed state changes, so one
watched cutscene proves the byte's live semantics.

## Evidence (H4EK-first; retail verifies)

- **H4EK** `halo4_tag_test.exe` registers game-state members
  `cinematic globals` (0x54 bytes) and `cinematic globals non deterministic`
  (0x28) at its unique registration site (kit 0x234D00,
  `__tls_set_g_cinematic_globals_allocator` symbols, source string
  `c:\mcc\release\h4\shared\engine\source\blofeld\cutscene\cinematics.cpp`).
  The kit caches the member pointer in the engine TLS block (kit +0x550) and
  a null-checked leaf getter (kit 0x234CD0, 161 callers) returns **byte +5**
  of the member - the same "+5 = cinematic active" layout already proven for
  Halo 3 and ODST. Dozens of kit sites gate on `cmp byte [member+5], 0`.
- **Retail** `halo4.dll`
  (`7c53e7d5bc9848545a1b70e2768242479336fba1b7630d7ab955f7fd0c34fa84`)
  registers the same two members with the same sizes at its unique site
  0x12CA24: TLS index dword at `halo4+0x1057218`, member pointer cached at
  TLS block **+0xC8** (non-deterministic at +0x1E0). The retail getter at
  0x12EF68 null-checks the member and returns byte +5; the teardown path at
  0x12CCE2 writes byte +5 = 0. The hs name table pairs id 0x2AF with
  `cinematic_in_progress` in both kit and retail. Both 12-byte verification
  patterns match exactly once in the module.

## The change

The Stage 3BU steady-state `jne` at 0x2C2BC (once per claimed Halo 4 eye) is
re-pointed to `s3bv_probe` (401 bytes at 0x2F9E10, limit 0x2FA000), which
ends with `jmp 0x2F9D10` so the accepted 3BU scene write-back runs unchanged.

The probe: at most one sample per 250 ms (GetTickCount64 via the IAT; all
other eyes cost a compare+branch). A sample resolves `halo4.dll` via
GetModuleHandleW (IAT), byte-verifies the unique registration site
(`8B 15 8D A7 F2 00 41 B9 | 28 00 00 00` at +0x12CA85) before trusting any
offset, bounds the TLS index (<256, the mod's own H3/ODST rule), then reads
TLS block +0xC8 -> bytes +4/+5. Composite state: 1 = proof unavailable,
2 = member null, 3 = idle (byte +5 == 0), 4 = cinematic (byte +5 != 0).
`LOG` fires only on transitions. Fail-open: every failed check degrades to
state 1 and the engine/3BU path continues untouched. Stack- and
payload-internal writes only; no allocation.

## What the next log answers

- `3 -> 4` at cutscene start and `4 -> 3` at its end proves the in-progress
  byte (theatre detection can then publish `AuthoredLocked`).
- Whether cutscene frames are claimed stereo frames (the probe only runs on
  claimed eyes; C-H4-9's stock-window telemetry covers the unclaimed case).
- Byte +4's behavior rides along for layout evidence.

## Test

`python tools/test_stage3bv_h4_cine_probe.py` - PASS: byte identity vs 3BU
outside the 6-byte splice + payload; probe decode (two IAT calls + LOG only
on transitions, registration verify qword, TLS bound, member 0xC8, bytes
+4/+5, gs:[0x58] read); exits into the intact 3BU thunk; all 3BQ..3BU
artifacts intact; 3BJ absent.

## Deployment

Installed 2026-08-30 into both editions (MCC confirmed closed); Stage 3BU
preserved under `out/deploy-backups/2026-08-30-pre-3BV/{steam,xbox}`.

## Headset test (plain language)

Play Halo 4 and let one in-game cutscene play (level intro is fine), then
quit. Nothing should look different - this build only takes notes. Send the
log (or just say the test is done) and name the edition. I read the H4CINE
lines to confirm the cutscene detector, then build the actual theatre.
