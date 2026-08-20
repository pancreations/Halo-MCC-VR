#pragma once

#include <cstddef>
#include <cstdint>

// Dormant until the title worker deliberately selects the synchronous Halo 2
// route. The temporal C-H2-2 core remains separate and must never be polled at
// the same time: both own render_player_window.
bool Halo2Stereo_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed) noexcept;

bool Halo2Stereo_Installed() noexcept;
bool Halo2Stereo_Armed() noexcept;
uint32_t Halo2Stereo_Generation() noexcept;
void Halo2Stereo_ShutdownForVrFailure() noexcept;
// Atomic-only. The next valid exact-serial render sample becomes the new
// translation/orientation reference; safe to call from universal recenter.
void Halo2Stereo_RequestRecenter() noexcept;
