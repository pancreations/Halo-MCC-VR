#pragma once

#include <cstddef>
#include <cstdint>

// C-H2-1. Samples the official game-time singleton and, only after a coherent
// post-initialization tick proves an active level, verifies the pinned retail
// identity and anchor table against the loaded halo2.dll image. Returns whether
// liveness has opened for this module generation. Read-only: no engine call,
// hook, write, controller admission, or OpenXR ownership is performed.
bool Halo2ColdObservation_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation) noexcept;

// Discards incomplete liveness samples when Halo 2 is no longer the uniquely
// active title. A completed generation remains latched and never scans twice.
void Halo2ColdObservation_Rearm() noexcept;

bool Halo2ColdObservation_Pending(uint32_t generation) noexcept;
bool Halo2ColdObservation_Passed(uint32_t generation) noexcept;
