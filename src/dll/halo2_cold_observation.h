#pragma once

#include <cstddef>
#include <cstdint>

#include "../common/halo2_render_logic.h"

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

// C-H2-73. Read-only reuse of the same H2EK-proven game_time_globals sample
// used by the cold-observation gate. Returns false rather than manufacturing a
// value if the slot/object is absent, moving, unreadable, or incoherent.
bool Halo2ColdObservation_ReadGameTime(
    bool& initialized, uint32_t& tick) noexcept;

// E-H2-3. Which halo2.dll renderer owns the frame this module generation.
// Halo 2 ships two: the classic Blam tree and the remastered Anniversary
// (Saber GroundHog) renderer. The classic tree is skipped whole while the
// remastered one is live, so a classic-path render hook can install correctly
// and still receive zero callbacks. Returns Unknown until the C-H2-1 cold
// observation has passed for that exact generation.
Halo2GraphicsMode Halo2ColdObservation_GraphicsMode(
    uint32_t generation) noexcept;

// Convenience: true only while the classic render tree actually executes.
bool Halo2ColdObservation_ClassicRenderTreeRuns(uint32_t generation) noexcept;

// E-H2-4. Signature-resolved address of observer result 0, the camera root
// both renderers consume. Zero until proven for that generation.
uintptr_t Halo2ColdObservation_ObserverResultArray(
    uint32_t generation) noexcept;
