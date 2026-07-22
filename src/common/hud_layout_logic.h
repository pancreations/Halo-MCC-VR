#pragma once

#include <array>
#include <cstdint>

// Player-facing HUD layout behavior is shared across titles. Each title only
// supplies the immutable bytes that prove its native s_chud_curvature_info
// layout in loaded tag data.
enum class HudLayoutProfile : uint32_t
{
    None = 0,
    Halo3,
    Halo3ODST,
};

struct HudLayoutAdapter
{
    HudLayoutProfile profile;
    const char* name;
    std::array<uint8_t, 24> anchor;
    int expectedBlocks;
};

inline constexpr HudLayoutAdapter kHalo3HudLayoutAdapter = {
    HudLayoutProfile::Halo3,
    "Halo 3",
    {
        0x00, 0x05, 0x00, 0x00, 0xD0, 0x02, 0x00, 0x00, // 1280, 720
        0x00, 0x00, 0x5C, 0x42, 0x00, 0x40, 0x25, 0x44, // 55.0, 661.0
        0x00, 0x00, 0x68, 0x42, 0x00, 0x00, 0x80, 0x40, // 58.0, 4.0
    },
    3,
};

inline constexpr HudLayoutAdapter kOdstHudLayoutAdapter = {
    HudLayoutProfile::Halo3ODST,
    "Halo 3: ODST",
    {
        0x00, 0x05, 0x00, 0x00, 0xD0, 0x02, 0x00, 0x00, // 1280, 720
        0x00, 0x00, 0xFA, 0x44, 0x00, 0x00, 0xFA, 0x44, // 2000.0, 2000.0
        0x00, 0x00, 0x68, 0x42, 0x00, 0x00, 0x80, 0x40, // 58.0, 4.0
    },
    1,
};

inline constexpr const HudLayoutAdapter* HudLayoutAdapterFor(
    HudLayoutProfile profile)
{
    switch (profile)
    {
    case HudLayoutProfile::Halo3:
        return &kHalo3HudLayoutAdapter;
    case HudLayoutProfile::Halo3ODST:
        return &kOdstHudLayoutAdapter;
    default:
        return nullptr;
    }
}

inline constexpr bool HudLayoutPublicationMatches(
    HudLayoutProfile currentProfile, uint32_t currentGeneration,
    HudLayoutProfile publishedProfile, uint32_t publishedGeneration)
{
    return currentProfile != HudLayoutProfile::None &&
        currentProfile == publishedProfile &&
        currentGeneration == publishedGeneration;
}

inline constexpr bool HudLayoutCanReacquireFromRemembered(
    int rememberedCount, int expectedBlocks)
{
    return expectedBlocks > 0 && rememberedCount == expectedBlocks;
}
