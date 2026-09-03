#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Halo 4's authored reticle is emitted by the CUI command dispatcher.  These
// are retail facts for the pinned halo4.dll image, not offsets copied from
// Halo 3, ODST, or Reach.  The entry and its only proven caller edge must both
// match uniquely at the recorded RVAs before the optional feature is installed.
inline constexpr uint32_t kHalo4CuiReticleDispatcherRva = 0x003F0EA4;
inline constexpr char kHalo4CuiReticleDispatcherEntryAob[] =
    "48 8B C4 55 56 57 41 56 41 57 48 8D A8 B8 FC FF FF 48 81 EC 50 04 00 00";
inline constexpr std::array<uint8_t, 24>
    kHalo4CuiReticleDispatcherEntryBytes{
        0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x56,
        0x41, 0x57, 0x48, 0x8D, 0xA8, 0xB8, 0xFC, 0xFF,
        0xFF, 0x48, 0x81, 0xEC, 0x50, 0x04, 0x00, 0x00};

inline constexpr uint32_t kHalo4CuiReticleCallerRva = 0x003F4B6B;
inline constexpr char kHalo4CuiReticleCallerAob[] =
    "49 8B 8F 10 04 00 00 4D 8D 8F 20 04 00 00 49 8B D6 E8 ?? ?? ?? ??";
inline constexpr std::array<uint8_t, 18> kHalo4CuiReticleCallerFixedBytes{
    0x49, 0x8B, 0x8F, 0x10, 0x04, 0x00, 0x00, 0x4D, 0x8D,
    0x8F, 0x20, 0x04, 0x00, 0x00, 0x49, 0x8B, 0xD6, 0xE8};
inline constexpr size_t kHalo4CuiReticleCallerCallOpcodeOffset = 17;
inline constexpr size_t kHalo4CuiReticleCallerCallDisplacementOffset = 18;
inline constexpr size_t kHalo4CuiReticleCallerCallNextOffset = 22;

// The dispatcher also services Halo 4's 216x96 auxiliary texture pass and
// later menu/overlay UI.  Only the full-size gameplay-HUD call below is inside
// the accepted per-eye render transaction.  A second optional hook brackets
// this exact call and supplies the TLS phase that admits reticle commands.
inline constexpr uint32_t kHalo4CuiGameplayRenderRva = 0x003ACD60;
inline constexpr char kHalo4CuiGameplayRenderEntryAob[] =
    "48 8B C4 55 53 56 57 41 56 41 57 48 8D 68 B1 48 81 EC A8 00 00 00 0F 29 78 B8 44 0F 29 40 A8";
inline constexpr std::array<uint8_t, 31>
    kHalo4CuiGameplayRenderEntryBytes{
        0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41,
        0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xB1, 0x48,
        0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x0F, 0x29,
        0x78, 0xB8, 0x44, 0x0F, 0x29, 0x40, 0xA8};

inline constexpr uint32_t kHalo4CuiGameplayCallerRva = 0x00375C51;
inline constexpr char kHalo4CuiGameplayCallerAob[] =
    "8B 8E 8C 03 00 00 4C 8D 45 A0 45 33 C9 44 88 6C 24 28 33 D2 89 7C 24 20 E8 ?? ?? ?? ?? 83 FB 03";
inline constexpr size_t kHalo4CuiGameplayCallerCallOpcodeOffset = 24;
inline constexpr size_t kHalo4CuiGameplayCallerCallDisplacementOffset = 25;
inline constexpr size_t kHalo4CuiGameplayCallerCallNextOffset = 29;
inline constexpr uint32_t kHalo4CuiGameplayCallerReturnRva = 0x00375C6E;

inline constexpr uint32_t kHalo4CuiCommandBegin = 0x28;
inline constexpr uint32_t kHalo4CuiCommandEnd = 0x29;
inline constexpr uint32_t kHalo4CuiCommandPolyart = 0x20;
inline constexpr uint32_t kHalo4CuiCommandPolyartThreeColor = 0x1F;
inline constexpr uint16_t kHalo4CuiCommandBeginPayloadSize = 0x0C;

inline constexpr uint32_t kHalo4CuiReticleAnchorCount = 4;

constexpr bool Halo4CuiReticleCallerTargetsDispatcher(
    uint32_t decodedTargetRva) noexcept
{
    return decodedTargetRva == kHalo4CuiReticleDispatcherRva;
}

constexpr bool Halo4CuiGameplayCallerTargetsRender(
    uint32_t decodedTargetRva) noexcept
{
    return decodedTargetRva == kHalo4CuiGameplayRenderRva;
}

// The CUI interception is optional. Its resources, both hooked entries, and
// both caller edges are proven before either hook is created; any missing fact
// leaves only this feature on the stock path.
struct Halo4CuiReticleInstallProof
{
    bool transformLayoutProven = false;
    uint32_t anchorsMatchedOnce = 0;
    uint32_t anchorsAtPinnedRva = 0;
    bool callerDecodesDispatcher = false;
    bool gameplayCallerDecodesRender = false;
    bool executableRange = false;
    bool mappingStable = false;
};

constexpr bool Halo4CuiReticleInstallComplete(
    const Halo4CuiReticleInstallProof& proof) noexcept
{
    return proof.transformLayoutProven &&
        proof.anchorsMatchedOnce == kHalo4CuiReticleAnchorCount &&
        proof.anchorsAtPinnedRva == kHalo4CuiReticleAnchorCount &&
        proof.callerDecodesDispatcher && proof.gameplayCallerDecodesRender &&
        proof.executableRange &&
        proof.mappingStable;
}

enum class Halo4CuiReticleOptionalInstallState : uint8_t
{
    StockFallback,
    CleanupRequired,
    Installed,
};

struct Halo4CuiReticleLifecycleAction
{
    bool nativeTransformLive = false;
    bool cleanupFeature = false;
    bool disarmCameraCore = false;
    bool endOpenXrSession = false;
};

// Hook installation alone is not enough to own native suppression. If the
// prepared reticle chain later fails, both CUI hooks remain installed only as
// pass-through trampolines and the title's stock reticle is left visible.
constexpr bool Halo4CuiReticleOwnsNativeSuppression(
    bool hooksLive, bool captureResourcesHealthy) noexcept
{
    return hooksLive && captureResourcesHealthy;
}

// Even a partially-created optional two-hook transaction owns only its own
// cleanup. Camera ownership and the OpenXR session are never lifecycle
// consequences of this feature's install result.
constexpr Halo4CuiReticleLifecycleAction Halo4CuiReticleLifecycleFor(
    Halo4CuiReticleOptionalInstallState state,
    const Halo4CuiReticleInstallProof& proof) noexcept
{
    return {
        state == Halo4CuiReticleOptionalInstallState::Installed &&
            Halo4CuiReticleInstallComplete(proof),
        state == Halo4CuiReticleOptionalInstallState::CleanupRequired,
        false,
        false,
    };
}

enum class Halo4CuiReticleAction : uint8_t
{
    DrawStock,
    HideNative,
};

// This decision is intentionally fail-open.  Unowned, uninstalled, malformed,
// or unrelated CUI work remains stock; unlike the old Reach transaction, an
// optional reticle miss never rejects the stereo frame.
constexpr Halo4CuiReticleAction Halo4DecideCuiReticleAction(
    bool ownsStereoTransaction, bool nativeTransformLive,
    bool commandReadable, uint32_t command, bool crosshairEnabled,
    bool killNativeReticle, int stereoEye, bool rightEyeFirst) noexcept
{
    if (!ownsStereoTransaction || !nativeTransformLive || !commandReadable ||
        command != kHalo4CuiCommandBegin || stereoEye < 0 || stereoEye > 1)
    {
        return Halo4CuiReticleAction::DrawStock;
    }

    if (!crosshairEnabled)
        return Halo4CuiReticleAction::HideNative;
    if (!killNativeReticle)
        return Halo4CuiReticleAction::DrawStock;

    // Match Halo 3/ODST/Reach: authored pixels are presented by the existing
    // OpenXR reticle quad, so the native flat copy is hidden.
    (void)rightEyeFirst;
    return Halo4CuiReticleAction::HideNative;
}

// Retail's type-0x28 handler pushes one 0x34-byte real_matrix4x3 entry. The
// uniform scale is float zero and reticle-only translation is the final float3
// at +0x28. Adjusting only those fields preserves Halo 4's bitmap animation,
// spread, hit marker, and target colour while leaving every draw command and
// every other HUD transform stock.
inline constexpr uint32_t kHalo4CuiTransformStackCountOffset = 0x870;
inline constexpr uint32_t kHalo4CuiTransformStackEntriesOffset = 0x878;
inline constexpr uint32_t kHalo4CuiTransformStride = 0x34;
inline constexpr uint32_t kHalo4CuiTransformTranslationOffset = 0x28;
inline constexpr uint32_t kHalo4CuiTransformStackMaximum = 0x60;

constexpr bool Halo4CuiTransformStackCountValid(uint32_t count) noexcept
{
    return count != 0 && count <= kHalo4CuiTransformStackMaximum;
}
// Stage 3BH measured these from Halo 4's own live CUI framing. Stage 3BR then
// enlarged the selected weapon reticle 2.5x around the 512x512 capture centre.
// These are Halo 4 measurements, not values copied from another title.
inline constexpr float kHalo4CuiCaptureWidth = 4134.312f;
inline constexpr float kHalo4CuiCaptureHeight = 1346.196f;
inline constexpr float kHalo4CuiCaptureTextureExtent = 512.0f;
inline constexpr float kHalo4CuiCaptureCenter = 256.0f;
inline constexpr float kHalo4CuiCaptureVerticalBias = 104.0f;
inline constexpr float kHalo4CuiCaptureVerticalBiasRatio =
    kHalo4CuiCaptureVerticalBias / kHalo4CuiCaptureHeight;
inline constexpr float kHalo4CuiCaptureScale = 2.5f;

// Stage 3CR/3CX source fold-in (2026-08-31). Halo 4 re-picks its HUD layout
// class per level load, so the calibrated selector displacement, vertical
// bias and presentation scale above are exact only on the calibrated canvas
// (baseY 336.549, hide 4134.312). The accepted 3CX image (7fdf539a...)
// computes all three live per capture from the values the visible-pass hide
// records, and falls back to the calibrated constants whenever a live value
// is invalid (the payload skips its refresh):
//   selector un-hide = live hide shift 4*|baseX|                     (3CR)
//   bias ratio       = clamp(P + Q*u + R*u^2, 0, f_cal), u = 1/baseY (3CX)
//   capture scale    = ZA + ZB/hide                                  (3CX)
// Anchors are on-disk capture-dump measurements pinned by the 3CX builder
// (tools/build_stage3cx_h4_three_point_bias.py):
//   f(336.549) = f_cal (bit-exact), f(731.878) = 0.003274,
//   f(582.373) = 0.006040;  z(4134.312) = 2.5, z(6543.256) = 2.5*185/286.
inline constexpr double kHalo4CuiBiasAnchorY1 = 336.549;
inline constexpr double kHalo4CuiBiasAnchorY2 = 731.878;
inline constexpr double kHalo4CuiBiasAnchorY3 = 582.373;
inline constexpr double kHalo4CuiBiasAnchorF1 =
    static_cast<double>(kHalo4CuiCaptureVerticalBiasRatio);
inline constexpr double kHalo4CuiBiasAnchorF2 = 0.003274;
inline constexpr double kHalo4CuiBiasAnchorF3 = 0.006040;

namespace halo4_cui_detail
{
// The exact quadratic in u = 1/y through the three anchors, the same
// algebra the 3CX builder runs before welding the constants into the image.
inline constexpr double kBiasU1 = 1.0 / kHalo4CuiBiasAnchorY1;
inline constexpr double kBiasU2 = 1.0 / kHalo4CuiBiasAnchorY2;
inline constexpr double kBiasU3 = 1.0 / kHalo4CuiBiasAnchorY3;
inline constexpr double kBiasR =
    ((kHalo4CuiBiasAnchorF3 - kHalo4CuiBiasAnchorF2) / (kBiasU3 - kBiasU2) -
     (kHalo4CuiBiasAnchorF1 - kHalo4CuiBiasAnchorF2) / (kBiasU1 - kBiasU2)) /
    (kBiasU3 - kBiasU1);
inline constexpr double kBiasQ =
    (kHalo4CuiBiasAnchorF1 - kHalo4CuiBiasAnchorF2) / (kBiasU1 - kBiasU2) -
    kBiasR * (kBiasU1 + kBiasU2);
inline constexpr double kBiasP = kHalo4CuiBiasAnchorF2 -
    kBiasQ * kBiasU2 - kBiasR * kBiasU2 * kBiasU2;
inline constexpr double kZoomCalWidth = 4134.312;
inline constexpr double kZoomCalScale = 2.5;
inline constexpr double kZoomNewWidth = 6543.256;
inline constexpr double kZoomNewScale = 2.5 * 185.0 / 286.0;
inline constexpr double kZoomA =
    (kZoomCalScale * kZoomCalWidth - kZoomNewScale * kZoomNewWidth) /
    (kZoomCalWidth - kZoomNewWidth);
inline constexpr double kZoomB = (kZoomCalScale - kZoomA) * kZoomCalWidth;
}

inline constexpr float kHalo4CuiBiasQuadP =
    static_cast<float>(halo4_cui_detail::kBiasP);
inline constexpr float kHalo4CuiBiasQuadQ =
    static_cast<float>(halo4_cui_detail::kBiasQ);
inline constexpr float kHalo4CuiBiasQuadR =
    static_cast<float>(halo4_cui_detail::kBiasR);
inline constexpr float kHalo4CuiZoomA =
    static_cast<float>(halo4_cui_detail::kZoomA);
inline constexpr float kHalo4CuiZoomB =
    static_cast<float>(halo4_cui_detail::kZoomB);

// The 3CX payload's bias refresh: valid only for a finite positive baseY
// (the stock centre is (-halfWidth, +halfHeight), so live baseY > 0);
// otherwise the calibrated ratio stands.
inline float Halo4CuiLiveVerticalBiasRatio(float liveBaseY) noexcept
{
    if (!std::isfinite(liveBaseY) || liveBaseY <= 0.0f)
        return kHalo4CuiCaptureVerticalBiasRatio;
    const float u = 1.0f / liveBaseY;
    // Horner order, matching the 3CX payload's instruction sequence
    // ((R*u)+Q)*u+P so the produced bias is bit-identical to the image's.
    const float f =
        (kHalo4CuiBiasQuadR * u + kHalo4CuiBiasQuadQ) * u + kHalo4CuiBiasQuadP;
    if (!std::isfinite(f))
        return kHalo4CuiCaptureVerticalBiasRatio;
    return std::min(std::max(f, 0.0f), kHalo4CuiCaptureVerticalBiasRatio);
}

// The 3CX payload's zoom refresh: z = ZA + ZB/hide, exact at both measured
// size anchors; the calibrated 2.5x stands for an invalid live hide.
inline float Halo4CuiLiveCaptureScale(float liveHideShift) noexcept
{
    if (!std::isfinite(liveHideShift) || liveHideShift <= 0.0f)
        return kHalo4CuiCaptureScale;
    const float scale = kHalo4CuiZoomA + kHalo4CuiZoomB / liveHideShift;
    if (!std::isfinite(scale) || scale <= 0.0f)
        return kHalo4CuiCaptureScale;
    return scale;
}

struct Halo4CuiCaptureSelectionState
{
    bool insideReticleContainer = false;
    bool polyartContainer = false;
};

// H4EK proves the grenade and damage indicators are PolyartWidget children of
// reticule_offset_container, while weapon reticles use bitmap widgets. Mark a
// polyart container before its draw so its vertices bake with the hidden
// transform, then keep it hidden through the matching 0x29.
inline bool Halo4CuiCaptureMarkPolyartBeforeDraw(
    Halo4CuiCaptureSelectionState& state, bool headerReadable,
    uint32_t command) noexcept
{
    if (!headerReadable || !state.insideReticleContainer ||
        (command != kHalo4CuiCommandPolyart &&
         command != kHalo4CuiCommandPolyartThreeColor))
    {
        return false;
    }
    state.polyartContainer = true;
    return true;
}

inline void Halo4CuiCaptureAdvanceAfterDraw(
    Halo4CuiCaptureSelectionState& state, bool headerReadable,
    uint32_t command, uint16_t payloadSize) noexcept
{
    if (!headerReadable)
        return;
    if (command == kHalo4CuiCommandBegin &&
        payloadSize == kHalo4CuiCommandBeginPayloadSize)
    {
        state.insideReticleContainer = true;
        state.polyartContainer = false;
    }
    else if (command == kHalo4CuiCommandEnd)
    {
        state = {};
    }
}

constexpr bool Halo4CuiCaptureKeepsTopTransform(
    const Halo4CuiCaptureSelectionState& state) noexcept
{
    return state.insideReticleContainer && !state.polyartContainer;
}

// 3CR: the un-hide displacement is the LIVE hide shift the visible pass
// baked (4*|baseX|), not the calibrated canvas width - a frozen 4134.312
// under-shot the live hide by 2409 units on other canvases and left the
// reticle outside the capture window (byte-empty captures). An invalid
// live value falls back to the calibrated width.
inline bool Halo4CuiCaptureAdjustedTranslationX(
    float currentX, bool keepOnTarget, float liveHideShift,
    float& adjustedX) noexcept
{
    adjustedX = currentX;
    if (!std::isfinite(currentX))
        return false;
    const float hideShift =
        std::isfinite(liveHideShift) && liveHideShift >= 4.0f
            ? liveHideShift
            : kHalo4CuiCaptureWidth;
    const float offscreenThreshold = hideShift * 0.5f;
    if (keepOnTarget)
    {
        if (currentX >= offscreenThreshold)
            adjustedX = currentX - hideShift;
    }
    else if (currentX < offscreenThreshold)
    {
        adjustedX = currentX + hideShift;
    }
    return std::isfinite(adjustedX);
}

inline bool Halo4CuiCaptureAdjustedTranslationX(
    float currentX, bool keepOnTarget, float& adjustedX) noexcept
{
    return Halo4CuiCaptureAdjustedTranslationX(
        currentX, keepOnTarget, kHalo4CuiCaptureWidth, adjustedX);
}

struct Halo4CuiCaptureViewport
{
    float topLeftX = 0.0f;
    float topLeftY = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
};

// 3CX: the vertical bias ratio and the presentation scale are supplied by
// the caller from the live layout (Halo4CuiLiveVerticalBiasRatio /
// Halo4CuiLiveCaptureScale); the 104-unit absolute fallback for a missing
// backbuffer description stays fixed exactly as the accepted 3BR thunk
// keeps its k104 slot.
inline bool Halo4BuildCuiCaptureDrawViewport(
    const Halo4CuiCaptureViewport& base, uint32_t backbufferWidth,
    uint32_t backbufferHeight, float verticalBiasRatio, float captureScale,
    Halo4CuiCaptureViewport& output) noexcept
{
    output = base;
    if (!std::isfinite(base.topLeftX) || !std::isfinite(base.topLeftY) ||
        !std::isfinite(base.width) || !std::isfinite(base.height) ||
        base.width <= 0.0f || base.height <= 0.0f ||
        !std::isfinite(verticalBiasRatio) || verticalBiasRatio < 0.0f ||
        !std::isfinite(captureScale) || captureScale <= 0.0f)
    {
        return false;
    }

    if (backbufferWidth != 0 && backbufferHeight != 0)
    {
        output.height = output.width *
            static_cast<float>(backbufferHeight) /
            static_cast<float>(backbufferWidth);
        output.topLeftY =
            (kHalo4CuiCaptureTextureExtent - output.height) * 0.5f -
            output.height * verticalBiasRatio;
    }
    else
    {
        output.topLeftY -= kHalo4CuiCaptureVerticalBias;
    }

    output.width *= captureScale;
    output.height *= captureScale;
    output.topLeftX = kHalo4CuiCaptureCenter - captureScale *
        (kHalo4CuiCaptureCenter - output.topLeftX);
    output.topLeftY = kHalo4CuiCaptureCenter - captureScale *
        (kHalo4CuiCaptureCenter - output.topLeftY);
    return std::isfinite(output.topLeftX) &&
        std::isfinite(output.topLeftY) && std::isfinite(output.width) &&
        std::isfinite(output.height) && output.width > 0.0f &&
        output.height > 0.0f;
}

inline bool Halo4BuildCuiCaptureDrawViewport(
    const Halo4CuiCaptureViewport& base, uint32_t backbufferWidth,
    uint32_t backbufferHeight, Halo4CuiCaptureViewport& output) noexcept
{
    return Halo4BuildCuiCaptureDrawViewport(
        base, backbufferWidth, backbufferHeight,
        kHalo4CuiCaptureVerticalBiasRatio, kHalo4CuiCaptureScale, output);
}
// Both canonical H4EK weapon CUI exports carry this nominal authored reticle
// height in their widescreen overlay. It is a Halo 4 content measurement, not
// a copied CHUD/capture calibration. Mapping that height through the live CUI
// half-height and eye FOV gives crosshair_size_deg its usual angular meaning.
inline constexpr float kHalo4CuiNominalReticleHeight = 81.92f;

struct Halo4CuiAimOffset
{
    float x = 0.0f;
    float y = 0.0f;
    bool valid = false;
};

// Halo 4 parity with Halo 3/ODST/Reach never positions authored art in CUI.
// The CUI transform is used only to remove the duplicate flat copy. Placement
// belongs exclusively to the shared OpenXR weapon-ray quad.
inline Halo4CuiAimOffset Halo4BuildHiddenCuiTranslation(
    float baseX, float baseY) noexcept
{
    Halo4CuiAimOffset result{};
    if (!std::isfinite(baseX) || !std::isfinite(baseY))
        return result;
    const float halfWidth = std::fabs(baseX);
    const float halfHeight = std::fabs(baseY);
    if (halfWidth < 1.0f || halfHeight < 1.0f ||
        halfWidth > 32768.0f || halfHeight > 32768.0f)
        return result;
    result.x = halfWidth * 4.0f;
    result.y = 0.0f;
    result.valid = std::isfinite(result.x);
    return result;
}

// C-H4-55 keeps the private authored-reticle replay on the stock CUI transform
// even while the visible HUD consumes Stage 3X size/aspect/height controls.
// Capture framing must therefore use the replay's own measured canvas. Before
// the first replay marker is observed, fall back to the visible-pass sample so
// startup remains identical to the accepted 3CX behavior.
inline bool Halo4SelectCuiCaptureCanvas(
    float replayBaseX, float replayBaseY,
    float visibleBaseX, float visibleBaseY,
    float& baseY, float& hideShift) noexcept
{
    const auto select = [&](float x, float y) noexcept
    {
        const Halo4CuiAimOffset hide =
            Halo4BuildHiddenCuiTranslation(x, y);
        if (!hide.valid || !std::isfinite(y) || y <= 0.0f)
            return false;
        baseY = y;
        hideShift = hide.x;
        return true;
    };
    return select(replayBaseX, replayBaseY) ||
        select(visibleBaseX, visibleBaseY);
}

// The projected aim is normalized device space. Halo 4's pushed CUI
// transform is not: the headset log measured the stock centre at
// (-halfWidth,+halfHeight). Convert through that live transform rather than
// copying a resolution, aspect ratio, or scale from another title.
inline Halo4CuiAimOffset Halo4MapAimToCuiTranslation(
    const Halo4CuiAimOffset& normalizedAim, float baseX, float baseY,
    bool hide) noexcept
{
    Halo4CuiAimOffset result{};
    if (!std::isfinite(baseX) || !std::isfinite(baseY))
        return result;
    const float halfWidth = std::fabs(baseX);
    const float halfHeight = std::fabs(baseY);
    if (halfWidth < 1.0f || halfHeight < 1.0f ||
        halfWidth > 32768.0f || halfHeight > 32768.0f)
        return result;

    if (hide)
        return Halo4BuildHiddenCuiTranslation(baseX, baseY);
    if (!normalizedAim.valid || !std::isfinite(normalizedAim.x) ||
        !std::isfinite(normalizedAim.y))
        return result;

    result.x = normalizedAim.x * halfWidth;
    result.y = normalizedAim.y * halfHeight;
    result.valid = std::isfinite(result.x) && std::isfinite(result.y);
    return result;
}

inline bool Halo4MapAngularSizeToCuiScale(
    float baseY, float halfFovY, float sizeDegrees,
    float& outScale) noexcept
{
    outScale = 0.0f;
    if (!std::isfinite(baseY) || !std::isfinite(halfFovY) ||
        !std::isfinite(sizeDegrees))
        return false;
    const float halfHeight = std::fabs(baseY);
    if (halfHeight < 1.0f || halfHeight > 32768.0f ||
        halfFovY <= 0.01f || halfFovY >= 1.56f ||
        sizeDegrees < 0.3f || sizeDegrees > 20.0f)
        return false;

    const float halfAngle = sizeDegrees * 0.5f * 0.01745329251994329577f;
    const float tanHalfFov = std::tan(halfFovY);
    const float nominalPixelHeight =
        2.0f * halfHeight * std::tan(halfAngle) / tanHalfFov;
    const float scale = nominalPixelHeight / kHalo4CuiNominalReticleHeight;
    if (!std::isfinite(scale) || scale < 0.001f || scale > 128.0f)
        return false;
    outScale = scale;
    return true;
}

inline Halo4CuiAimOffset Halo4ProjectAimToCuiOffset(
    const float cameraForward[3], const float cameraUp[3],
    const float aimForward[3], float halfFovX, float halfFovY) noexcept
{
    Halo4CuiAimOffset result{};
    if (!cameraForward || !cameraUp || !aimForward ||
        !std::isfinite(halfFovX) || !std::isfinite(halfFovY) ||
        halfFovX <= 0.01f || halfFovX >= 1.56f ||
        halfFovY <= 0.01f || halfFovY >= 1.56f)
        return result;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(cameraForward[i]) || !std::isfinite(cameraUp[i]) ||
            !std::isfinite(aimForward[i]))
            return result;

    const float right[3] = {
        cameraForward[1] * cameraUp[2] - cameraForward[2] * cameraUp[1],
        cameraForward[2] * cameraUp[0] - cameraForward[0] * cameraUp[2],
        cameraForward[0] * cameraUp[1] - cameraForward[1] * cameraUp[0]};
    const float forward = aimForward[0] * cameraForward[0] +
        aimForward[1] * cameraForward[1] +
        aimForward[2] * cameraForward[2];
    const float tanX = std::tan(halfFovX);
    const float tanY = std::tan(halfFovY);
    if (!std::isfinite(forward) || forward <= 0.01f ||
        !std::isfinite(tanX) || !std::isfinite(tanY) ||
        tanX <= 0.0f || tanY <= 0.0f)
        return result;

    result.x = (aimForward[0] * right[0] + aimForward[1] * right[1] +
                aimForward[2] * right[2]) / (forward * tanX);
    // The 43l headset result proved that the pushed reticle transform consumes
    // positive translation Y as camera-up. The earlier static assumption that
    // this composed matrix still used raster-down Y inverted gun movement.
    result.y = (aimForward[0] * cameraUp[0] + aimForward[1] * cameraUp[1] +
                aimForward[2] * cameraUp[2]) / (forward * tanY);
    result.valid = std::isfinite(result.x) && std::isfinite(result.y) &&
        std::fabs(result.x) <= 8.0f && std::fabs(result.y) <= 8.0f;
    return result;
}

// Build the ray from this rendered eye to the same finite point the shared VR
// reticle occupies. The engine aim vector is the centre-camera line through
// that point; reconstructing the point and subtracting each eye position keeps
// the native CUI art at the configured stereo depth instead of at infinity.
inline bool Halo4BuildReticleEyeRay(
    const float centerPosition[3], const float eyePosition[3],
    const float engineAimForward[3], float targetRange,
    float outEyeRay[3]) noexcept
{
    if (!centerPosition || !eyePosition || !engineAimForward || !outEyeRay ||
        !std::isfinite(targetRange) || targetRange <= 0.01f ||
        targetRange > 100000.0f)
        return false;
    float lengthSquared = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        if (!std::isfinite(centerPosition[i]) ||
            !std::isfinite(eyePosition[i]) ||
            !std::isfinite(engineAimForward[i]))
            return false;
        lengthSquared += engineAimForward[i] * engineAimForward[i];
    }
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8f)
        return false;
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    float eyeLengthSquared = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float target = centerPosition[i] +
            engineAimForward[i] * inverseLength * targetRange;
        outEyeRay[i] = target - eyePosition[i];
        eyeLengthSquared += outEyeRay[i] * outEyeRay[i];
    }
    return std::isfinite(eyeLengthSquared) && eyeLengthSquared > 1.0e-8f;
}

constexpr bool Halo4CuiReticlePairPositionsNative(
    uint64_t leftWriteSerial, uint64_t rightWriteSerial,
    uint64_t leftRenderedSerial, uint64_t rightRenderedSerial) noexcept
{
    return leftWriteSerial != 0 && leftWriteSerial == rightWriteSerial &&
        leftWriteSerial == leftRenderedSerial &&
        leftWriteSerial == rightRenderedSerial;
}

// Enabling the dispatcher hook is not proof that a non-blank authored image
// has reached the OpenXR swapchain. Halo 4 alone keeps the procedural gun-ray
// pixels during that bootstrap interval; the first validated authored upload
// replaces them without ever exposing a no-reticle frame.
constexpr bool Halo4CuiReticleNeedsProceduralBootstrap(
    bool authoredCaptureLive, bool authoredArtHeld, bool crosshairEnabled,
    bool killNativeReticle) noexcept
{
    return authoredCaptureLive && !authoredArtHeld && crosshairEnabled &&
        killNativeReticle;
}

// Stage 3BU deliberately retained this conservative raw bootstrap request.
// EnsureReticleChain's measured-art and held-art guards run before any repaint,
// so a validated authored upload still replaces the procedural pixels and is
// retained. Stage 3BJ forced this false before capture framing/selection was
// proven and was rejected; do not repeat that isolated change.
constexpr bool Halo4CuiReticleUsesProceduralFallback(
    bool authoredCaptureLive, bool crosshairEnabled,
    bool killNativeReticle) noexcept
{
    return authoredCaptureLive && crosshairEnabled && killNativeReticle;
}
