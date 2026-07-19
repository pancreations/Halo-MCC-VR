#pragma once

// Settings live in halo3xr.cfg next to the DLL, as plain "key = value" text
// so users can edit them by hand. The in-headset menu edits the same values
// live and saves them back.

struct Config
{
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

    // Hide the game's centered weapon reticle. It's head-locked (causes eye
    // strain in VR) and redundant with our floating VR reticle. This works by
    // skipping ONE HUD element at draw time — but the reticle's element id is
    // runtime tag data, so the user identifies it in the menu (reticle_element_id
    // below) by stepping until the crosshair disappears. ON = hide that element.
    bool kill_reticle = true;
    // Which HUD element id to hide as the reticle. -1 = none picked yet. Set from
    // the in-headset menu once the user finds the id that removes the crosshair.
    int reticle_element_id = -1;

    // Game brightness / gamma (0x278EE0's screen color constant). 1.0 = the game's
    // own brightness; higher = brighter, lower = darker. NOT a HUD control — the
    // function once thought to size the HUD actually adjusts brightness.
    float game_brightness = 1.0f;

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

};

extern Config g_config;

void ConfigLoad(const wchar_t* path); // missing file -> file is created with defaults
void ConfigSave();
