#pragma once

#include <cstddef>
#include <cstdint>

// C-H2-8. Halo 2's per-user observer is the single camera root that BOTH of
// halo2.dll's renderers consume: the classic Blam tree reads it through the
// camera builder 0x7DF5A0 into the window cameras, and the remastered
// Anniversary (Saber GroundHog) renderer reads the same struct through the
// bridge 0x5F510, which builds its world matrix in metres. Writing a headset
// pose there therefore reaches whichever renderer is live, without forcing the
// player's graphics mode and without touching either renderer.
//
// The injection sits at the tail of the observer's LAST per-frame writer
// (0x6F0250). That ordering is load-bearing: the Saber bridge is invoked from
// inside observer_update_all itself (0x6F1C1F), so a pose applied after
// observer_update_all returns would already be too late for the remastered
// renderer.
//
// Only three non-contiguous 12-byte spans are ever written - position, forward
// and up. Field of view, aspect, cluster indices and velocity stay engine-owned.
// The engine rebuilds all three from its own state every frame (0x6F10E0), so
// an injected pose cannot accumulate and no restore is required.
bool Halo2Observer6Dof_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed,
    uintptr_t observerResultArray) noexcept;

bool Halo2Observer6Dof_Installed() noexcept;
bool Halo2Observer6Dof_Armed() noexcept;

// Atomic-only. The next valid sample becomes the new orientation/translation
// reference; safe to call from the universal recenter path.
void Halo2Observer6Dof_RequestRecenter() noexcept;

// Atomic-only. Removes ownership when the VR runtime is gone.
void Halo2Observer6Dof_ShutdownForVrFailure() noexcept;
