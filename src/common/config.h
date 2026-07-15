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

    // M2 stereo ghosting is fixed unconditionally in RenderViewHook (the
    // frame's post pass is anchored to the first eye instead of the centre).
    // Two renders per frame, no fps cost, no options: an fps-halving warm-up
    // pass was tried as a fallback and removed on request.

    // M2: give each eye a private copy of the bloom/exposure history buffers.
    // DISPROVEN as the ghost cause (discovery finds no qualifying target and
    // toggling it changes nothing). Kept only as a diagnostic switch.
    bool per_eye_history = false;

    // Halo's sun-shaft radial blur. Suspected as the ghost source, then
    // DISPROVEN by headset A/B: neutralizing it changed nothing. Default 1 =
    // leave the game's effect alone rather than disable it for no benefit.
    bool stereo_sun_shafts = true;

    // Which controller, held next to the head, turns the left stick into the
    // D-pad (UEVR-style gesture): 0 = left controller, 1 = right controller.
    int dpad_hand = 0;

    // Ghosting diagnostic: render the right eye before the left. If the
    // post-processing ghost trails move to the right lens, the cause is
    // confirmed as shared temporal history consumed by whichever eye goes first.
    bool right_eye_first = false;
};

extern Config g_config;

void ConfigLoad(const wchar_t* path); // missing file -> file is created with defaults
void ConfigSave();
