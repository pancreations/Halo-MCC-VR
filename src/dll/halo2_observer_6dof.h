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
// C-H2-43: independently installed firing-only direction hook. False keeps
// controller weapon placement dormant; it never controls camera admission.
bool Halo2Observer6Dof_DirectWeaponAimArmed() noexcept;
// C-H2-50: optional final visible-palette transaction. The controller-owned
// shot path is admitted only while this exact visual boundary is installed.
bool Halo2Observer6Dof_FinalPaletteArmed() noexcept;

// The pose the observer core applied on its most recent successful frame
// (E-H2-6 publication). Lock-free seqlock read, bounded; false when nothing
// has been published this generation or the read was torn four times.
struct Halo2ObserverPosePublication;
bool Halo2Observer6Dof_ReadPublishedPose(
    Halo2ObserverPosePublication& out) noexcept;
// E-H2-21: up to `maximum` recent publications, newest first, one per
// distinct tracked camera. Returns how many were filled.
int Halo2Observer6Dof_ReadPublishedPoses(
    Halo2ObserverPosePublication* out, int maximum) noexcept;

// E-H2-23 (C-H2-32): the ring index of the publication the most recent
// game-tick weapon placement was witnessed against, for this generation.
// False until a tick is witnessed, and false again after a tick that could
// not be witnessed (the previous witness never stands for a later tick).
// `previousIndex` is the tick before it (0 when there is none), so a renderer
// can follow the frame interpolator's blend between the two.
bool Halo2Observer6Dof_WeaponTickPublication(
    uint32_t generation, uint64_t& index, uint64_t& previousIndex) noexcept;

// E-H2-34 (C-H2-39): the stereo core that owns the frame names the cameras
// of the eye pass about to render - the centre the eyes render from, and for
// the classic pass the stale camera it draws from - so the weapon re-anchor
// moves the first-person geometry to exactly that pass. nullptr clears.
// Seqlocked copy; bounded; safe on the render thread.
struct Halo2FirstPersonPassCameras;
void Halo2Observer6Dof_SetFirstPersonPassCameras(
    const Halo2FirstPersonPassCameras* cameras) noexcept;

// Atomic-only. The next valid sample becomes the new orientation/translation
// reference; safe to call from the universal recenter path.
void Halo2Observer6Dof_RequestRecenter() noexcept;

// Atomic-only. Removes ownership when the VR runtime is gone.
void Halo2Observer6Dof_ShutdownForVrFailure() noexcept;
