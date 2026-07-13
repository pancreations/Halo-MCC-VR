#pragma once

// Settings live in halo3xr.cfg next to the DLL, as plain "key = value" text
// so users can edit them by hand. The in-headset menu edits the same values
// live and saves them back.

struct Config
{
    float screen_width_m = 4.0f;    // width of the virtual screen, in meters
    float screen_distance_m = 2.4f; // how far away the screen floats, in meters
};

extern Config g_config;

void ConfigLoad(const wchar_t* path); // missing file -> file is created with defaults
void ConfigSave();
