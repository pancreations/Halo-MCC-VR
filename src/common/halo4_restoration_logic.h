#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// C-H4-53 ports only bindings that already existed in the headset-tested
// Stage 3AI/3AW line. Keep the pure guards here so the package tests can prove
// the exact retail contracts without loading halo4.dll.

inline constexpr size_t kHalo4PauseReasonGetterBytes = 51;

inline bool Halo4PauseReasonGetterMatches(
    const uint8_t* bytes, size_t size) noexcept
{
    if (!bytes || size < kHalo4PauseReasonGetterBytes)
        return false;
    // game_is_paused_for_reason(int32), halo4+0xA0AE4. Bytes 2..5 are the
    // retail RIP displacement and are deliberately wildcarded.
    static constexpr uint8_t prefix[] = {0x8B, 0x15};
    static constexpr uint8_t suffix[] = {
        0x65,0x48,0x8B,0x04,0x25,0x58,0x00,0x00,0x00,
        0x41,0xB8,0x90,0x00,0x00,0x00,0x48,0x8B,0x04,0xD0,
        0x49,0x8B,0x14,0x00,0x32,0xC0,0x38,0x02,0x74,0x0F,
        0xB8,0x01,0x00,0x00,0x00,0x66,0xD3,0xE0,0x66,0x85,
        0x42,0x02,0x0F,0x95,0xC0,0xC3};
    return bytes[0] == prefix[0] && bytes[1] == prefix[1] &&
        std::equal(std::begin(suffix), std::end(suffix), bytes + 6);
}

inline bool Halo4EffectDescriptorIsLocalFirstPerson(uint8_t flags) noexcept
{
    return (flags & 0x0Fu) != 0 && (flags & 0xF0u) != 0xF0u;
}

struct Halo4HudAffine
{
    float horizontal = 1.0f;
    float vertical = 1.0f;
    float heightPixels = 0.0f;
};

inline bool Halo4ComputeNativeHudAffine(
    float hudSize, float hudAspect, float heightPixels,
    Halo4HudAffine& out) noexcept
{
    if (!std::isfinite(hudSize) || !std::isfinite(hudAspect) ||
        !std::isfinite(heightPixels))
        return false;
    out.vertical = std::clamp(hudSize, 0.30f, 1.0f);
    out.horizontal = std::clamp(
        out.vertical * hudAspect, 0.15f, 1.0f);
    out.heightPixels = std::clamp(heightPixels, -300.0f, 300.0f);
    return true;
}
