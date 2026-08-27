#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "halo2_render_logic.h"

// Runtime identities taken from the current Toggle HUD for Halo 2 v1.2
// 3DMigoto replacement set. That mod changes the actual shipping HUD by
// selecting these original pixel shaders, so these are render-path evidence,
// not inferred engine addresses. Its d3dx.ini explicitly uses 3DMigoto's
// traditional unseeded 64-bit FNV-1 shader hash.
namespace halo2_hud_shader
{
enum class Role : uint8_t
{
    Other = 0,
    GameplayHud,
    Crosshair,
};

inline constexpr uint64_t kCrosshairHash = 0x0a9b60d8f40268f6ULL;

inline constexpr std::array<uint64_t, 15> kGameplayHudHashes{{
    0x417f1685ec6744aaULL, // shield
    0x50e9b7e9476762fcULL, // shield meter
    0x5138655c43bc2879ULL, // ammunition
    0x740e59c9d34791b5ULL, // weapon icons
    0xbdfb9aa0140f7f34ULL, // round icon
    0xc7f2063e1d1d87dfULL, // motion sensor
    0xf972418445d6e438ULL, // pickup notification
    0x11deb23283167559ULL, // multiplayer CUI / HUD
    0x1c889b6efc492695ULL,
    0x1fe71a633df057f9ULL,
    0x7681944ad6937bd6ULL,
    0xc975aaa399a4ae72ULL,
    0xf0540984f5878178ULL,
    0xf665e50c823d4832ULL,
    0x2c96ddc4df718dd8ULL,
}};

inline constexpr Role Classify(uint64_t hash) noexcept
{
    if (hash == kCrosshairHash)
        return Role::Crosshair;
    for (const uint64_t gameplayHash : kGameplayHudHashes)
        if (hash == gameplayHash)
            return Role::GameplayHud;
    return Role::Other;
}

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

struct RasterAffine
{
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
};

inline bool BuildRasterAffine(
    const Halo2CameraRectangle& source,
    const Halo2CameraRectangle& layout,
    RasterAffine& output) noexcept
{
    const float sourceWidth =
        static_cast<float>(static_cast<int32_t>(source.x1) - source.x0);
    const float sourceHeight =
        static_cast<float>(static_cast<int32_t>(source.y1) - source.y0);
    const float layoutWidth =
        static_cast<float>(static_cast<int32_t>(layout.x1) - layout.x0);
    const float layoutHeight =
        static_cast<float>(static_cast<int32_t>(layout.y1) - layout.y0);
    if (sourceWidth < 8.0f || sourceHeight < 8.0f ||
        layoutWidth < 8.0f || layoutHeight < 8.0f)
    {
        return false;
    }

    RasterAffine result{};
    result.scaleX = layoutWidth / sourceWidth;
    result.scaleY = layoutHeight / sourceHeight;
    result.offsetX = static_cast<float>(layout.x0) -
        static_cast<float>(source.x0) * result.scaleX;
    result.offsetY = static_cast<float>(layout.y0) -
        static_cast<float>(source.y0) * result.scaleY;
    if (!std::isfinite(result.scaleX) || !std::isfinite(result.scaleY) ||
        !std::isfinite(result.offsetX) || !std::isfinite(result.offsetY) ||
        result.scaleX <= 0.0f || result.scaleY <= 0.0f)
    {
        return false;
    }
    output = result;
    return true;
}

inline float MapX(const RasterAffine& transform, float x) noexcept
{
    return transform.offsetX + x * transform.scaleX;
}

inline float MapY(const RasterAffine& transform, float y) noexcept
{
    return transform.offsetY + y * transform.scaleY;
}
}
