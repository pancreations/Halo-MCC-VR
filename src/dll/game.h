#pragma once

// The Halo 3 engine (halo3.dll) is loaded by MCC only once you actually enter
// a Halo 3 / ODST level — not at the main menu. This module waits for it to
// appear, then (in M1+) signature-scans it for the camera so head tracking can
// drive the in-game view. It runs on its own thread and never blocks rendering.

void Game_Init();

// True once halo3.dll is loaded and we've located what we need in it.
bool Game_IsHooked();
