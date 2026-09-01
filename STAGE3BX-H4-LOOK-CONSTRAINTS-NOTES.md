# Stage 3BX - Halo 4 theatre obeys the ODST look-constraint rule

**Cumulative DLL:** `built/Stage3BX-HaloMCCVR.dll`
**SHA-256:** `54130fd5a37b2d5e19e610aad712fa8595501849cc27ead7753b132de7dfbd9e`
**Chain:** 3BW `9d6bba76` -> 3BX. One behavioral change.

**User requirement (2026-08-30):** "if you're able to turn your head, then
you should be in 6DOF in-game stereo; if player movement is locked, that is
when we are in the 3D cutscene" - i.e. the ODST standard.

## Why 3BW was not enough

3BW published `AuthoredLocked` on the cinematic in-progress byte alone. That
is exactly the test ODST's first headset pass disproved: the drop-pod
sequence is flagged cinematic while the player still has right-stick look, so
the theatre captured a sequence the player was controlling
(`docs/CUTSCENE-THEATRE-EVIDENCE.md`, "ODST live look constraints"). ODST's
accepted rule keeps `AuthoredLocked` only when the title's live look
constraints prove zero freedom.

## Halo 4 evidence for the same rule (new; H4EK-first, retail verifies)

- **H4EK** `halo4_tag_test.exe` carries ODST's authored block verbatim:
  `cinematic_shot_user_input_constraints_block`
  (`sizeof(s_scene_shot_user_input_constraints)`) with `ticks`,
  `maximum look angles`, `frictional force`, plus the script external
  `cinematic_scripting_set_user_input_constraints` ("Set user input
  constraints from a cinematic tag").
- **Retail** `halo4.dll` (`7c53e7d5…`) registers that external at
  `0x15EC5B`; its implementation `0x2C9314` walks scene -> shot -> constraint
  record and calls the live writer `0x28D18C`. The writer uses the engine TLS
  index (`halo4+0x1057218`), the cinematic camera at TLS block **+0x58**,
  verifies camera type word `[cam+2] == 6`, and derives per-tick rates into
  **+0xD8/+0xDC/+0xE0/+0xE4** from the targets and the CURRENT limits at
  **+0xC8/+0xCC/+0xD0/+0xD4**, storing remaining ticks at **+0xF0**
  (friction +0xE8/+0xEC). The paired reset `0x28D00B` zeroes both vectors and
  sets type 6, proving all-zero limits is the engine's own "no look freedom"
  state; the per-tick consumer `0x28C91B` reads the same block.
- ODST's proven layout is the same structure at TLS+0x50 with camera type 5.
  Halo 4's is independently derived here - no ODST offset was assumed.

## The rule as shipped

| observed | published | result |
| --- | --- | --- |
| in-progress byte 0 | PlayerControlled | immersive 6DOF gameplay |
| any current limit non-zero | PlayerControlled | immersive (you can look) |
| ticks > 0 with any non-zero rate | PlayerControlled | immersive (freedom pending) |
| all limits zero, nothing pending | **AuthoredLocked** | **3D theatre** |
| camera missing / type != 6 / ticks out of range | *nothing published* | immersive |

Limits and rates are tested as `|x| > 1e-4` by masking the sign bit and
comparing the raw float bits, so a NaN or Inf reads as freedom - every
failure direction lands on immersive gameplay, never on a wrongly captured
cutscene. The log now reports the verdict:
`H4CINE: state a -> b [1=no proof, 2=member null, 3=gameplay, 4=cinematic
LOCKED (theatre), 5=cinematic but look is free (immersive), 6=constraints
unreadable]`.

## The change

The `jne` at 0x2C2BC now lands on `s3bx_theatre` (817 bytes at 0x2FACB0),
which carries the proven probe, the new constraint classification, and the
same two publications; it exits into the intact 3BU write-back (0x2F9D10).
The 3BW payload stays byte-identical but unreachable, and 3BW's capability
grants (0x1F3 at 0x68112 / 0x1890D4) are untouched.

## Test

`python tools/test_stage3bx_h4_look_constraints.py` - PASS: byte identity vs
3BW outside the splice + payload; camera from TLS+0x58 with the type-6 check;
all four limits and all four rates tested by mask+epsilon; tick range checked
before any rate is trusted; AuthoredLocked only on state 4; Unknown and
no-proof publish nothing; exactly one call each to LOG / Generation / both
publishers; stack- and payload-only writes; capability grants and all
3BQ..3BW artifacts intact; 3BJ absent. The builder additionally byte-verifies
all nine halo4.dll anchors against the pinned module before emitting.

## Deployment

Installed 2026-08-30 into both editions (MCC confirmed closed); Stage 3BW
preserved under `out/deploy-backups/2026-08-30-pre-3BX/{steam,xbox}`.

## Headset test (plain language)

1. Play a Halo 4 in-engine cutscene where you cannot move or look: it should
   fade onto the room-fixed 3D screen.
2. Any scripted moment where you CAN still look around should stay normal
   immersive VR - no screen.
3. Known limit still open: during theatre the picture is still filmed with a
   head-tracked camera, so it shifts as you move your head, and the Depth
   slider does not yet change Halo 4's separation. That is the next stage.
4. Say which edition you used.
