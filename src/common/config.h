#pragma once

// Settings live in halomccvr.cfg next to the DLL, as plain "key = value" text
// so users can edit them by hand. The in-headset menu edits the same values
// live and saves them back.

struct Config
{
    int config_version = 1;

    // Portable OpenXR feedback and controller-ray stabilization. These are
    // shared defaults; title adapters may disable features they cannot prove.
    float haptic_intensity = 0.70f;
    float aim_stabilization = 0.35f;

    float screen_width_m = 4.0f;    // width of the virtual screen, in meters
    float screen_distance_m = 2.4f; // how far away the screen floats, in meters

    // M3 VR controller turning (right Sense stick).
    bool turn_smooth = false;          // false = snap turn, true = smooth turn
    float turn_snap_deg = 30.0f;       // degrees per snap
    float turn_smooth_deg_s = 120.0f;  // smooth turn speed, degrees/second

    // Which controller, held next to the head, turns the left stick into the
    // D-pad (UEVR-style gesture): 0 = left controller, 1 = right controller.
    int dpad_hand = 0;

    // Aim crosshair (stereo only): a small reticle floating where the weapon
    // actually shoots. Drawn as a compositor quad layer, so it costs no game
    // rendering. The game's own HUD reticle sits at head-center and is wrong
    // whenever hand aim is on; this one is the truth.
    bool crosshair = true;
    float crosshair_distance_m = 10.0f; // how far along the aim ray it floats
    float crosshair_size_deg = 2.25f;   // apparent (angular) size

    // Crosshair color (0-1 per channel). Default approximates Halo 3's own
    // light CHUD blue; edit live in the F1 menu (a reset button restores it).
    float reticle_r = 0.62f;
    float reticle_g = 0.87f;
    float reticle_b = 1.0f;

    // Hand-anchored first-person weapon: uniform size multiplier applied to
    // the wrist subtree (hand + weapon) around the wrist. Under the true
    // world projection the authored viewmodels read oversized; 0.85 is the
    // new-pipeline starting point. Home/End adjust live.
    float gun_scale = 0.85f;

    // (gun_length_scale removed 2026-07-19: a barrel-only squash is not
    // expressible in the engine's uniform-scale bone format; moving bone
    // origins just translated the rigid gun mesh.)

    // Fixed mounting rotation between the weapon bone's authored frame and
    // the controller (degrees). Rotates ONLY the visible gun + muzzle flash;
    // the cursor/bullet ray stays fixed on the controller, so tune these
    // until the barrel lies on the cursor line. Tune LIVE in the F1 menu;
    // save keeps your calibration.
    float gun_pitch_deg = 0.0f;
    float gun_yaw_deg = 0.0f;
    float gun_roll_deg = 0.0f;

    // Push the whole arms+gun assembly along the controller's forward axis,
    // in meters. 0 = anchored at the controller; negative seats the gun back
    // into/behind your fist (the practical "gun feels too long" trim);
    // positive moves it out of your face. Never touches aim.
    float gun_forward_m = 0.0f;

    // (show_hud / hud_ammo / hud_health / hud_motion / hud_grenades retired
    // 2026-07-19 evening: their chud+0x144..0x14A byte writes used a
    // headset-disproven offset map and suppressed the whole HUD except the
    // objective text. The native HUD is fully game-managed now; the only
    // element control is the reticle kill below.)

    // Hide every native CHUD widget collection marked scripting class
    // crosshair. The head-locked game cursor is redundant with our VR reticle.
    bool kill_reticle = true;

    // Game brightness / gamma (0x278EE0's screen color constant). 1.0 = the game's
    // own brightness; higher = brighter, lower = darker. NOT a HUD control — the
    // function once thought to size the HUD actually adjusts brightness.
    float game_brightness = 1.0f;

    // Halo's internal raster preset, applied by the launcher on the next game
    // start: potato .50, low .67, medium .80, high 1.00, ultra 1.10.
    // The OpenXR projection remains at the headset's full size.
    float resolution_scale = 1.0f;

    // (The 0x2EEFC8 placement-slider experiment is retired: measured 2026-07-19,
    // that struct holds colors/alpha/animation only — Halo's HUD has no position
    // data to edit. The HUD panel below is the real fix.)

    // (HUD sizing experiments before 2026-07-19 PM are retired: the capture-diff
    // panel was headset-disproven and the hud_zoom [view+0x2B0]+0x174 poke never
    // resized anything. hud_size below is the one that works — it is DATA, not
    // code: Halo's own layout input.)

    // HUD size: the fraction of the view the HUD lays out into. This drives
    // Halo's own "global safe frame" floats inside the loaded chud_globals tag
    // data (located at runtime by their immutable byte neighborhood; proven in
    // H3EK tag_test on desktop AND live in MCC, 2026-07-19 probe: the engine
    // re-lays the HUD out the same frame the floats change). 0.87 = the game's
    // stock value (mod applies nothing); smaller pulls shields/radar/ammo
    // toward the screen center where both VR eyes can see them.
    float hud_size = 0.87f;

    // Automatically enter VR (head tracking + stereo) when a level loads, and
    // drop back to the flat menu screen when you leave — no F2/F11 needed.
    bool auto_vr = true;

    // Two-handed weapon aiming: when you bring your left hand up to the gun
    // (support-hand grip), aim along the line from the right hand to the left
    // hand instead of the right wrist alone — steadier, and the barrel points
    // exactly where you look down the gun. Auto-engages by hand pose; drops
    // when you lower the support hand. The right grip still cycles grenades.
    bool two_handed_aim = true;
    // Two-hand engage style: true = toggle (click left grip on/off), false =
    // hold (engaged only while the left grip is held).
    bool two_hand_toggle = true;
    // Wrist-to-palm correction for the left controller. This same point drives
    // the rendered support hand and the two-hand aiming line so they stay aligned.
    float left_hand_forward_m = 0.12f;
    // Sideways nudge of the two-hand grab zone along the right controller's +X
    // (positive = toward the player's right) so the grab line sits on the
    // visible barrel. Headset request 2026-07-19: the AR's barrel sat right of
    // the zone and the left hand reached past it.
    float two_hand_zone_right_m = 0.03f;

    // VRIK stage A1: show the player's real body (game-animated) by flipping
    // the engine's director/viewmodel switches. Experimental gate for the
    // upper-body VRIK plan (docs/VRIK-ROADMAP.md).
    bool body_wip = false;

    // VRIK arm IK: bend the first-person arm (shoulder planted, elbow solved,
    // hand+gun to the controller) instead of rigid-parenting the whole
    // assembly. ON = articulated arm; OFF = the previous rigid parent.
    bool arm_ik = true;

    // Lower the RIGHT (weapon) shoulder so Master Chief's arm doesn't clip up
    // into your face — drops the shoulder anchor along your view-down axis.
    // 0 = the game's authored (high) shoulder; higher = lower shoulder. Tune
    // to match your left shoulder. In world units (~1 wu = 3 m).
    float right_shoulder_drop = 0.06f;

    // Keep the IK shoulders LEVEL with the horizon instead of pitching with your
    // head — anchor the arms to a torso frame that turns with your heading (yaw)
    // but not pitch/roll, so looking up/down no longer swings the shoulder into
    // your face. The gravity/up axis is now MEASURED from the engine's camera-up
    // (the first attempt hardcoded the wrong axis). ON by default; OFF = the old
    // behavior (shoulders ride the camera). Hand and gun are unaffected either way.
    bool shoulder_level = true;

    // Halo 3's camera motion blur. In two-render stereo its "previous frame"
    // camera is the other eye's, smearing bright content into repeated echoes
    // (the long-standing first-eye ghost). Off is also the VR comfort
    // standard. 0 = blur scales forced to zero (default), 1 = engine values.
    bool motion_blur = false;

    // (bullet_snap retired: the composed-wrist snap spun the right hand and
    // sent bullets stage-left; reverted. The real fix is a runtime fire hook —
    // see docs and the bullet_probe diagnostic below.)

    // DIAGNOSTIC (off by default). Ignores the controller and shoves the whole
    // composed first-person assembly a fixed distance to the left. Answers one
    // binary question that the disassembly cannot: does the visible gun MESH
    // consume the bone matrices we edit? See docs/CONTINUATION.md 2026-07-15.
    bool weapon_probe = false;

    // DIAGNOSTIC (off by default). Logs the CHUD state-byte window whenever it
    // changes, to locate the reticle "on target" (enemy red) state and the
    // per-element visibility flags. Log-only; changes nothing.
    bool hud_probe = false;

    // DIAGNOSTIC (off by default; set bullet_probe=1 in the cfg for the
    // fire-hook hunt). On each shot, logs the camera (where Halo spawns your
    // bullet) vs the gun muzzle world position, to measure the "bullets from
    // thin air" gap. Log-only; the actual origin-move needs a runtime fire hook.
    bool bullet_probe = false;

    // Ghosting diagnostic, and the one reliable way to reproduce the open
    // left-eye ghost bug on demand: render the right eye first and the trails
    // move to the right lens. See docs/CONTINUATION.md "KNOWN MAJOR BUG".
    bool right_eye_first = false;
};

extern Config g_config;

void ConfigLoad(const wchar_t* path); // missing file -> file is created with defaults
void ConfigLoadMigrating(const wchar_t* primaryPath, const wchar_t* legacyPath);
void ConfigSave();
