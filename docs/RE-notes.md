# Verified reverse-engineering map

Target used for these findings: MCC Halo 3 build 1.3528.0.0. RVAs are evidence for this build, not stable addresses. Production code must locate each boundary with a unique AOB signature and fail safely if the match is missing or ambiguous.

## Camera and stereo

- halo3+0x2A628C: camera-copy function, fastcall(dst, src). Source projection tangents are at src+0x68/+0x6C; the hook is the stable head/look state boundary.
- halo3+0x17DF44: `observer_apply_camera_effect`. Static disassembly and the
  matching 0x3D0-byte observer stride show that it reads the already-computed
  observer position/forward/up, composes Halo's authored camera-effect
  transform, and writes those three fields back. The production comfort hook
  bypasses this stage only while VR head tracking is active, removing recoil
  and other artificial screen shake without filtering locomotion or headset
  leaning. Its 60-byte AOB is unique in the 1.3528 DLL; a missing or ambiguous
  match leaves stock behavior active. Firing-recoil suppression was
  headset-confirmed on 2026-07-20.
- halo3+0x286A14: inner prepared-view renderer used for the two eye passes.
- The view contains current and first-person camera/derived pairs. FpDriverHook and FpCameraRebuildHook stamp the active eye values immediately before the corresponding first-person draw/upload.
- First-person camera upload is called through Halo's own uploader after a stamp. Do not replace this with a guessed constant-buffer hook.

## First-person skeleton

- Engine TLS +0x568 reaches per-player first-person weapon state; TLS +0x560 reaches the two animation-bank snapshots.
- halo3+0x184B08: first-person interpolation boundary. This supplies the authored render pose used by marker/effect and visible-palette consumers.
- 0x23200C / 0x2320B8: hierarchy composers used by the first-person evaluation path.
- The visible-palette hook is the safe boundary for arm IK and render-palette substitution. Bone count and all indices are bounded to 64.
- The right chain uses the verified runtime hand chain; the left chain is derived from the runtime skeleton topology. Do not substitute guessed string ids.
- Marker/muzzle bones and the visible palette use the same DesiredWristWorld transform so the weapon and effects stay coherent.
- Halo's later weapon-lag pass can separate post-edited composed bones from marker transforms. Do not restore the retired composed-output rewrite.

## Shotgun support hand

H3EK export showed the shotgun has a weapon-IK block mapping left_hand to a pump marker, while the assault rifle comparison has no such block. Runtime disassembly then identified the stock decision at halo3+0x2C393C.

The production startup patch:

- requires a unique signature;
- verifies bytes at signature +3 are exactly 74 05;
- writes EB 18 under temporary writable protection;
- flushes the instruction cache;
- selects Halo's existing no-weapon-IK path at halo3+0x2C3959;
- logs and leaves stock behavior untouched on any mismatch.

The user confirmed that this frees the shotgun left arm. The removed synthetic palette fallback did not.

## HUD and picture

- halo3+0x2EDF24 matches ManagedDonkey's chud_draw_widget: r8w is a runtime
  chud_definition tag index, while descriptor bytes +3/+4 select the widget
  collection/widget. The old picker therefore selected tag indices, not stable
  crosshair element ids.
- A complete H3EK sweep of all 65 ui/chud definitions confirms that native
  reticles use widget-collection scripting class 2 (crosshair). Collection
  indices vary by weapon and non-weapon HUD tags reuse those indices.
- halo3+0x2EDF24 calls game_is_playback first. Normal play short-circuits around
  both the class-2 comparison and halo3+0x2EDE38, explaining why hooking
  0x2EDE38 alone did nothing in-headset.
- The production path in commit c923842 NOPs only that validated short-circuit and hooks
  0x2EDE38. Halo still performs its own tag lookup and class comparison; the
  hook returns hidden only after Halo identifies class 2. No runtime tag-table
  dereferences were added to the render hook.
- Headset testing across multiple weapons confirmed that this removes the native
  crosshairs while preserving the VR reticle and the rest of the HUD, without
  reproducing the earlier black-screen failure.
- The discarded f0d5a88 classifier performed runtime tag-table dereferences from
  the element-submit hook. It caused a black headset view when stereo entered a
  level and was narrowly removed by 7fdf019. Never restore that lookup path.
- The native HUD is the accepted rendering path.
- The function once investigated as CHUD scale near 0x278EE0 controls screen brightness/alpha. It is used only for the brightness setting.
- Direct CHUD state-byte writes and capture/diff HUD extraction are disproven and removed.
- HUD layout size is DATA, not code (2026-07-19): the chud_globals tag's "curvature infos" blocks (one per skin: default/dervish/monitor) hold two "global safe frame" floats (shipped 0.87f, bits 0x3F5EB852) that scale the whole HUD layout toward screen center. Proven twice: H3EK tag_test re-laid the HUD out at 0.5 (desktop observation), and MCC consumes the floats per frame (live pokes resized the HUD instantly in-headset).
- At runtime those floats live in loaded tag data (private read-write heap, not the module image). They are located by the immutable 24-byte prefix [int32 1280][int32 720][55.0f][661.0f][58.0f][4.0f] (virtual canvas, sensor origin/radius, blip radius) with the safe-frame pair at +24/+28. Exactly 3 blocks exist and all 3 were found with bit-exact payloads (log-verified). A hit counts only when the prefix and plausible layout payload agree; every later write repeats those checks and rejects a stale slot.
- The same authoritative `s_chud_curvature_info` layout places `destination_offset_z` four bytes before that immutable prefix (safe-frame slot -28). Quest 3 testing proved this changes HUD curvature/depth, not vertical placement. `hud_curvature` maps 0.00 flat to a +0.30 delta and 1.00 fully curved to -0.30; 0.50 restores the retained per-skin authored baseline. Config version 2 migrates the earlier signed/one-tenth scale and the first-build `hud_height` alias without a visual jump.
- `hud_aspect` is a separate 0.50..2.00 horizontal multiplier applied after the runtime headset-aspect correction. It lets the user counter the safe-frame size control's perceived squeeze without coupling width to curvature or changing the vertical safe-frame value.
- True HUD height (2026-07-21): `chud_compute_anchor_basis` is signature-located at runtime and its matrix position Y (`basis+0x2C`) is translated by `hud_vertical_offset` (-300..+300 virtual pixels). Positive raises and negative lowers the complete CHUD. The hook is VR-only and skips the scripting-class-2 authored reticle while it is redirected into the controller-ray capture, preserving aim placement.
- Runtime chud_definition tag indices such as 0x62C, 0xF70, and 0x1A90 are
  observations, not portable element identifiers. Do not use them as defaults.
- HUD aspect correction is derived from the runtime per-eye FOV tangents. The
  Halo render aspect is compared with the runtime tangent-space aspect, hud_size
  remains the larger safe-frame bound, and only the other axis is reduced.
  Commit 1b53139 was headset-confirmed on Quest 3 and PSVR2 with OpenXR Toolkit
  disabled. It is a substantial improvement, although a mild squeeze remains.

## Resolution and upscaling

- Resolution scaling changes Halo's internal 2912x2100 raster before launch; it
  does not change the runtime-recommended OpenXR stereo-array swapchain or the
  submitted imageRect.
- The launcher scales both dimensions by the same preset and rounds to even
  values. The existing normalized eye blit then expands the complete source eye
  into the full-size OpenXR projection, preserving the lens frame and aspect.
- The restart-applied presets are Potato 50% (1456x1050), Low 67% (1952x1408),
  Medium 80% (2330x1680), High 100% (2912x2100), Ultra 110% (3204x2310), and
  Keith David 150% (4368x3150). Legacy or arbitrary config values normalize to
  the nearest tier; the tier boundaries are the midpoints 0.585, 0.735, 0.90,
  1.05, and 1.30, applied identically in config.cpp, launcher.cpp, and menu.cpp.
- Low 67% is headset-confirmed with Toolkit scaling disabled. The other tiers
  require headset coverage; do not describe them as validated yet.
- Enabling OpenXR Toolkit FSR produced tiled/overlapping regions in the observed
  VR View, consistent with an incompatibility around the current stereo-array
  presentation rather than ordinary low-resolution softness. The exact
  third-party cause was not proven.
- Product decision: do not add a built-in FSR path or an FOV slider. The supported
  performance control is the internal resolution preset; third-party upscalers
  remain outside the mod's supported render path.

## Input and firing

- OpenXR actions are merged into XInput slot 0.
- Controller aim is converted into Halo's normal aim steering so projectile direction, target logic, vehicles, and turrets remain game-owned.
- Halo still owns projectile origin. There is no verified fire-origin hook.
- The visible weapon mount trim must not rotate the reticle/projectile ray; otherwise calibration cannot converge.

## Native pause state

- H3EK/HaloScript evidence exposes an external boolean named `game_paused`, but
  live MCC pause/unpause testing proved that record remains zero. It is a
  developer override, not the native MCC pause-menu state.
- Four read-only module snapshots (paused, unpaused, paused, unpaused) reduced
  the state search to repeated candidates. A 2 ms timing trace then showed
  `halo3.dll+0xA3CA9A` changing before MCC's generic suspension flags on both
  entry and exit.
- Production code does not retain that RVA. A unique 45-byte owner signature at
  `halo3.dll+0xB682` resolves the final RIP-relative write target and requires a
  boolean initial value. Missing, ambiguous, out-of-module, or non-boolean
  results disable authority and retain the controller-edge fallback.
- When the native flag is available, it owns the head-locked 2D/stereo 3D
  transition and the camera-heartbeat restart heuristic is disabled. This is
  intended to fix Restart Level clearing Halo's pause state without a matching
  controller edge; headset confirmation is still required.

## Forbidden and fragile boundaries

- Never hook halo3+0x120DF8. A pass-through MinHook detour crashed on level load.
- Do not write gameplay roots, animation banks, or CHUD bytes from guessed offsets.
- Do not perform module scans, logging bursts, file I/O, allocations, COM calls, or resource census work in render/palette hot paths.
- Do not dereference runtime tag-table structures from the CHUD submit/visibility
  hot path, and do not interpret r8w as a stable widget or reticle id.
- Do not implement resolution scaling by shrinking the OpenXR swapchain or
  submitted imageRect. Keep the projection full-sized and scale Halo's source
  raster uniformly.
- Never treat an RVA in this document as a shipping address. The matching signature in source is the implementation authority.
