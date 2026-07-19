#pragma once

// The Halo 3 engine (halo3.dll) loads only once you enter a level. This module
// waits for it, then (M1) drives the in-game camera from the headset. Runs on
// its own threads and never blocks rendering.

void Game_Init();
bool Game_IsHooked();
bool Game_IsHeadTracking(); // true while F2 head tracking is on

// HUD size: config hud_size drives Halo's own safe-frame floats in the loaded
// chud_globals tag data (auto-located at runtime; proven live 2026-07-19).
void Game_LocateHudSafeFrames();  // manual rescan (menu button; normally automatic)
void Game_GetHudSafeFrameStatus(int& matches, bool& scanning);

// Head-tracking controls, driven by hotkeys so we can tune it live in-headset.
void Game_ToggleHeadTracking(); // F2
void Game_AutoVrTick();         // per-frame: auto-enter/exit VR on level load/exit
void Game_Recenter();           // F3
void Game_FlipYaw();            // F1 menu only (was F4: SteamVR's Alt+F4 kept triggering it)
void Game_FlipPitch();          // F1 menu only (was F5)
void Game_TogglePositional();   // F6: leaning / positional tracking on/off
void Game_ForcePositional();    // stereo VR requires 6DOF
void Game_ToggleUp();           // F1 menu only (was F7)
float Game_GetYawSign();        // current calibration state, shown in the menu
float Game_GetPitchSign();
bool Game_GetWriteUp();
void Game_PitchTrim(int dir);   // F8 (down) / F9 (up): nudge pitch offset
void Game_LeanScale(int dir);   // PageDown / PageUp: leaning strength
void Game_GunScale(int dir); // Home (bigger) / End (smaller): hand-anchored weapon mesh size
void Game_ToggleVrAim();        // Insert: right controller steers the weapon aim

// M3 VR aim: called by the XInput hook on the game's input thread. Returns
// the right-stick deflection (-1..1) that turns the game's aim toward the
// right controller ray; false = leave the player's real stick alone.
bool Game_ComputeAimStick(float& outRx, float& outRy);
// Rotates a move-stick vector so pushing forward walks toward the gaze
// instead of the hand-steered aim heading. No-op when VR aim is inactive.
void Game_MapMoveStick(float& mx, float& my);
// Hooks XInputGetState in every loaded xinput DLL; returns how many are
// hooked. Safe to call repeatedly until it succeeds.
int Input_InstallXInputHook();
// Writes our shims into MCC's import table for xinput1_3 (Steam Input patches
// the same slots, bypassing DLL-level hooks). Call repeatedly to re-assert.
int Input_ClaimXInputIat();


// M2 alternate-eye proof. -1 removes the stereo eye offset; 0/1 selects the
// left/right render camera for the next game frame.
void Game_SetStereoEye(int eye);
float Game_GetWorldScale();
// >1 while the player is zoomed (weapon scope); 1.0 at hip. Drives the scope.
float Game_GetZoomFactor();
// The mount-trimmed controller-local aim direction (unit, OpenXR local axes).
// Shared by bullet steering (game.cpp) and the reticle (vr.cpp) so barrel,
// flash, reticle and bullets stay on one ray as the user trims the mount.
// Symmetric half-frustum tangents from Halo's active world camera.
void Game_GetProjectionTangents(float& tanX, float& tanY);
void Game_GetRenderHalfFov(float& halfX, float& halfY);
