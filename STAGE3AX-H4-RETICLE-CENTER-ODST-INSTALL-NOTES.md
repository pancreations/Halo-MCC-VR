# Stage 3AX — Halo 4 reticle-centre isolation + ODST deterministic install

Status: **HEADSET PENDING**. Stage 3AW's pause result is accepted by the player
(`pause menu fixed`) and its complete pause helper is byte-identical here.
`docs/CURRENT-STATE.md` is not advanced until the remaining headset checks pass.

Input Stage 3AW SHA-256:
`AD0C6BBCA337F2436A258CB4A0CB9DA5884B20270BC2F9DADF7A06DABA1ED676`.

Output Stage 3AX SHA-256:
`0D32751585670C28BB7B98110A35B04817EC4F683FCC3AB3301C0941A4613053`.

## Halo 4 reticle

The 2026-08-28 11:18 Steam/SteamVR/Oculus/120 Hz run is preserved under
`out/test-runs/stage3aw-h4-pause-pass-visor-capture-odst-no-arm-steam-20260828-1118/`
(log SHA-256
`3AC9DF41C8E47664F36B48EBED12F9EF49D170F002F670B5F5F962FCF9AD63BE`).
It proves Stage 3AW captured stable nonblank pixels (`art 819..847`, probe
alpha about 823) but the player identified them as a visor fragment rather
than the reticle.

H4EK's canonical weapon screen places the actual reticle under the centred
`reticule_offset_container`; visor/HUD content is outside that tree. Stage 3AX
retains Stage 3AW's selected runtime transform and finite guards, but tightens
the centred capture from 2x to 4x authored magnification:

```text
Width  = 8 * abs(baseX)
Height = 8 * abs(baseY)
Left   = 256 - 4 * abs(baseX)
Top    = 256 - 4 * abs(baseY)
```

The private target and scissor remain 512x512. This is a spatial isolation of
the proven authored centre: outer visor pixels move outside the target while
the centre reticle, hit art and spread art remain eligible. It adds no new
hook, allocation, scan, COM call, log, or file access to the CUI hot path.
Invalid transforms still refuse only authored capture and keep the procedural
fallback.

## ODST

In the same run, ODST's level-liveness gate correctly proved frozen-then-
ticking at 11:19:20.554, but installation stayed behind the camera-tail sample:
`tail=[0,1,0,0,0,0,0,1]`, `blend=0`. The compiled binary already passes
`requireFirstPerson=false`; blend-zero opening cameras were not the remaining
blocker.

ODST's tail boolean is documented and measured to toggle about 10 Hz during
ordinary play. Making installation depend on polling its zero phase recreates
the old luck-based startup behavior. Stage 3AX keeps the full camera-mode call
and all layout/signature checks, but after the independent frozen-then-ticking
level proof it always continues to hook installation. The existing post-install
fresh-camera debounce remains unchanged and is still the only edge that arms
stereo; it tolerates tail gaps up to 350 ms and cannot arm on an invalid camera.
Loading screens still cannot install because the liveness gate precedes this
site.

## Static surface

- File size unchanged: 2,919,424 bytes.
- 222 bytes differ from Stage 3AW, all inside the guarded reticle call, the
  one-byte ODST post-liveness branch, and zero-filled `.s3qd` RVA
  `0x2FA9A0..0x2FAA9D`.
- Stage 3AW pause payload `0x2FA890..0x2FA990` is byte-identical.
- `tools/test_stage3ax_h4_reticle_center_odst_install.py`: PASS.
- Stage 3AW static acceptance and dual-edition H4 pause binding: PASS.
- Historical Stage 3AL ODST teardown/unpin regression: PASS.
- Reach consistency gate: PASS.

## Headset oracle

1. Halo 4: the gun-ray quad shows the weapon's actual reticle rather than a
   visor fragment; the flat duplicate remains absent.
2. Halo 4 pause: remains visible and resumes to stereo (regression only).
3. ODST: after the level-liveness line, the camera core installs, arms, and
   gameplay/cutscenes render in stereo 3D.

## Deployment

Installed to both Steam and Microsoft Store on 2026-08-28 after confirming MCC
and the launcher were closed. Both installed DLLs independently hash to
`0D32751585670C28BB7B98110A35B04817EC4F683FCC3AB3301C0941A4613053`.
The exact prior Stage 3AW files from both editions are preserved under
`out/deploy-backups/2026-08-28-pre-3AX/`; both backed-up DLLs hash to
`AD0C6BBCA337F2436A258CB4A0CB9DA5884B20270BC2F9DADF7A06DABA1ED676`.
No configuration or launcher was changed, and MCC was not launched.
