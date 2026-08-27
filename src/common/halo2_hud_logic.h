#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "halo2_render_logic.h"

// Halo 2 rasterizes a symmetric cover that is wider than either headset eye
// and later submits only the native-FOV sub-rectangle. Stock HUD anchors can
// therefore land in pixels the compositor deliberately crops away.
//
// Keep the engine's own HUD renderer, art, animation, and state. The first
// helper derives a desired layout rectangle inside the area both eyes can see;
// C-H2-77 consumes it through exact D3D shader draws. The second helper is
// retained as Stage 3D audit evidence but is dormant: the headset recorded zero
// retail callbacks at its alleged anchor consumer. The rectangle passed to
// chud_draw_screen is a clip/render bound, not the widget-layout basis; changing
// it was Stage 3C's disproven hypothesis.
namespace halo2_hud
{
inline constexpr float kMinimumLayoutFraction = 0.15f;
inline constexpr float kMaximumLayoutFraction = 1.0f;
inline constexpr float kMinimumHudSize = 0.30f;
inline constexpr float kMaximumHudSize = 1.00f;
inline constexpr float kMinimumHudAspect = 0.50f;
inline constexpr float kMaximumHudAspect = 2.00f;
inline constexpr float kMaximumVerticalOffset = 300.0f;

inline bool ComputeVisibleLayoutRectangle(
    const Halo2CameraRectangle& source,
    const Halo2SymmetricHalfFovs& cover,
    const float leftEyeFov[4], const float rightEyeFov[4],
    float hudSize, float hudAspect, float hudVerticalOffset,
    Halo2CameraRectangle& output) noexcept
{
    const int32_t sourceWidth =
        static_cast<int32_t>(source.x1) - source.x0;
    const int32_t sourceHeight =
        static_cast<int32_t>(source.y1) - source.y0;
    if (sourceWidth < 16 || sourceHeight < 16 ||
        !std::isfinite(cover.horizontal) ||
        !std::isfinite(cover.vertical) ||
        cover.horizontal <= 0.01f || cover.horizontal >= 1.55f ||
        cover.vertical <= 0.01f || cover.vertical >= 1.55f ||
        !leftEyeFov || !rightEyeFov ||
        !std::isfinite(hudSize) || !std::isfinite(hudAspect) ||
        !std::isfinite(hudVerticalOffset) ||
        hudSize < kMinimumHudSize || hudSize > kMaximumHudSize ||
        hudAspect < kMinimumHudAspect || hudAspect > kMaximumHudAspect ||
        std::fabs(hudVerticalOffset) > kMaximumVerticalOffset)
    {
        return false;
    }

    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(leftEyeFov[i]) ||
            !std::isfinite(rightEyeFov[i]))
            return false;
    const auto eyeFovValid = [](const float fov[4]) noexcept {
        return fov[0] > -1.55f && fov[0] < -0.01f &&
            fov[1] > 0.01f && fov[1] < 1.55f &&
            fov[2] > 0.01f && fov[2] < 1.55f &&
            fov[3] > -1.55f && fov[3] < -0.01f;
    };
    if (!eyeFovValid(leftEyeFov) || !eyeFovValid(rightEyeFov))
        return false;

    const float coverX = std::tan(cover.horizontal);
    const float coverY = std::tan(cover.vertical);
    const float left0 = std::tan(leftEyeFov[0]);
    const float right0 = std::tan(leftEyeFov[1]);
    const float up0 = std::tan(leftEyeFov[2]);
    const float down0 = std::tan(leftEyeFov[3]);
    const float left1 = std::tan(rightEyeFov[0]);
    const float right1 = std::tan(rightEyeFov[1]);
    const float up1 = std::tan(rightEyeFov[2]);
    const float down1 = std::tan(rightEyeFov[3]);
    const float visibleLeft = std::max(left0, left1);
    const float visibleRight = std::min(right0, right1);
    const float visibleUp = std::min(up0, up1);
    const float visibleDown = std::max(down0, down1);
    if (!std::isfinite(coverX) || !std::isfinite(coverY) ||
        coverX <= 0.01f || coverY <= 0.01f ||
        visibleLeft >= visibleRight || visibleDown >= visibleUp ||
        visibleLeft < -coverX || visibleRight > coverX ||
        visibleDown < -coverY || visibleUp > coverY)
    {
        return false;
    }

    const float sourceX0 = static_cast<float>(source.x0);
    const float sourceY0 = static_cast<float>(source.y0);
    const float width = static_cast<float>(sourceWidth);
    const float height = static_cast<float>(sourceHeight);
    float visibleX0 = sourceX0 +
        width * (visibleLeft + coverX) / (2.0f * coverX);
    float visibleX1 = sourceX0 +
        width * (visibleRight + coverX) / (2.0f * coverX);
    float visibleY0 = sourceY0 +
        height * (coverY - visibleUp) / (2.0f * coverY);
    float visibleY1 = sourceY0 +
        height * (coverY - visibleDown) / (2.0f * coverY);

    // Leave a small integer-pixel guard at the submitted frustum boundary.
    visibleX0 = std::ceil(visibleX0) + 2.0f;
    visibleY0 = std::ceil(visibleY0) + 2.0f;
    visibleX1 = std::floor(visibleX1) - 2.0f;
    visibleY1 = std::floor(visibleY1) - 2.0f;
    const float visibleWidth = visibleX1 - visibleX0;
    const float visibleHeight = visibleY1 - visibleY0;
    if (visibleWidth < 16.0f || visibleHeight < 16.0f)
        return false;

    const float horizontalFraction = std::clamp(
        hudSize * hudAspect,
        kMinimumLayoutFraction, kMaximumLayoutFraction);
    const float verticalFraction = std::clamp(
        hudSize, kMinimumLayoutFraction, kMaximumLayoutFraction);
    const float layoutWidth = visibleWidth * horizontalFraction;
    const float layoutHeight = visibleHeight * verticalFraction;
    const float centerX = (visibleX0 + visibleX1) * 0.5f;
    float centerY = (visibleY0 + visibleY1) * 0.5f - hudVerticalOffset;
    centerY = std::clamp(
        centerY,
        visibleY0 + layoutHeight * 0.5f,
        visibleY1 - layoutHeight * 0.5f);

    const long x0 = std::lround(centerX - layoutWidth * 0.5f);
    const long x1 = std::lround(centerX + layoutWidth * 0.5f);
    const long y0 = std::lround(centerY - layoutHeight * 0.5f);
    const long y1 = std::lround(centerY + layoutHeight * 0.5f);
    if (x0 < INT16_MIN || x1 > INT16_MAX ||
        y0 < INT16_MIN || y1 > INT16_MAX ||
        x1 - x0 < 8 || y1 - y0 < 8)
    {
        return false;
    }

    output.y0 = static_cast<int16_t>(y0);
    output.x0 = static_cast<int16_t>(x0);
    output.y1 = static_cast<int16_t>(y1);
    output.x1 = static_cast<int16_t>(x1);
    return true;
}

// Official H2EK new_hud_definition tags use anchor 4 for the crosshair. Keep
// that aiming reference on the engine's original basis: the layout sliders
// own information widgets, never the weapon/shot direction.
inline constexpr int32_t kCrosshairAnchor = 4;

inline bool MapNativeAnchorPoint(
    const Halo2CameraRectangle& source,
    const Halo2CameraRectangle& layout,
    int32_t anchor, float& x, float& y) noexcept
{
    if (anchor == kCrosshairAnchor ||
        !std::isfinite(x) || !std::isfinite(y))
    {
        return false;
    }

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

    const float mappedX = static_cast<float>(layout.x0) +
        (x - static_cast<float>(source.x0)) * layoutWidth / sourceWidth;
    const float mappedY = static_cast<float>(layout.y0) +
        (y - static_cast<float>(source.y0)) * layoutHeight / sourceHeight;
    if (!std::isfinite(mappedX) || !std::isfinite(mappedY))
        return false;
    x = mappedX;
    y = mappedY;
    return true;
}
}
