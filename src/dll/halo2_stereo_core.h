#pragma once

#include <cstddef>
#include <cstdint>

enum class Halo2StereoQuarantineReason : uint32_t
{
    None = 0,
    CoreClaimedTransactionFailed,
    CorePreparedSerialGap,
    VrClaimedPresentationUnavailable,
    VrClaimedPairUnavailable,
    VrClaimedSwapchainFailed,
    VrClaimedProjectionFailed,
    VrClaimedSubmissionFailed,
};

// Dormant until the title worker deliberately selects the synchronous Halo 2
// route. The temporal C-H2-2 core remains separate and must never be polled at
// the same time: both own render_player_window.
// `classicRenderTreeRuns` is E-H2-3: halo2.dll ships two renderers and the
// classic Blam tree this core hooks is skipped whole while the remastered
// Anniversary renderer owns the frame. Arming there would install hooks that
// can never fire, so the core stays stock and says so instead.
bool Halo2Stereo_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed,
    bool classicRenderTreeRuns) noexcept;

bool Halo2Stereo_Installed() noexcept;
bool Halo2Stereo_Armed() noexcept;
uint32_t Halo2Stereo_Generation() noexcept;
void Halo2Stereo_ShutdownForVrFailure() noexcept;
// Atomic-only. False immediately prevents a new H2 presentation claim. True
// only publishes readiness; the worker Poll still owns every other admission
// gate and must run before claims resume.
void Halo2Stereo_SetPresentationReady(bool ready) noexcept;
// Atomic-only, allocation-free, and first-reason-wins for an owned module
// generation. The worker consumes this request and removes only the H2 stereo
// hooks; the touched frame remains claimed/dropped while later frames fail open
// through the stock-screen presentation path.
void Halo2Stereo_RequestGenerationQuarantine(
    uint32_t generation, Halo2StereoQuarantineReason reason) noexcept;
// Atomic-only. The next valid exact-serial render sample becomes the new
// translation/orientation reference; safe to call from universal recenter.
void Halo2Stereo_RequestRecenter() noexcept;
