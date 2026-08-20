#pragma once

#include <cstddef>
#include <cstdint>

// C-H2-2's isolated, position-only temporal-stereo hook core. The title
// worker is the sole caller of Poll; the remaining accessors are lock-free so
// shared lifecycle code can publish an exact runtime state without reaching
// into the hook implementation.
bool Halo2TemporalStereo_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed) noexcept;

bool Halo2TemporalStereo_Installed() noexcept;
bool Halo2TemporalStereo_Armed() noexcept;
uint32_t Halo2TemporalStereo_Generation() noexcept;

// Immediate, non-blocking revocation used when OpenXR can no longer submit.
// The title worker performs verified MinHook cleanup on its next poll.
void Halo2TemporalStereo_ShutdownForVrFailure() noexcept;
