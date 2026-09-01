#pragma once

#include <cstdint>

// H4EK's m30_cryptum boosted-Ghost area_screen_effect selects the shared
// `screen\motion_suck` material shader. The official screen shader bank's
// complete PC DXBC blob is byte-identical to the blob in retail
// m30_cryptum.map and has this 3Dmigoto FNV-1 hash. Suppressing this one pixel
// shader removes the incompatible radial scene-history sample while leaving
// the separate speed-line/tint shader and every other post-process untouched.
namespace halo4_screen_effect_shader
{
inline constexpr uint64_t kMotionSuckHash = 0x47668A1953271934ULL;

constexpr bool IsMotionSuckShader(uint64_t hash) noexcept
{
    return hash == kMotionSuckHash;
}

constexpr bool ShouldSuppress(
    bool shaderPathAvailable, bool halo4Active, bool stereoEnabled,
    bool motionSuckShaderBound) noexcept
{
    return shaderPathAvailable && halo4Active && stereoEnabled &&
        motionSuckShaderBound;
}
}
