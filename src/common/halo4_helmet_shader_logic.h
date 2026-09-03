#pragma once

#include <cstddef>
#include <cstdint>

// The known-good V6 headset build identifies Halo 4's in-helmet visor/framing
// draw by this exact 3DMigoto FNV-1 pixel-shader hash. The supplied 7a24814
// runtime log proves both discovery and live checkbox suppression while radar,
// reticle, shields, ammo, and the remaining HUD shaders stay untouched.
namespace halo4_helmet_shader
{
inline constexpr uint64_t kVisorFramingHash = 0x4BE62AC49C2BF210ULL;

inline uint64_t MigotoFnv1(const void* data, size_t size) noexcept
{
    if (!data && size)
        return 0;
    constexpr uint64_t prime = 0x100000001b3ULL;
    uint64_t hash = 0;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index)
    {
        hash *= prime;
        hash ^= bytes[index];
    }
    return hash;
}

constexpr bool IsVisorFramingShader(uint64_t hash) noexcept
{
    return hash == kVisorFramingHash;
}

constexpr bool ShouldSuppress(
    bool shaderPathAvailable, bool halo4Active, bool helmetVisible,
    bool visorShaderBound) noexcept
{
    return shaderPathAvailable && halo4Active && !helmetVisible &&
        visorShaderBound;
}
}
