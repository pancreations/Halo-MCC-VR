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

    // Ghosting diagnostic, and the one reliable way to reproduce the open
    // left-eye ghost bug on demand: render the right eye first and the trails
    // move to the right lens. See docs/CONTINUATION.md "KNOWN MAJOR BUG".
    bool right_eye_first = false;
};

extern Config g_config;

void ConfigLoad(const wchar_t* path); // missing file -> file is created with defaults
void ConfigSave();
