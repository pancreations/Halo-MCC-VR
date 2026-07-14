#pragma once

// The Halo 3 engine (halo3.dll) loads only once you enter a level. This module
// waits for it, then (M1) drives the in-game camera from the headset. Runs on
// its own threads and never blocks rendering.

void Game_Init();
bool Game_IsHooked();
bool Game_IsHeadTracking(); // true while F2 head tracking is on

// Head-tracking controls, driven by hotkeys so we can tune it live in-headset.
void Game_ToggleHeadTracking(); // F2
void Game_Recenter();           // F3
void Game_FlipYaw();            // F4
void Game_FlipPitch();          // F5
void Game_TogglePositional();   // F6: leaning / positional tracking on/off
void Game_ToggleUp();           // F7
void Game_PitchTrim(int dir);   // F8 (down) / F9 (up): nudge pitch offset
void Game_LeanScale(int dir);   // PageDown / PageUp: leaning strength
