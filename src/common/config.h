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

    // Hand-anchored first-person weapon: uniform size multiplier applied to
    // the whole arms+gun assembly around the wrist. 1 = the size the model
    // was authored at (the overlay frustum matches the world projection, so
    // authored size = true physical size). Home/End adjust it live.
    float gun_scale = 0.75f;

    // Fixed mounting rotation between the weapon bone's authored frame and
    // the controller (degrees). Defaults are identity: the -90 pitch guess
    // rotated the gun out of view (2026-07-15 04:3x). Tune LIVE in the F1
    // menu while looking at the gun; save keeps your calibration.
    float gun_pitch_deg = 0.0f;
    float gun_yaw_deg = 0.0f;
    float gun_roll_deg = 0.0f;

    // Experimental: scales the non-weapon overlay cameras' frustum (HUD).
    // 1 = untouched. >1 draws the HUD smaller / closer to center. The weapon
    // camera is never scaled (its projection must match the world exactly for
    // controller registration).
    float hud_scale = 1.0f;

    // DIAGNOSTIC (off by default). Ignores the controller and shoves the whole
    // composed first-person assembly a fixed distance to the left. Answers one
    // binary question that the disassembly cannot: does the visible gun MESH
    // consume the bone matrices we edit? See docs/CONTINUATION.md 2026-07-15.
    bool weapon_probe = false;

    // Ghosting diagnostic, and the one reliable way to reproduce the open
    // left-eye ghost bug on demand: render the right eye first and the trails
    // move to the right lens. See docs/CONTINUATION.md "KNOWN MAJOR BUG".
    bool right_eye_first = false;
};

extern Config g_config;

void ConfigLoad(const wchar_t* path); // missing file -> file is created with defaults
void ConfigSave();
