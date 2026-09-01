# Stage 3BW - Halo 4 joins the shared 3D cutscene theatre

**Cumulative DLL:** `built/Stage3BW-HaloMCCVR.dll`
**SHA-256:** `9d6bba764e93dd6cd7b2482e0131765057196f92da725836277497f4996620f1`
**Chain:** 3BV `e98e9502` -> 3BW. One behavioral change: theatre enablement.

## Detector acceptance behind this stage (2026-08-30, Steam, live session)

The 3BV probe's headset run proved the H4EK-evidenced byte live:
`H4CINE: state 0 -> 4` at 18:13:28 (held through the prologue video and the
authored intro cutscene) and `state 4 -> 3` at 18:18:04 - the user confirmed
from inside the headset that control returned at exactly that moment. The
same log proves cutscene frames are fully claimed stereo pairs (C-H4-9: 168
completed pairs, 0 mid-cutscene stock windows), so the theatre's image
source (`g_eyeCache`) is already populated during cinematics.

## The change (four byte-regions, one behavior)

1. The `jne` at 0x2C2BC now lands on `s3bw_theatre` (521 bytes at 0x2FAAA0,
   limit 0x2FB000), which carries the identical proven probe (250 ms gate,
   registration byte-verify, TLS+0xC8 byte +5, H4CINE transition logging)
   and then, on states 3/4 with a nonzero Halo 4 generation
   (`TitleRuntime::Generation` 0xA5A0 on `g_titleRuntime` 0x2BA538):
   - `TitleAdapter_PublishCinematicControl` (0x88400): state 4 ->
     AuthoredLocked(2), state 3 -> PlayerControlled(1), 250 ms heartbeat
     (inside the 500 ms freshness window);
   - `TitleAdapter_PublishCutsceneTheaterProjection` (0x884C0): aspect =
     backbuffer W/H (0x2AEB58/5C, 3BN/3BR-proven; 3786x2730 = 1.387:1).
   The thunk exits into the intact 3BU write-back at 0x2F9D10; the 3BV
   payload stays byte-identical but unreachable.
2. `kHalo4RuntimeCapabilities` immediate 0xF3 -> 0x1F3 at 0x68112 (the
   compiled PublishHalo4Lifecycle ternary), adding
   `TitleCapability_CutsceneTheater` (1<<8) to the lifecycle publication.
3. Registry `kTitles[Halo4].capabilities` dword 0xF3 -> 0x1F3 at 0x1890D4
   (row verified: title 4, module L"halo4.dll", admission 0x40 untouched).
4. No menu change needed: the F1 "3D Theatre" category and the enable
   checkbox are title-blind and already live.

Everything downstream is the existing shared theatre
(`Game_GetCutsceneTheaterPresentation` 0x463A0, the transition/fade
compositor, cine bars, flip depth, subtitles): no title-specific
presentation code was added, exactly as the evidence doc's "Future titles"
contract prescribes.

## Expected result

- An in-engine Halo 4 cutscene fades (100 ms) onto the room-fixed stereo
  screen; it fades back at the handoff to gameplay. Pause/menu exits the
  theatre by publication staleness within ~600 ms.
- F1 Theatre settings and `cutscene_theater_enabled` govern Halo 4 exactly
  as the other titles; unchecking the box keeps cutscenes immersive.
- **Known 3BW limitation (next stage 3BX):** the captured eyes still carry
  head-look, so the picture on the screen pans when the head moves, and the
  theatre Depth slider does not yet scale Halo 4's capture separation. The
  authored-camera lock lands in 3BX.
- Other titles byte-identical in behavior; gameplay Halo 4 untouched
  (PlayerControlled publications cannot activate anything).

## Test

`python tools/test_stage3bw_h4_cutscene_theatre.py` - PASS (byte identity
outside the four regions; probe shape intact; exact call set; state map;
aspect divss; capability values 0x1F3 with neighbors stock; all prior
artifacts intact; 3BJ absent).

## Deployment

Installed 2026-08-30 into both editions (MCC confirmed closed); Stage 3BV
preserved with the 18:12 session log under
`out/deploy-backups/2026-08-30-pre-3BW/{steam,xbox}`.

## Headset test (plain language)

1. Play Halo 4 and let an in-engine cutscene play. It should fade onto a
   big fixed screen in front of you (with 3D depth), then fade back to
   normal gameplay when it ends.
2. Expect the picture to shift if you move your head during the cutscene -
   that is the known 3BW limit, fixed next stage. Judge entry, exit, the
   screen itself, and the F1 Theatre settings.
3. To test the off-switch: untick "Enable Stereo 3D Theatre for cutscenes"
   in F1 -> 3D Theatre and replay a cutscene; it should stay immersive.
4. Say which edition you used.
