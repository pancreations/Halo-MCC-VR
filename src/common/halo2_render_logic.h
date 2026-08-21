#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Halo 2-only render evidence and cold-observation policy. This header is
// deliberately pure: no Windows APIs, logging, allocation, hooks, or engine
// writes. Every address and layout below is derived from the official H2EK and
// verified against both pinned retail halo2.dll images; see
// docs/HALO2-SIGNATURE-EVIDENCE.md.

inline constexpr size_t kHalo2RetailFileSize = 15807960;
inline constexpr size_t kHalo2RetailImageSize = 0x02A38000;
inline constexpr uint32_t kHalo2RetailPeTimestamp = 0x68A0F0F2;

inline constexpr const char* kHalo2RetailModuleSha256[] = {
    // Steam
    "DE65B4F4FDBF3F0A5EAB7431FE530DA17DD815599182DFD6AE9B7E21CF171946",
    // Microsoft Store / Xbox app (Game Pass)
    "81E5F41A7F8409D27A5454A28BFBECB8CD273E389366FB9865DD1D01E6BE689D",
};

inline constexpr char kHalo2KitBuildTag[] =
    "2023.06.20.176294.1-Release";
inline constexpr char kHalo2KitTagTestSha256[] =
    "D0B71186D3948C48DDD02E2CCB88FA13E77E25A3D8F7FA60922F23A2A0073E36";

// Halo 2 does not yet own a title-native pause signal. A pause/head-lock
// transition requested by another MCC engine must therefore be cleared while
// H2 owns the title context, and the synchronous render hooks must remain stock
// until both the requested and currently displayed presentation have returned
// to immersive stereo. Keeping this policy pure makes the no-claim boundary
// independently testable without touching OpenXR state.
inline bool Halo2MustClearForeignPause(
    bool stereoContext, bool pauseTarget, bool pausePresentation)
{
    return stereoContext && (pauseTarget || pausePresentation);
}

inline bool Halo2ShouldRequestForeignPauseClear(
    bool stereoContext, bool pauseTarget, bool pausePresentation,
    bool clearAlreadyRequested)
{
    return Halo2MustClearForeignPause(
               stereoContext, pauseTarget, pausePresentation) &&
        !clearAlreadyRequested;
}

inline constexpr float kHalo2MinimumAppCadenceHz = 72.0f;
inline constexpr float kHalo2MaximumAppCadenceHz = 144.0f;

inline bool Halo2RefreshCadenceSupported(float appCadenceHz)
{
    return std::isfinite(appCadenceHz) &&
        appCadenceHz >= kHalo2MinimumAppCadenceHz &&
        appCadenceHz <= kHalo2MaximumAppCadenceHz;
}

inline constexpr uint64_t kHalo2NanosecondsPerSecond = 1000000000ull;
// XrDuration is an integer count of nanoseconds. The prepared-frame cadence
// is ADVISORY: it only proves the runtime is alive and pacing (a period
// between 1000 Hz and 4 Hz). It used to demand 72-144 Hz on both the
// xrWaitFrame target and the predicted-display delta to keep a half-rate
// alternate-eye mode out; same-frame stereo makes that mode impossible by
// construction, and on 2026-08-21 the strict band blocked 22 times in two
// minutes (the game at 64-70 fps, SteamVR throttling to 30 Hz) and put the
// player on the flat screen each time - 864 of Anniversary's 920 stock
// passes. A slow frame now still gets its true per-eye pair; the
// compositor reprojects it.
inline constexpr uint64_t kHalo2FastestCadencePeriodNs = 1000000ull;
inline constexpr uint64_t kHalo2SlowestCadencePeriodNs = 250000000ull;

inline constexpr bool Halo2CadencePeriodSupported(uint64_t periodNs) noexcept
{
    return periodNs >= kHalo2FastestCadencePeriodNs &&
        periodNs <= kHalo2SlowestCadencePeriodNs;
}

inline constexpr bool Halo2PreparedCadenceSupported(
    uint64_t targetPeriodNs, uint64_t /*predictedDisplayDeltaNs*/) noexcept
{
    // The predicted-display delta between two prepared frames is NOT a
    // liveness signal: the runtime legitimately predicts the same display
    // time twice (a delta of nanoseconds, logged as 1e9 Hz) and skips
    // display periods. Only the xrWaitFrame target period says whether the
    // runtime is pacing at all.
    return Halo2CadencePeriodSupported(targetPeriodNs);
}

inline float Halo2CadenceHz(uint64_t periodNs) noexcept
{
    return periodNs
        ? static_cast<float>(
              static_cast<double>(kHalo2NanosecondsPerSecond) /
              static_cast<double>(periodNs))
        : 0.0f;
}

inline bool Halo2PresentationMayClaim(
    bool stereoContext, bool coreUsable, bool presentationIntended,
    bool pauseTarget, bool pausePresentation, uint64_t targetPeriodNs,
    uint64_t predictedDisplayDeltaNs)
{
    return stereoContext && coreUsable && presentationIntended &&
        !pauseTarget && !pausePresentation &&
        Halo2PreparedCadenceSupported(
            targetPeriodNs, predictedDisplayDeltaNs);
}

// Forward progress only. This used to demand exactly previousCompleted + 1,
// which was wrong: OpenXR prepared serials advance at the HEADSET's rate while
// the game renders at its own. A title running at 34 fps against a 90 Hz panel
// skips prepared serials as a matter of course, and the strict rule quarantined
// Halo 2 stereo 140 ms after its first accepted pair - observed 2026-08-21,
// "expected prepared serial 7031 after the first complete pair, saw 7038".
//
// A gap is not half-rate stereo. Half-rate is prevented by the two guarantees
// that actually describe it: both eyes must be freshly rendered inside ONE game
// frame at the CURRENT prepared serial, and the cadence gate must hold. What
// must still be refused is going backwards or repeating a serial, because that
// is what a stale or reused eye looks like.
inline constexpr bool Halo2PreparedSerialMayFollowCompletedPair(
    uint64_t lastCompletedSerial, uint64_t preparedSerial) noexcept
{
    if (!preparedSerial)
        return false;
    if (!lastCompletedSerial)
        return true;
    return lastCompletedSerial != UINT64_MAX &&
        preparedSerial > lastCompletedSerial;
}

// A completed engine eye is not the only point of no safe retry. If an owned
// camera span cannot be restored, or an H2 raster scope cannot be closed, the
// current frame may stay screen-visible but this module generation must not
// attempt another title transaction. A clean zero-eye failure remains eligible
// for the proven stock replay.
inline constexpr bool Halo2StructuralFailureRequiresQuarantine(
    uint32_t renderViewCalls, bool ownedStateRestoreFailed,
    bool rasterScopeCloseFailed, bool transactionExceptionSeen,
    bool transactionShapeFailed) noexcept
{
    return renderViewCalls > 0 || ownedStateRestoreFailed ||
        rasterScopeCloseFailed || transactionExceptionSeen ||
        transactionShapeFailed;
}

// The engine stores one heap-allocated 0x2C-byte game_time_globals object
// behind this module pointer slot. The official H2EK establishes the layout;
// retail's unique incrementer and level initializer independently decode the
// same slot.
inline constexpr uint32_t kHalo2GameTimeSlotRva = 0x015FE008;
inline constexpr uint32_t kHalo2GameTimeGlobalsSize = 0x2C;
inline constexpr uint32_t kHalo2GameTimeInitializedOffset = 0x00;
inline constexpr uint32_t kHalo2GameTimeTickRateOffset = 0x02;
inline constexpr uint32_t kHalo2GameTimeSecondsPerTickOffset = 0x04;
inline constexpr uint32_t kHalo2GameTimeCurrentTickOffset = 0x08;

// Exact H2EK-to-retail render transaction. Win64 passes `window` in RCX and
// the byte flag in low DL. The entry has one other retail caller, so entry-AOB
// uniqueness alone never authorizes a stereo transaction: the detour must also
// see the ordinary render-frame return address and the primary player index.
using Halo2RenderPlayerWindowFn = void (*)(void* window, uint8_t flag);
inline constexpr uint32_t kHalo2KitRenderPlayerWindowRva = 0x0029EAD0;
inline constexpr uint32_t kHalo2RetailRenderPlayerWindowRva = 0x007E2130;
inline constexpr uint32_t kHalo2RetailRenderPlayerWindowCallRva = 0x007E1706;
inline constexpr uint32_t kHalo2RetailRenderPlayerWindowReturnRva = 0x007E170B;
inline constexpr uint32_t kHalo2RetailRenderPlayerWindowOtherCallRva =
    0x007F0A60;
inline constexpr uint32_t kHalo2KitRenderViewRva = 0x002A0160;
inline constexpr uint32_t kHalo2RetailRenderViewRva = 0x007E30D0;
inline constexpr uint32_t kHalo2RetailRenderViewCallRva = 0x007E2412;
inline constexpr uint32_t kHalo2RetailRenderViewReturnRva = 0x007E2417;
// H2EK's final postprocess helper selects the swapchain backbuffer RTV. The
// retail homolog loads the same pointer slot here; C-H2-3 resolves it from a
// unique full-image signature and uses only the resulting exact RTV pointer in
// the OM hot hook.
inline constexpr uint32_t kHalo2RetailFinalOutputRtvLoadRva = 0x00975297;
inline constexpr uint32_t kHalo2RetailFinalOutputRtvSlotRva = 0x0197EE58;

// Retail camera/window facts. The two cameras occupy adjacent 0x74-byte
// records inside a 0x120-byte window. C-H2-3 owns only their three 12-byte pose
// vectors plus the 4-byte vertical-FOV cover, restoring every field it writes.
inline constexpr uint32_t kHalo2RetailWindowStride = 0x120;
inline constexpr uint32_t kHalo2WindowTypeOffset = 0x00;
inline constexpr uint32_t kHalo2WindowPlayerIndexOffset = 0x04;
inline constexpr uint32_t kHalo2WindowOutputUserOffset = 0x08;
inline constexpr uint32_t kHalo2RenderCameraOffset = 0x0C;
inline constexpr uint32_t kHalo2RasterCameraOffset = 0x80;
inline constexpr uint32_t kHalo2WindowTrailingViewArgumentOffset = 0xF8;
inline constexpr uint32_t kHalo2CameraBytes = 0x74;
inline constexpr uint32_t kHalo2CameraPositionOffset = 0x00;
inline constexpr uint32_t kHalo2CameraForwardOffset = 0x0C;
inline constexpr uint32_t kHalo2CameraUpOffset = 0x18;
inline constexpr uint32_t kHalo2CameraVectorBytes = 0x0C;
inline constexpr uint32_t kHalo2CameraVerticalFovOffset = 0x28;
inline constexpr uint32_t kHalo2CameraViewportRectangleOffset = 0x30;
inline constexpr uint32_t kHalo2CameraWindowRectangleOffset = 0x38;
inline constexpr uint32_t kHalo2CameraRectangleBytes = 0x08;
inline constexpr uint32_t kHalo2RectangleY0Offset = 0x00;
inline constexpr uint32_t kHalo2RectangleX0Offset = 0x02;
inline constexpr uint32_t kHalo2RectangleY1Offset = 0x04;
inline constexpr uint32_t kHalo2RectangleX1Offset = 0x06;
inline constexpr uint32_t kHalo2CameraZNearOffset = 0x40;
inline constexpr uint32_t kHalo2CameraZFarOffset = 0x44;
inline constexpr uint32_t kHalo2CameraAsymmetricEnableOffset = 0x58;
inline constexpr uint32_t kHalo2CameraFrustumCenterXOffset = 0x5C;
inline constexpr uint32_t kHalo2CameraFrustumCenterYOffset = 0x60;
inline constexpr uint32_t kHalo2CameraFrustumExtentScaleOffset = 0x64;
inline constexpr uint32_t kHalo2CameraPixelOffsetEnableOffset = 0x68;
inline constexpr uint32_t kHalo2CameraPixelOffsetXOffset = 0x6C;
inline constexpr uint32_t kHalo2CameraPixelOffsetYOffset = 0x70;

inline constexpr uint32_t kHalo2WindowRenderPositionOffset =
    kHalo2RenderCameraOffset + kHalo2CameraPositionOffset;
inline constexpr uint32_t kHalo2WindowRasterPositionOffset =
    kHalo2RasterCameraOffset + kHalo2CameraPositionOffset;
inline constexpr uint32_t kHalo2WindowRenderForwardOffset =
    kHalo2RenderCameraOffset + kHalo2CameraForwardOffset;
inline constexpr uint32_t kHalo2WindowRenderUpOffset =
    kHalo2RenderCameraOffset + kHalo2CameraUpOffset;
inline constexpr uint32_t kHalo2WindowRasterForwardOffset =
    kHalo2RasterCameraOffset + kHalo2CameraForwardOffset;
inline constexpr uint32_t kHalo2WindowRasterUpOffset =
    kHalo2RasterCameraOffset + kHalo2CameraUpOffset;
inline constexpr uint32_t kHalo2WindowRenderVerticalFovOffset =
    kHalo2RenderCameraOffset + kHalo2CameraVerticalFovOffset;
inline constexpr uint32_t kHalo2WindowRasterVerticalFovOffset =
    kHalo2RasterCameraOffset + kHalo2CameraVerticalFovOffset;

static_assert(kHalo2RenderCameraOffset + kHalo2CameraBytes ==
    kHalo2RasterCameraOffset);
static_assert(kHalo2RasterCameraOffset + kHalo2CameraBytes <=
    kHalo2RetailWindowStride);
static_assert(kHalo2CameraPixelOffsetYOffset + sizeof(float) ==
    kHalo2CameraBytes);

// Halo 2-specific metric evidence, not an inherited constant. H2EK's unique
// 3.048f is consumed by a source-backed metre/kilometre formatter; retail has
// the same unique constant and behavior, plus the unique reciprocal.
inline constexpr float kHalo2MetersPerWorldUnit = 3.048f;
inline constexpr float kHalo2WorldUnitsPerMeter =
    1.0f / kHalo2MetersPerWorldUnit;
// OpenXR eye poses are only centimetres from the midpoint. This deliberately
// generous bound turns a corrupt/torn runtime pose into a stock frame instead
// of allowing an unbounded engine-camera write.
inline constexpr float kHalo2MaxEyeOffsetMeters = 0.5f;
// Room-scale lean is CLAMPED per axis to this, never rejected: Halo 3,
// Reach and Halo 4 all clamp, and a rejected sample here costs the frame
// its stereo pair instead of a few centimetres of lean.
inline constexpr float kHalo2MaxHeadTranslationMeters = 4.0f;
// Sanity bound on the ABSOLUTE tracked head position (a torn or corrupt
// pose), deliberately far beyond any play space so a long walk from the
// tracking origin never disables the headset camera.
inline constexpr float kHalo2MaxHeadPositionMeters = 64.0f;
inline constexpr float kHalo2MaxHeadTranslationWorldUnits = 1.5f;
inline constexpr uint32_t kHalo2KitMetersPerWorldUnitRva = 0x007AD4F8;
inline constexpr uint32_t kHalo2RetailMetersPerWorldUnitFileOffset =
    0x00B13AF4;
inline constexpr uint32_t kHalo2RetailMetersPerWorldUnitRva = 0x00B14CF4;
inline constexpr uint32_t kHalo2RetailWorldUnitsPerMeterFileOffset =
    0x00B5B754;
inline constexpr uint32_t kHalo2RetailWorldUnitsPerMeterRva = 0x00B5C954;

inline constexpr int kHalo2LeftEye = 0;
inline constexpr int kHalo2RightEye = 1;
inline constexpr int kHalo2EyeCount = 2;

// Exact first 0x24 bytes of either retail camera. Projection fields remain
// separate so the scoped restore can never overwrite the engine-owned z_far.
struct Halo2CameraBasis
{
    float position[3]{};
    float forward[3]{};
    float up[3]{};
};
static_assert(sizeof(Halo2CameraBasis) == 0x24);
static_assert(offsetof(Halo2CameraBasis, position) ==
    kHalo2CameraPositionOffset);
static_assert(offsetof(Halo2CameraBasis, forward) ==
    kHalo2CameraForwardOffset);
static_assert(offsetof(Halo2CameraBasis, up) == kHalo2CameraUpOffset);

// H2 camera producers supply an orthonormal basis. A torn or corrupt read must
// never turn an otherwise bounded IPD into arbitrary engine memory writes.
inline bool Halo2ValidateCameraBasis(const Halo2CameraBasis& basis) noexcept
{
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(basis.position[axis]) ||
            !std::isfinite(basis.forward[axis]) ||
            !std::isfinite(basis.up[axis]))
        {
            return false;
        }
    }

    const float forwardLengthSquared =
        basis.forward[0] * basis.forward[0] +
        basis.forward[1] * basis.forward[1] +
        basis.forward[2] * basis.forward[2];
    const float upLengthSquared =
        basis.up[0] * basis.up[0] + basis.up[1] * basis.up[1] +
        basis.up[2] * basis.up[2];
    const float forwardUpDot =
        basis.forward[0] * basis.up[0] +
        basis.forward[1] * basis.up[1] +
        basis.forward[2] * basis.up[2];
    return std::isfinite(forwardLengthSquared) &&
        std::isfinite(upLengthSquared) && std::isfinite(forwardUpDot) &&
        std::fabs(forwardLengthSquared - 1.0f) < 0.05f &&
        std::fabs(upLengthSquared - 1.0f) < 0.05f &&
        std::fabs(forwardUpDot) < 0.05f;
}

struct Halo2TemporalEyePositions
{
    float render[3]{};
    float raster[3]{};
};

// eyePositionMeters is the selected OpenXR view origin relative to the stereo
// midpoint (+X right, +Y up, -Z forward). Both engine cameras are displaced in
// their own validated basis so visibility and raster consumers stay coherent:
//   delta = (cross(forward,up)*x + up*y - forward*z) / 3.048.
// Failure leaves `out` untouched.
inline bool Halo2BuildTemporalEyePositions(
    const Halo2CameraBasis& renderCamera,
    const Halo2CameraBasis& rasterCamera,
    const float eyePositionMeters[3], Halo2TemporalEyePositions& out) noexcept
{
    if (!eyePositionMeters || !Halo2ValidateCameraBasis(renderCamera) ||
        !Halo2ValidateCameraBasis(rasterCamera))
    {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(eyePositionMeters[axis]) ||
            std::fabs(eyePositionMeters[axis]) > kHalo2MaxEyeOffsetMeters)
            return false;

    Halo2TemporalEyePositions candidate{};
    const auto buildPosition = [&](const Halo2CameraBasis& camera,
                                   float position[3]) noexcept {
        const float right[3] = {
            camera.forward[1] * camera.up[2] -
                camera.forward[2] * camera.up[1],
            camera.forward[2] * camera.up[0] -
                camera.forward[0] * camera.up[2],
            camera.forward[0] * camera.up[1] -
                camera.forward[1] * camera.up[0]};
        for (int axis = 0; axis < 3; ++axis)
        {
            position[axis] = camera.position[axis] +
                (right[axis] * eyePositionMeters[0] +
                 camera.up[axis] * eyePositionMeters[1] -
                 camera.forward[axis] * eyePositionMeters[2]) *
                    kHalo2WorldUnitsPerMeter;
            if (!std::isfinite(position[axis]))
                return false;
        }
        return true;
    };
    if (!buildPosition(renderCamera, candidate.render) ||
        !buildPosition(rasterCamera, candidate.raster))
    {
        return false;
    }
    out = candidate;
    return true;
}

// Retail stores both camera rectangles as signed 16-bit y0/x0/y1/x1. The
// projection builder treats +0x28 as a full vertical FOV in radians.
struct Halo2CameraRectangle
{
    int16_t y0 = 0;
    int16_t x0 = 0;
    int16_t y1 = 0;
    int16_t x1 = 0;
};
static_assert(sizeof(Halo2CameraRectangle) == kHalo2CameraRectangleBytes);
static_assert(offsetof(Halo2CameraRectangle, y0) == kHalo2RectangleY0Offset);
static_assert(offsetof(Halo2CameraRectangle, x0) == kHalo2RectangleX0Offset);
static_assert(offsetof(Halo2CameraRectangle, y1) == kHalo2RectangleY1Offset);
static_assert(offsetof(Halo2CameraRectangle, x1) == kHalo2RectangleX1Offset);

struct Halo2SymmetricHalfFovs
{
    float horizontal = 0.0f;
    float vertical = 0.0f;
};

// This is only truthful while both native off-center controls are disabled.
// The generic rectangle parameter lets the caller use the camera's proven
// viewport rectangle without inventing a packed engine-camera C++ type.
inline bool Halo2DeriveSymmetricHalfFovs(
    float verticalFov, const Halo2CameraRectangle& rectangle,
    Halo2SymmetricHalfFovs& out) noexcept
{
    constexpr float kPi = 3.14159265f;
    if (!std::isfinite(verticalFov) || verticalFov <= 1.0e-4f ||
        verticalFov >= kPi - 1.0e-4f)
    {
        return false;
    }
    const int32_t width =
        static_cast<int32_t>(rectangle.x1) - rectangle.x0;
    const int32_t height =
        static_cast<int32_t>(rectangle.y1) - rectangle.y0;
    if (width <= 0 || height <= 0)
        return false;

    const float aspect =
        static_cast<float>(width) / static_cast<float>(height);
    const float halfVertical = verticalFov * 0.5f;
    const float verticalTangent = std::tan(halfVertical);
    const float halfHorizontal = std::atan(verticalTangent * aspect);
    if (!std::isfinite(aspect) || aspect <= 0.0f ||
        !std::isfinite(verticalTangent) || verticalTangent <= 0.0f ||
        !std::isfinite(halfHorizontal) || halfHorizontal <= 0.0f ||
        halfHorizontal >= kPi * 0.5f)
    {
        return false;
    }
    out = {halfHorizontal, halfVertical};
    return true;
}

constexpr bool Halo2StockProjectionIsSymmetric(
    uint8_t renderAsymmetricEnable, uint8_t renderPixelOffsetEnable,
    uint8_t rasterAsymmetricEnable, uint8_t rasterPixelOffsetEnable) noexcept
{
    return renderAsymmetricEnable == 0 && renderPixelOffsetEnable == 0 &&
        rasterAsymmetricEnable == 0 && rasterPixelOffsetEnable == 0;
}

// Serial parity, not callback count, owns the eye. Prepared serial zero is an
// invalid/unpublished frame. `rightEyeFirst` flips the odd/even assignment in
// the same way as the established title setting.
constexpr int Halo2TemporalEyeForSerial(
    uint64_t preparedSerial, bool rightEyeFirst) noexcept
{
    if (preparedSerial == 0)
        return -1;
    const int oddSerialEye = rightEyeFirst ? kHalo2RightEye : kHalo2LeftEye;
    return (preparedSerial & 1u) != 0 ? oddSerialEye : 1 - oddSerialEye;
}

struct Halo2TemporalEyeStamp
{
    uint32_t generation = 0;
    uint64_t preparedSerial = 0;
    int eye = -1;
    bool complete = false;
};

constexpr bool Halo2TemporalEyeStampValid(
    const Halo2TemporalEyeStamp& stamp, bool rightEyeFirst) noexcept
{
    return stamp.complete && stamp.generation != 0 &&
        stamp.preparedSerial != 0 && stamp.eye >= kHalo2LeftEye &&
        stamp.eye <= kHalo2RightEye &&
        stamp.eye == Halo2TemporalEyeForSerial(
            stamp.preparedSerial, rightEyeFirst);
}

enum class Halo2TemporalPairAction : uint8_t
{
    RejectCurrent = 0,
    SeedWithCurrent,
    PublishAdjacentPair,
};

// `previous` is the opposite-eye cache before `current` is committed. Current
// must match the caller's exact active generation and prepared serial. A first
// eye, generation boundary, or forward serial gap becomes a clean new seed;
// replay/out-of-order input is rejected. Only exactly N-1/N, opposite-parity
// completed eyes may publish.
constexpr Halo2TemporalPairAction SelectHalo2TemporalPairAction(
    const Halo2TemporalEyeStamp& previous,
    const Halo2TemporalEyeStamp& current, uint32_t activeGeneration,
    uint64_t expectedCurrentSerial, bool rightEyeFirst) noexcept
{
    if (!activeGeneration || !expectedCurrentSerial ||
        !Halo2TemporalEyeStampValid(current, rightEyeFirst) ||
        current.generation != activeGeneration ||
        current.preparedSerial != expectedCurrentSerial)
    {
        return Halo2TemporalPairAction::RejectCurrent;
    }
    if (!Halo2TemporalEyeStampValid(previous, rightEyeFirst) ||
        previous.generation != current.generation)
    {
        return Halo2TemporalPairAction::SeedWithCurrent;
    }
    if (previous.preparedSerial >= current.preparedSerial)
        return Halo2TemporalPairAction::RejectCurrent;
    if (current.preparedSerial - previous.preparedSerial != 1 ||
        previous.eye == current.eye)
    {
        return Halo2TemporalPairAction::SeedWithCurrent;
    }
    return Halo2TemporalPairAction::PublishAdjacentPair;
}

enum class Halo2CameraPositionWrite : uint8_t
{
    Reject = 0,
    RenderPosition,
    RasterPosition,
};

// A mechanical allow-list for the only two engine writes C-H2-2 owns.
constexpr Halo2CameraPositionWrite SelectHalo2CameraPositionWrite(
    uint32_t windowRelativeOffset, size_t bytes) noexcept
{
    if (bytes != kHalo2CameraVectorBytes)
        return Halo2CameraPositionWrite::Reject;
    if (windowRelativeOffset == kHalo2WindowRenderPositionOffset)
        return Halo2CameraPositionWrite::RenderPosition;
    if (windowRelativeOffset == kHalo2WindowRasterPositionOffset)
        return Halo2CameraPositionWrite::RasterPosition;
    return Halo2CameraPositionWrite::Reject;
}

enum class Halo2TemporalTransactionAction : uint8_t
{
    CallStockOnce = 0,
    RejectTemporalFrameAndCallStockOnce,
    ScopedPositionsAndCallOnce,
};

// Everything the hot detour must prove before touching either position. A
// foreign caller is simply stock; a malformed transaction on the exact player
// edge revokes that temporal frame but never disarms the title core.
struct Halo2TemporalTransactionInput
{
    bool stereoRequested = false;
    bool hookArmed = false;
    bool coldObservationPassed = false;
    bool exactCaller = false;
    bool flagValid = false;
    bool windowReadable = false;
    int32_t playerIndex = -1;
    bool levelLive = false;
    bool captureReady = false;
    bool teardownRequested = false;
    bool serialAlreadyClaimed = false;
    uint32_t activeGeneration = 0;
    uint32_t snapshotGeneration = 0;
    uint64_t preparedSerial = 0;
    int eye = -1;
    bool rightEyeFirst = false;
    bool renderCameraValid = false;
    bool rasterCameraValid = false;
    bool eyePositionValid = false;
    bool stockProjectionSymmetric = false;
    bool halfFovsValid = false;
};

constexpr Halo2TemporalTransactionAction SelectHalo2TemporalTransactionAction(
    const Halo2TemporalTransactionInput& input) noexcept
{
    if (!input.stereoRequested || !input.hookArmed)
        return Halo2TemporalTransactionAction::CallStockOnce;
    if (!input.exactCaller)
        return Halo2TemporalTransactionAction::CallStockOnce;
    if (!input.coldObservationPassed || !input.flagValid ||
        !input.windowReadable ||
        input.playerIndex != 0 || !input.levelLive || !input.captureReady ||
        input.teardownRequested || input.serialAlreadyClaimed ||
        input.activeGeneration == 0 ||
        input.snapshotGeneration != input.activeGeneration ||
        input.preparedSerial == 0 || input.eye < kHalo2LeftEye ||
        input.eye > kHalo2RightEye ||
        input.eye != Halo2TemporalEyeForSerial(
            input.preparedSerial, input.rightEyeFirst) ||
        !input.renderCameraValid || !input.rasterCameraValid ||
        !input.eyePositionValid || !input.stockProjectionSymmetric ||
        !input.halfFovsValid)
    {
        return Halo2TemporalTransactionAction::
            RejectTemporalFrameAndCallStockOnce;
    }
    return Halo2TemporalTransactionAction::ScopedPositionsAndCallOnce;
}

// Publication is permitted only after one (never zero or two) original call,
// successful return, both selective writes, and byte-restoration of both saved
// positions. Whole-camera restore or any other camera write fails this proof.
struct Halo2TemporalTransactionResult
{
    uint32_t originalCalls = 0;
    bool originalReturned = false;
    bool renderPositionWritten = false;
    bool rasterPositionWritten = false;
    bool renderPositionRestored = false;
    bool rasterPositionRestored = false;
    bool otherCameraBytesWritten = false;
};

constexpr bool Halo2TemporalTransactionResultMatches(
    Halo2TemporalTransactionAction action,
    const Halo2TemporalTransactionResult& result) noexcept
{
    if (result.originalCalls != 1 || !result.originalReturned ||
        result.otherCameraBytesWritten)
    {
        return false;
    }
    if (action == Halo2TemporalTransactionAction::ScopedPositionsAndCallOnce)
    {
        return result.renderPositionWritten && result.rasterPositionWritten &&
            result.renderPositionRestored && result.rasterPositionRestored;
    }
    if (action == Halo2TemporalTransactionAction::CallStockOnce ||
        action == Halo2TemporalTransactionAction::
            RejectTemporalFrameAndCallStockOnce)
    {
        return !result.renderPositionWritten &&
            !result.rasterPositionWritten &&
            !result.renderPositionRestored &&
            !result.rasterPositionRestored;
    }
    return false;
}

// C-H2-3 same-frame stereo + 6DOF. These are title-local camera operations:
// H2EK proves position/forward/up and retail preserves those exact offsets.
// OpenXR quaternions are expressed in +X right, +Y up, -Z forward axes.
inline bool Halo2NormalizeQuaternion(
    const float input[4], float output[4]) noexcept
{
    if (!input || !output)
        return false;
    float lengthSquared = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        if (!std::isfinite(input[i]))
            return false;
        lengthSquared += input[i] * input[i];
    }
    if (!std::isfinite(lengthSquared) || lengthSquared < 0.5f ||
        lengthSquared > 1.5f)
    {
        return false;
    }
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    for (int i = 0; i < 4; ++i)
        output[i] = input[i] * inverseLength;
    return true;
}

inline void Halo2MultiplyQuaternion(
    const float left[4], const float right[4], float output[4]) noexcept
{
    output[0] = left[3] * right[0] + left[0] * right[3] +
        left[1] * right[2] - left[2] * right[1];
    output[1] = left[3] * right[1] - left[0] * right[2] +
        left[1] * right[3] + left[2] * right[0];
    output[2] = left[3] * right[2] + left[0] * right[1] -
        left[1] * right[0] + left[2] * right[3];
    output[3] = left[3] * right[3] - left[0] * right[0] -
        left[1] * right[1] - left[2] * right[2];
}

inline void Halo2RotateVectorByQuaternion(
    const float quaternion[4], const float input[3], float output[3]) noexcept
{
    const float qx = quaternion[0], qy = quaternion[1];
    const float qz = quaternion[2], qw = quaternion[3];
    const float tx = 2.0f * (qy * input[2] - qz * input[1]);
    const float ty = 2.0f * (qz * input[0] - qx * input[2]);
    const float tz = 2.0f * (qx * input[1] - qy * input[0]);
    output[0] = input[0] + qw * tx + (qy * tz - qz * ty);
    output[1] = input[1] + qw * ty + (qz * tx - qx * tz);
    output[2] = input[2] + qw * tz + (qx * ty - qy * tx);
}

inline void Halo2RotateAboutAxis(
    float vector[3], const float axis[3], float cosine, float sine) noexcept
{
    const float dot = vector[0] * axis[0] + vector[1] * axis[1] +
        vector[2] * axis[2];
    const float cross[3] = {
        axis[1] * vector[2] - axis[2] * vector[1],
        axis[2] * vector[0] - axis[0] * vector[2],
        axis[0] * vector[1] - axis[1] * vector[0]};
    for (int i = 0; i < 3; ++i)
        vector[i] = vector[i] * cosine + cross[i] * sine +
            axis[i] * dot * (1.0f - cosine);
}

inline bool Halo2ApplyLocalQuaternion(
    Halo2CameraBasis& camera, const float localQuaternion[4]) noexcept
{
    if (!Halo2ValidateCameraBasis(camera))
        return false;
    float quaternion[4]{};
    if (!Halo2NormalizeQuaternion(localQuaternion, quaternion))
        return false;
    const float sineHalf = std::sqrt(
        quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
        quaternion[2] * quaternion[2]);
    if (sineHalf < 1.0e-6f)
        return true;
    float angle = 2.0f * std::atan2(sineHalf, quaternion[3]);
    constexpr float kPi = 3.14159265358979323846f;
    if (angle > kPi)
        angle -= 2.0f * kPi;
    const float localAxis[3] = {
        quaternion[0] / sineHalf,
        quaternion[1] / sineHalf,
        quaternion[2] / sineHalf};
    const float right[3] = {
        camera.forward[1] * camera.up[2] -
            camera.forward[2] * camera.up[1],
        camera.forward[2] * camera.up[0] -
            camera.forward[0] * camera.up[2],
        camera.forward[0] * camera.up[1] -
            camera.forward[1] * camera.up[0]};
    const float worldAxis[3] = {
        right[0] * localAxis[0] + camera.up[0] * localAxis[1] -
            camera.forward[0] * localAxis[2],
        right[1] * localAxis[0] + camera.up[1] * localAxis[1] -
            camera.forward[1] * localAxis[2],
        right[2] * localAxis[0] + camera.up[2] * localAxis[1] -
            camera.forward[2] * localAxis[2]};
    const float cosine = std::cos(angle), sine = std::sin(angle);
    Halo2RotateAboutAxis(camera.forward, worldAxis, cosine, sine);
    Halo2RotateAboutAxis(camera.up, worldAxis, cosine, sine);
    return Halo2ValidateCameraBasis(camera);
}

struct Halo2TrackedHeadInput
{
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float position[3]{};
    float referenceOrientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float referencePosition[3]{};
    // The universal halomccvr.cfg / F-key knobs every accepted title reads:
    // the F6 positional (leaning) toggle and world_scale, which IS the
    // metres-to-world-units factor (Halo 3 multiplies lean and eye offset
    // by it directly; its default 0.33 is 1/3.048 to two places).
    bool positional = true;
    float worldScale = kHalo2WorldUnitsPerMeter;
};

// The yaw-only part of an OpenXR head orientation: the rotation about room
// +Y that carries (0,0,-1) onto the head's horizontal forward. Halo 3, Reach
// and Halo 4 all recenter against yaw only, so a recenter taken with the
// head pitched or rolled does not tilt the world. Looking straight up or
// down has no horizontal forward; identity yaw is returned for that pole.
inline void Halo2YawOnlyQuaternion(
    const float orientation[4], float output[4]) noexcept
{
    const float forwardAxis[3] = {0.0f, 0.0f, -1.0f};
    float forward[3]{};
    Halo2RotateVectorByQuaternion(orientation, forwardAxis, forward);
    const float horizontal =
        std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (!std::isfinite(horizontal) || horizontal < 1.0e-4f)
    {
        output[0] = output[1] = output[2] = 0.0f;
        output[3] = 1.0f;
        return;
    }
    // Rotation about +Y by theta maps (0,0,-1) to (-sin theta, 0, -cos theta).
    const float theta = std::atan2(-forward[0], -forward[2]);
    output[0] = 0.0f;
    output[1] = std::sin(theta * 0.5f);
    output[2] = 0.0f;
    output[3] = std::cos(theta * 0.5f);
}

// Builds a tracked center camera from the stock H2 camera, the way the
// three accepted titles do it:
//  - orientation is the head's orientation relative to a YAW-ONLY recenter
//    reference, composed onto the game camera in its local axes, so pitch
//    and roll are the headset's own and yaw adds to the game's;
//  - lean is the room-space displacement from the recenter point, taken in
//    the recentered horizontal frame and re-applied in the GAME's horizontal
//    frame (room up -> world +Z) at world_scale world units per metre
//    (0.33 = 1/3.048 by default), clamped per axis and never rejected.
// A camera looking straight along world Z has no horizontal frame; the
// displacement then follows the camera basis itself.
inline bool Halo2BuildTrackedCenterCamera(
    const Halo2CameraBasis& stock, const Halo2TrackedHeadInput& input,
    Halo2CameraBasis& output) noexcept
{
    if (!Halo2ValidateCameraBasis(stock))
        return false;
    float current[4]{}, reference[4]{};
    if (!Halo2NormalizeQuaternion(input.orientation, current) ||
        !Halo2NormalizeQuaternion(input.referenceOrientation, reference) ||
        !std::isfinite(input.worldScale) || input.worldScale <= 0.0f)
    {
        return false;
    }
    float yawReference[4]{};
    Halo2YawOnlyQuaternion(reference, yawReference);
    const float inverseYawReference[4] = {
        -yawReference[0], -yawReference[1], -yawReference[2],
        yawReference[3]};

    Halo2CameraBasis candidate = stock;
    if (input.positional)
    {
        float rawDelta[3]{};
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!std::isfinite(input.position[axis]) ||
                !std::isfinite(input.referencePosition[axis]))
            {
                return false;
            }
            rawDelta[axis] =
                input.position[axis] - input.referencePosition[axis];
            if (!std::isfinite(rawDelta[axis]))
                return false;
            if (rawDelta[axis] > kHalo2MaxHeadTranslationMeters)
                rawDelta[axis] = kHalo2MaxHeadTranslationMeters;
            if (rawDelta[axis] < -kHalo2MaxHeadTranslationMeters)
                rawDelta[axis] = -kHalo2MaxHeadTranslationMeters;
        }
        // Room displacement in the recentered frame: +x right, +y up,
        // -z forward.
        float local[3]{};
        Halo2RotateVectorByQuaternion(inverseYawReference, rawDelta, local);
        const float rightMeters = local[0];
        const float upMeters = local[1];
        const float forwardMeters = -local[2];

        float forwardAxis[3]{};
        float rightAxis[3]{};
        float upAxis[3]{};
        const float horizontal = std::sqrt(
            stock.forward[0] * stock.forward[0] +
            stock.forward[1] * stock.forward[1]);
        if (std::isfinite(horizontal) && horizontal >= 1.0e-4f)
        {
            // Halo world: +Z up. Horizontal forward (cos yaw, sin yaw, 0),
            // right = forward x up = (sin yaw, -cos yaw, 0).
            forwardAxis[0] = stock.forward[0] / horizontal;
            forwardAxis[1] = stock.forward[1] / horizontal;
            forwardAxis[2] = 0.0f;
            rightAxis[0] = forwardAxis[1];
            rightAxis[1] = -forwardAxis[0];
            rightAxis[2] = 0.0f;
            upAxis[0] = 0.0f;
            upAxis[1] = 0.0f;
            upAxis[2] = 1.0f;
        }
        else
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                forwardAxis[axis] = stock.forward[axis];
                upAxis[axis] = stock.up[axis];
            }
            rightAxis[0] = stock.forward[1] * stock.up[2] -
                stock.forward[2] * stock.up[1];
            rightAxis[1] = stock.forward[2] * stock.up[0] -
                stock.forward[0] * stock.up[2];
            rightAxis[2] = stock.forward[0] * stock.up[1] -
                stock.forward[1] * stock.up[0];
        }
        const float scale = input.worldScale;
        for (int axis = 0; axis < 3; ++axis)
        {
            float offset =
                (forwardAxis[axis] * forwardMeters +
                 rightAxis[axis] * rightMeters +
                 upAxis[axis] * upMeters) * scale;
            if (!std::isfinite(offset))
                return false;
            if (offset > kHalo2MaxHeadTranslationWorldUnits)
                offset = kHalo2MaxHeadTranslationWorldUnits;
            if (offset < -kHalo2MaxHeadTranslationWorldUnits)
                offset = -kHalo2MaxHeadTranslationWorldUnits;
            candidate.position[axis] += offset;
        }
    }

    float relativeOrientation[4]{};
    Halo2MultiplyQuaternion(
        inverseYawReference, current, relativeOrientation);
    if (!Halo2ApplyLocalQuaternion(candidate, relativeOrientation))
        return false;
    output = candidate;
    return true;
}

// E-H2-6 pose ownership. The observer core publishes, once per game frame,
// the engine's camera as it found it (`stock`) and the camera it wrote
// (`tracked`), with the recenter reference it used. A per-eye core then
// decides, from the camera it actually holds, whether that camera already
// carries the head pose. This is what stops a frame the observer skipped
// (snapshot not ready yet - 576 of ~5300 frames on 2026-08-21) from being
// rendered as if it were tracked, and what keeps a director/playback camera
// (0x960780's override branch, not observer-derived) from being mistaken
// for one.
struct Halo2ObserverPosePublication
{
    uint32_t generation = 0;
    uint64_t serial = 0;
    Halo2CameraBasis stock{};
    Halo2CameraBasis tracked{};
    float referenceOrientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float referencePosition[3]{};
};

inline bool Halo2CameraBasisMatches(
    const Halo2CameraBasis& left, const Halo2CameraBasis& right) noexcept
{
    constexpr float kPositionEpsilonWorldUnits = 1.0e-3f;
    constexpr float kMinimumAxisDot = 0.9999f;
    float forwardDot = 0.0f;
    float upDot = 0.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(left.position[axis]) ||
            !std::isfinite(right.position[axis]) ||
            std::fabs(left.position[axis] - right.position[axis]) >
                kPositionEpsilonWorldUnits)
        {
            return false;
        }
        forwardDot += left.forward[axis] * right.forward[axis];
        upDot += left.up[axis] * right.up[axis];
    }
    return std::isfinite(forwardDot) && std::isfinite(upDot) &&
        forwardDot > kMinimumAxisDot && upDot > kMinimumAxisDot;
}

enum class Halo2PoseOwnerDecision : uint8_t
{
    // No publication, or the camera in hand is not the observer's tracked
    // camera (observer skipped this frame, or a non-observer camera): this
    // core tracks the centre itself from its own snapshot.
    SelfTrack = 0,
    // The camera in hand is the observer's tracked camera for this exact
    // prepared serial: use it as the centre and add only the eye offset.
    UsePublishedTracked,
    // The camera in hand is the observer's tracked camera, but for a
    // different prepared serial: rebuild the centre from the published stock
    // with this core's own (current) snapshot and the observer's reference.
    RederiveFromPublishedStock,
    // The publication claims to describe this camera but belongs to another
    // module generation: nothing can be trusted, render stock this frame.
    NoPose,
};

inline Halo2PoseOwnerDecision Halo2SelectPoseOwner(
    bool published, uint32_t publishedGeneration, uint64_t publishedSerial,
    const Halo2CameraBasis& publishedTracked,
    const Halo2CameraBasis& engineCamera,
    uint32_t generation, uint64_t serial) noexcept
{
    if (!published || !publishedSerial)
        return Halo2PoseOwnerDecision::SelfTrack;
    if (!Halo2CameraBasisMatches(engineCamera, publishedTracked))
        return Halo2PoseOwnerDecision::SelfTrack;
    if (publishedGeneration != generation || !generation)
        return Halo2PoseOwnerDecision::NoPose;
    return publishedSerial == serial
        ? Halo2PoseOwnerDecision::UsePublishedTracked
        : Halo2PoseOwnerDecision::RederiveFromPublishedStock;
}

// `worldUnitsPerMeter` is the universal world_scale, exactly as Halo 3 scales
// its eye offset (game.cpp eyeScale = g_worldScale).
inline bool Halo2BuildSynchronousEyeCamera(
    const Halo2CameraBasis& trackedCenter, const float eyePositionMeters[3],
    const float eyeOrientation[4], Halo2CameraBasis& output,
    float worldUnitsPerMeter = kHalo2WorldUnitsPerMeter) noexcept
{
    if (!eyePositionMeters || !eyeOrientation ||
        !Halo2ValidateCameraBasis(trackedCenter) ||
        !std::isfinite(worldUnitsPerMeter) || worldUnitsPerMeter <= 0.0f)
    {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(eyePositionMeters[axis]) ||
            std::fabs(eyePositionMeters[axis]) > kHalo2MaxEyeOffsetMeters)
            return false;
    Halo2CameraBasis candidate = trackedCenter;
    const float right[3] = {
        candidate.forward[1] * candidate.up[2] -
            candidate.forward[2] * candidate.up[1],
        candidate.forward[2] * candidate.up[0] -
            candidate.forward[0] * candidate.up[2],
        candidate.forward[0] * candidate.up[1] -
            candidate.forward[1] * candidate.up[0]};
    for (int axis = 0; axis < 3; ++axis)
    {
        candidate.position[axis] +=
            (right[axis] * eyePositionMeters[0] +
             candidate.up[axis] * eyePositionMeters[1] -
             candidate.forward[axis] * eyePositionMeters[2]) *
            worldUnitsPerMeter;
    }
    if (!Halo2ApplyLocalQuaternion(candidate, eyeOrientation))
        return false;
    output = candidate;
    return true;
}

enum class Halo2CameraPoseWrite : uint8_t
{
    Reject = 0,
    RenderPosition,
    RenderForward,
    RenderUp,
    RasterPosition,
    RasterForward,
    RasterUp,
    RenderVerticalFov,
    RasterVerticalFov,
};

constexpr Halo2CameraPoseWrite SelectHalo2CameraPoseWrite(
    uint32_t windowRelativeOffset, size_t bytes) noexcept
{
    if (bytes == kHalo2CameraVectorBytes)
    {
        if (windowRelativeOffset == kHalo2WindowRenderPositionOffset)
            return Halo2CameraPoseWrite::RenderPosition;
        if (windowRelativeOffset == kHalo2WindowRenderForwardOffset)
            return Halo2CameraPoseWrite::RenderForward;
        if (windowRelativeOffset == kHalo2WindowRenderUpOffset)
            return Halo2CameraPoseWrite::RenderUp;
        if (windowRelativeOffset == kHalo2WindowRasterPositionOffset)
            return Halo2CameraPoseWrite::RasterPosition;
        if (windowRelativeOffset == kHalo2WindowRasterForwardOffset)
            return Halo2CameraPoseWrite::RasterForward;
        if (windowRelativeOffset == kHalo2WindowRasterUpOffset)
            return Halo2CameraPoseWrite::RasterUp;
    }
    else if (bytes == sizeof(float))
    {
        if (windowRelativeOffset == kHalo2WindowRenderVerticalFovOffset)
            return Halo2CameraPoseWrite::RenderVerticalFov;
        if (windowRelativeOffset == kHalo2WindowRasterVerticalFovOffset)
            return Halo2CameraPoseWrite::RasterVerticalFov;
    }
    return Halo2CameraPoseWrite::Reject;
}

struct Halo2SameFramePairProof
{
    uint32_t generation = 0;
    uint64_t preparedSerial = 0;
    uint64_t attemptToken = 0;
    uint64_t eyeAttemptToken[2]{};
    uint64_t renderSerial[2]{};
    uint64_t captureSerial[2]{};
    uint8_t eyeMask = 0;
    uint8_t eyeRenderCount = 0;
    uint8_t freshEyeCount = 0;
    bool allPoseSpansRestored = false;
};

constexpr bool Halo2SameFramePairMatches(
    const Halo2SameFramePairProof& proof, uint32_t activeGeneration,
    uint64_t currentPreparedSerial) noexcept
{
    return activeGeneration != 0 && currentPreparedSerial != 0 &&
        proof.generation == activeGeneration &&
        proof.preparedSerial == currentPreparedSerial &&
        proof.attemptToken != 0 &&
        proof.eyeAttemptToken[0] == proof.attemptToken &&
        proof.eyeAttemptToken[1] == proof.attemptToken &&
        proof.renderSerial[0] == currentPreparedSerial &&
        proof.renderSerial[1] == currentPreparedSerial &&
        proof.captureSerial[0] == currentPreparedSerial &&
        proof.captureSerial[1] == currentPreparedSerial &&
        proof.eyeMask == 0x3 && proof.eyeRenderCount == 2 &&
        proof.freshEyeCount == 2 && proof.allPoseSpansRestored;
}

struct Halo2RetailAnchor
{
    const char* name;
    const char* pattern;
    uint32_t rva;
    uint8_t relativeDispOffset;
    uint32_t relativeTargetRva;
};

inline constexpr size_t kHalo2AnchorGameTimeIncrement = 0;
inline constexpr size_t kHalo2AnchorGameTimeInit = 1;
inline constexpr size_t kHalo2AnchorRenderFrame = 2;
inline constexpr size_t kHalo2AnchorPlayerWindow = 3;
inline constexpr size_t kHalo2AnchorRenderView = 4;
inline constexpr size_t kHalo2AnchorAsymmetricFrustum = 5;

// Every pattern matched exactly once over each complete mapped retail image.
// A nonzero relativeDispOffset names a disp32 inside the match; its instruction
// ends at offset+4, so the same decode covers RIP-relative data operands.
inline constexpr Halo2RetailAnchor kHalo2RetailAnchors[] = {
    { "game-time-increment",
      "48 8B 05 ?? ?? ?? ?? FF 40 08 C3",
      0x7067F0, 0x03, kHalo2GameTimeSlotRva },
    { "game-time-level-init",
      "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 33 C9 48 89 08 48 89 48 08 48 "
      "89 48 10 48 89 48 18 48 89 48 20 89 48 28 E8 ?? ?? ?? ?? 48 8B "
      "15 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 0F BF 48 08 66 89 4A 02 "
      "C7 42 0C 00 00 80 3F C6 02 01",
      0x706910, 0x07, kHalo2GameTimeSlotRva },
    { "render-frame",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
      "41 56 48 83 EC 40 41 8B F9 48 63 EA 41 8B F0 8B D9",
      0x7E1600, 0, 0 },
    { "render-player-window",
      "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 "
      "02 00 00",
      0x7E2130, 0, 0 },
    { "render-view",
      "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 44 88 4C 24 20 "
      "55 41 54 41 55 41 56 41 57 48 8D AC 24 B0 FD FF FF 48 81 EC 50 "
      "03 00 00",
      0x7E30D0, 0, 0 },
    { "asymmetric-frustum-helper",
      "48 89 5C 24 08 57 48 83 EC 40 0F BF 41 3A 48 8B F9 44 0F BF 49 "
      "3C 48 8B DA 0F BF 51 32 44 0F BF 41 3E",
      0x7DFCD0, 0, 0 },
};

inline constexpr size_t kHalo2RetailAnchorCount =
    sizeof(kHalo2RetailAnchors) / sizeof(kHalo2RetailAnchors[0]);

static_assert(kHalo2RetailAnchors[kHalo2AnchorPlayerWindow].rva ==
    kHalo2RetailRenderPlayerWindowRva);
static_assert(kHalo2RetailAnchors[kHalo2AnchorRenderView].rva ==
    kHalo2RetailRenderViewRva);

constexpr uint32_t Halo2RetailAnchorRelativeTargetCount()
{
    uint32_t count = 0;
    for (const Halo2RetailAnchor& anchor : kHalo2RetailAnchors)
        if (anchor.relativeDispOffset != 0)
            ++count;
    return count;
}

inline constexpr uint32_t kHalo2RetailAnchorRelativeTargets =
    Halo2RetailAnchorRelativeTargetCount();

struct Halo2ColdObservationResult
{
    bool moduleRangeValid = false;
    bool peIdentity = false;
    uint32_t anchorsMatchedOnce = 0;
    uint32_t anchorsAtPinnedRva = 0;
    uint32_t relativeTargetsAtPinnedRva = 0;
    bool postInitializationTickObserved = false;
    bool mappingStable = false;
};

constexpr bool Halo2ColdObservationPass(
    const Halo2ColdObservationResult& result)
{
    return result.moduleRangeValid && result.peIdentity &&
        result.anchorsMatchedOnce == kHalo2RetailAnchorCount &&
        result.anchorsAtPinnedRva == kHalo2RetailAnchorCount &&
        result.relativeTargetsAtPinnedRva ==
            kHalo2RetailAnchorRelativeTargets &&
        result.postInitializationTickObserved && result.mappingStable;
}

constexpr bool Halo2ColdObservationNeedsImageScan(
    bool completedForModuleInstance)
{
    return !completedForModuleInstance;
}

// Pure decision core for the read-only game-time liveness gate. The false->true
// initialized transition is only a new baseline: it is never mistaken for a
// game tick. A later different tick proves game_update reached the official
// active-update tail. `!=` is deliberate: the official code permits save-state
// restoration and uint32 wrap, neither of which invalidates the clock. If
// observation starts mid-level, six seconds of uninterrupted tick changes is
// the conservative already-running path used by the established title gates.
class Halo2GameTimeGateLogic
{
public:
    enum class Decision : uint8_t
    {
        Hold = 0,
        OpenAfterBoundaryThenTick,
        OpenAlreadyRunning,
    };

    // Six seconds at the title worker's 50 ms cadence. Kept equal to the
    // established gate, but implemented independently because H2's fast path
    // accepts ONLY the engine's explicit uninitialized lifecycle state; an
    // unchanged initialized clock is not allowed to manufacture that boundary.
    static constexpr uint32_t kAlreadyRunningSamples = 120;

    Decision Observe(bool initialized, uint32_t tick)
    {
        if (!initialized)
        {
            // halo2.dll normally remains resident while MCC leaves a level.
            // Process the engine's explicit level-dispose state even after a
            // prior open so liveness cannot remain falsely latched in menus or
            // carry across a later load.
            m_sawUninitialized = true;
            m_haveInitializedSample = false;
            m_changeRun = 0;
            ++m_stillRun;
            m_open = false;
            m_lastDecision = Decision::Hold;
            return Decision::Hold;
        }
        if (m_open)
            return m_lastDecision;
        if (!m_haveInitializedSample)
        {
            m_haveInitializedSample = true;
            m_tick = tick;
            return Decision::Hold;
        }
        const bool changed = tick != m_tick;
        m_tick = tick;
        if (!changed)
        {
            m_changeRun = 0;
            ++m_stillRun;
            return Decision::Hold;
        }
        m_stillRun = 0;
        if (m_sawUninitialized)
        {
            m_open = true;
            m_lastDecision = Decision::OpenAfterBoundaryThenTick;
            return m_lastDecision;
        }
        if (++m_changeRun >= kAlreadyRunningSamples)
        {
            m_open = true;
            m_lastDecision = Decision::OpenAlreadyRunning;
            return m_lastDecision;
        }
        return Decision::Hold;
    }

    // A null/unreadable/racy engine sample must not manufacture a still frame.
    // Preserve prior genuine frozen evidence, but force the next readable
    // initialized value to become a new baseline.
    void InvalidateSample()
    {
        m_haveInitializedSample = false;
        // The already-running proof is explicitly consecutive. A missing,
        // unreadable, or incoherent sample breaks that run even when a prior
        // real uninitialized lifecycle boundary remains valid evidence.
        m_changeRun = 0;
        m_stillRun = 0;
        m_open = false;
        m_lastDecision = Decision::Hold;
    }

    void Reset()
    {
        m_tick = 0;
        m_changeRun = 0;
        m_stillRun = 0;
        m_haveInitializedSample = false;
        m_sawUninitialized = false;
        m_open = false;
        m_lastDecision = Decision::Hold;
    }

    bool IsOpen() const { return m_open; }
    bool SawUninitialized() const { return m_sawUninitialized; }
    uint32_t ChangeRun() const { return m_changeRun; }
    uint32_t StillRun() const { return m_stillRun; }

private:
    uint32_t m_tick = 0;
    uint32_t m_changeRun = 0;
    uint32_t m_stillRun = 0;
    bool m_haveInitializedSample = false;
    bool m_sawUninitialized = false;
    bool m_open = false;
    Decision m_lastDecision = Decision::Hold;
};

// ---------------------------------------------------------------------------
// E-H2-3: Halo 2 ships two renderers in one module, and the classic Blam tree
// is gated off whole while the remastered (Saber GroundHog) renderer owns the
// frame. Every constant below is byte-proven against the pinned retail module;
// see docs/HALO2-SIGNATURE-EVIDENCE.md. Nothing here reads memory - these are
// pure descriptions plus pure decision logic so the policy stays testable.
// ---------------------------------------------------------------------------

enum class Halo2GraphicsMode : uint8_t
{
    Unknown = 0,
    Classic,
    Remastered,
};

// The classic per-frame driver 0x95FEC0 bails on its second instruction when
// this byte is non-zero. The mode dword is written by the render-mode applier
// 0x511E0, which passes (appliedMode == 0) to the Saber SSL argument named
// "isLegacy" - that is what proves the polarity.
inline constexpr uint32_t kHalo2ClassicRenderDriverRva = 0x0095FEC0;
inline constexpr uint32_t kHalo2ClassicRenderDisabledByteRva = 0x00E70CF8;
inline constexpr uint32_t kHalo2AppliedRenderModeRva = 0x00E21280;
inline constexpr uint32_t kHalo2RequestedRenderModeRva = 0x00E21278;
inline constexpr uint32_t kHalo2RenderModeApplierRva = 0x000511E0;

// Unique in the complete mapped image of both pinned retail editions.
// Bytes: push rbx / sub rsp,0x20 / cmp byte [rip+disp32],0 / jne / call.
// The cmp disp32 sits at +8 and is based at +13.
inline constexpr char kHalo2ClassicRenderGatePattern[] =
    "40 53 48 83 EC 20 80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? E8";
inline constexpr uint32_t kHalo2ClassicRenderGateDispOffset = 8;
inline constexpr uint32_t kHalo2ClassicRenderGateNextOffset = 13;

constexpr Halo2GraphicsMode Halo2GraphicsModeFromAppliedMode(
    int32_t appliedMode) noexcept
{
    return appliedMode == 0 ? Halo2GraphicsMode::Classic
                            : Halo2GraphicsMode::Remastered;
}

// The gate byte is the render-side truth: it is what the classic driver
// actually tests. A candidate must agree with it, not with the mode dword
// alone, because the applier writes the dword and the byte in that order.
constexpr bool Halo2ClassicRenderTreeRuns(
    uint8_t classicDisabledByte) noexcept
{
    return classicDisabledByte == 0;
}

constexpr bool Halo2GraphicsModeIsCoherent(
    int32_t appliedMode, uint8_t classicDisabledByte) noexcept
{
    return Halo2ClassicRenderTreeRuns(classicDisabledByte) ==
        (Halo2GraphicsModeFromAppliedMode(appliedMode) ==
         Halo2GraphicsMode::Classic);
}

// A classic-path render hook receiving zero callbacks is expected, not broken,
// while the remastered renderer owns the frame. This is the decision that four
// earlier candidates lacked.
constexpr bool Halo2ClassicRenderHookCanFire(
    Halo2GraphicsMode mode, uint8_t classicDisabledByte) noexcept
{
    return mode == Halo2GraphicsMode::Classic &&
        Halo2ClassicRenderTreeRuns(classicDisabledByte);
}

// ---------------------------------------------------------------------------
// E-H2-4: the observer is the one camera root upstream of BOTH renderers.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kHalo2ObserverArrayRva = 0x015F28B8;
inline constexpr uint32_t kHalo2ObserverStride = 0x368;
inline constexpr uint32_t kHalo2ObserverCount = 4;
inline constexpr uint32_t kHalo2ObserverResultOffset = 0xC4;
inline constexpr uint32_t kHalo2ObserverResultArrayRva = 0x015F297C;
inline constexpr uint32_t kHalo2ObserverHeaderSignature = 0x72616421;
inline constexpr uint32_t kHalo2ObserverTrailerOffset = 0x360;
inline constexpr uint32_t kHalo2ObserverUpdatedForFrameOffset = 0xC0;

// observer_get_result(user): imul rax,rbx,0x368 / lea rcx,[rip+disp32] /
// add rax,rcx. Unique in the complete mapped image; the stride is inside the
// signature itself, so a moved array cannot silently change the element size.
inline constexpr char kHalo2ObserverResultAccessorPattern[] =
    "48 69 C3 68 03 00 00 48 8D 0D ?? ?? ?? ?? 48 03 C1";
inline constexpr uint32_t kHalo2ObserverResultAccessorRva = 0x006F0E79;
inline constexpr uint32_t kHalo2ObserverResultAccessorDispOffset = 10;
inline constexpr uint32_t kHalo2ObserverResultAccessorNextOffset = 14;
inline constexpr uint32_t kHalo2ObserverResultAccessorStrideOffset = 3;

// s_observer_result. The camera builder 0x7DF5A0 copies exactly these fields.
inline constexpr uint32_t kHalo2ObserverResultPositionOffset = 0x00;
inline constexpr uint32_t kHalo2ObserverResultForwardOffset = 0x20;
inline constexpr uint32_t kHalo2ObserverResultUpOffset = 0x2C;
inline constexpr uint32_t kHalo2ObserverResultHorizontalFovOffset = 0x38;
inline constexpr uint32_t kHalo2ObserverResultAspectOffset = 0x3C;
inline constexpr uint32_t kHalo2ObserverResultVerticalFovOffset = 0x4C;
inline constexpr uint32_t kHalo2ObserverResultFovRatioOffset = 0x50;
inline constexpr uint32_t kHalo2ObserverResultOwnedBytes = 0x54;

// The last per-frame writer of position/forward/up. Injecting after this
// function returns is the only point where no engine code overwrites the pose
// again before either renderer consumes it.
inline constexpr uint32_t kHalo2ObserverFinalTransformRva = 0x006F0250;
inline constexpr uint32_t kHalo2ObserverUpdateAllRva = 0x006F1A60;
inline constexpr uint32_t kHalo2ObserverResultDeriveRva = 0x006F10E0;

constexpr uint64_t Halo2ObserverRecordOffset(uint32_t user) noexcept
{
    return static_cast<uint64_t>(user) * kHalo2ObserverStride;
}

constexpr uint64_t Halo2ObserverResultOffsetForUser(uint32_t user) noexcept
{
    return Halo2ObserverRecordOffset(user) + kHalo2ObserverResultOffset;
}

constexpr bool Halo2ObserverUserValid(uint32_t user) noexcept
{
    return user < kHalo2ObserverCount;
}

// The three owned observer spans, expressed exactly as the engine stores them.
// They are NOT contiguous, so a whole-struct copy is forbidden: fields between
// them belong to the engine.
enum class Halo2ObserverSpan : uint8_t
{
    Position = 0,
    Forward,
    Up,
};
inline constexpr uint32_t kHalo2ObserverOwnedSpanCount = 3;

constexpr uint32_t Halo2ObserverSpanOffset(Halo2ObserverSpan span) noexcept
{
    return span == Halo2ObserverSpan::Position
        ? kHalo2ObserverResultPositionOffset
        : (span == Halo2ObserverSpan::Forward
               ? kHalo2ObserverResultForwardOffset
               : kHalo2ObserverResultUpOffset);
}

constexpr uint32_t Halo2ObserverSpanBytes(Halo2ObserverSpan) noexcept
{
    return kHalo2CameraVectorBytes;
}

// Only the three pose vectors may ever be written. Field of view, aspect,
// cluster/leaf indices and velocity remain engine-owned: the vertical FOV at
// +0x4C is derived by 0x6F1700 from +0x38 and +0x3C, so writing one without
// the other produces an incoherent projection.
constexpr bool Halo2ObserverSpanWriteAllowed(
    uint32_t resultRelativeOffset, size_t bytes) noexcept
{
    if (bytes != kHalo2CameraVectorBytes)
        return false;
    return resultRelativeOffset == kHalo2ObserverResultPositionOffset ||
        resultRelativeOffset == kHalo2ObserverResultForwardOffset ||
        resultRelativeOffset == kHalo2ObserverResultUpOffset;
}

// ---------------------------------------------------------------------------
// The Blam to Saber camera hand-off, replicated exactly from halo2.dll
// 0x5F510 so no conversion is invented. Saber works in metres and swaps axes:
//   saber(v) = (v.x, v.z, -v.y)
// row0 = saber(forward) x saber(up), row1 = saber(up), row2 = saber(forward),
// row3 = saber(position) * 3.048 + offsets + forwardScale * saber(forward).
// ---------------------------------------------------------------------------

inline constexpr uint32_t kHalo2SaberCameraBridgeRva = 0x0005F510;
inline constexpr uint32_t kHalo2SaberCameraPushRva = 0x00051510;
inline constexpr uint32_t kHalo2SaberFrameDriverRva = 0x000515E0;
inline constexpr uint32_t kHalo2SaberSceneSlotRva = 0x01E91210;
inline constexpr uint32_t kHalo2SaberSceneCameraListOffset = 0x100;
inline constexpr uint32_t kHalo2SaberSceneCameraCountOffset = 0x108;
inline constexpr uint32_t kHalo2SaberCameraMatrixOffset = 0x00;
inline constexpr uint32_t kHalo2SaberCameraFovChangedOffset = 0x14C;
// E-H2-7 (2026-08-21): +0x150 is the HORIZONTAL and +0x154 the VERTICAL
// field of view in degrees, proven three ways: the projection builder
// 0x1C82C0 maps +0x150 to P[0][0] (0x1C8386) and +0x154 to P[1][1]
// (0x1C8390); the constructor's aspect is 0.75 for 640x480, i.e. HEIGHT /
// WIDTH (0xBC162); and 0xBC4F0(cam, 80) writes +0x150 = 80 and +0x154 =
// 2*atan(tan 40 deg * 0.75) = 64 deg, the classic 4:3 pair. The earlier
// constants had the two names swapped.
inline constexpr uint32_t kHalo2SaberCameraHorizontalFovDegreesOffset = 0x150;
inline constexpr uint32_t kHalo2SaberCameraVerticalFovDegreesOffset = 0x154;
inline constexpr uint32_t kHalo2SaberCameraAspectOffset = 0x158;
inline constexpr uint32_t kHalo2SaberCameraNearOffset = 0x80;
// 0xBC560(cam, verticalDegrees): +0x154 = V, +0x150 = 2*atan(tan(V/2) /
// aspect), then jmp 0xBC380.
inline constexpr uint32_t kHalo2SaberCameraSetFovRva = 0x000BC560;
// 0xBC380(cam): refreshes the near-plane rectangle/polygon and the
// pixels-per-metre fields (+0x88..+0xA8, +0x12C..+0x138, +0x15C/+0x160)
// from +0x150/+0x154/+0x80/+0x144/+0x148. Reads neither +0x158 nor the
// matrix. Writing both degree fields and calling this is the engine's own
// way to a field of view that is not aspect-locked.
inline constexpr uint32_t kHalo2SaberCameraRefreshRectRva = 0x000BC380;
inline constexpr uint8_t kHalo2SaberCameraRefreshRectEntryBytes[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0xF3, 0x0F, 0x10, 0x81, 0x50, 0x01,
    0x00, 0x00};
inline constexpr uint32_t kHalo2SaberCameraCommitRva = 0x000BC2B0;
inline constexpr uint8_t kHalo2SaberCameraCommitEntryBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
    0x83, 0xEC, 0x40};

// Saber render thread: frame entry -> view loop -> scene render. The view loop
// already runs the scene render more than once per frame, which is why a
// per-eye second pass is native rather than a replay.
inline constexpr uint32_t kHalo2SaberFrameRenderRva = 0x002DC3D0;
inline constexpr uint32_t kHalo2SaberViewLoopRva = 0x002DEC00;
inline constexpr uint32_t kHalo2SaberSceneRenderRva = 0x002DF190;

// Engine-owned translation constants read by 0x5F510. They are module globals,
// not literals, so a candidate must read them rather than bake them in.
inline constexpr uint32_t kHalo2SaberOffsetXRva = 0x01E8D058;
inline constexpr uint32_t kHalo2SaberOffsetZRva = 0x01E8D05C;
inline constexpr uint32_t kHalo2SaberOffsetYRva = 0x01E8D060;
inline constexpr uint32_t kHalo2SaberForwardScaleRva = 0x01E8D08C;

struct Halo2SaberViewMatrix
{
    float m[16]{};
};

struct Halo2SaberCameraConstants
{
    float offsetX = 0.0f;
    float offsetZ = 0.0f;
    float offsetY = 0.0f;
    float forwardScale = 0.0f;
};

inline bool Halo2SaberCameraConstantsFinite(
    const Halo2SaberCameraConstants& constants) noexcept
{
    return std::isfinite(constants.offsetX) &&
        std::isfinite(constants.offsetZ) &&
        std::isfinite(constants.offsetY) &&
        std::isfinite(constants.forwardScale);
}

inline bool Halo2BuildSaberViewMatrix(
    const Halo2CameraBasis& basis,
    const Halo2SaberCameraConstants& constants,
    Halo2SaberViewMatrix& out) noexcept
{
    if (!Halo2ValidateCameraBasis(basis) ||
        !Halo2SaberCameraConstantsFinite(constants))
    {
        return false;
    }

    const float forward[3] = {
        basis.forward[0], basis.forward[2], -basis.forward[1]};
    const float up[3] = {basis.up[0], basis.up[2], -basis.up[1]};
    const float right[3] = {
        forward[1] * up[2] - forward[2] * up[1],
        forward[2] * up[0] - forward[0] * up[2],
        forward[0] * up[1] - forward[1] * up[0]};

    Halo2SaberViewMatrix candidate{};
    candidate.m[0] = right[0];
    candidate.m[1] = right[1];
    candidate.m[2] = right[2];
    candidate.m[3] = 0.0f;
    candidate.m[4] = up[0];
    candidate.m[5] = up[1];
    candidate.m[6] = up[2];
    candidate.m[7] = 0.0f;
    candidate.m[8] = forward[0];
    candidate.m[9] = forward[1];
    candidate.m[10] = forward[2];
    candidate.m[11] = 0.0f;
    candidate.m[12] = basis.position[0] * kHalo2MetersPerWorldUnit +
        constants.offsetX + constants.forwardScale * forward[0];
    candidate.m[13] = basis.position[2] * kHalo2MetersPerWorldUnit +
        constants.offsetZ + constants.forwardScale * forward[1];
    candidate.m[14] = (constants.offsetY -
                       basis.position[1] * kHalo2MetersPerWorldUnit) +
        constants.forwardScale * forward[2];
    candidate.m[15] = 1.0f;

    for (int index = 0; index < 16; ++index)
        if (!std::isfinite(candidate.m[index]))
            return false;
    out = candidate;
    return true;
}

// 0xBC560(cam, V) stores the VERTICAL field of view and derives the
// HORIZONTAL one as 2*atan(tan(V/2) / aspect), where the Saber aspect is
// HEIGHT / WIDTH (0.75 for 640x480). This mirrors that derivation exactly;
// degrees, as the engine stores.
inline bool Halo2SaberHorizontalFovDegreesFromVertical(
    float verticalFovDegrees, float aspectHeightOverWidth,
    float& outHorizontalDegrees) noexcept
{
    constexpr float kPi = 3.14159265f;
    if (!std::isfinite(verticalFovDegrees) ||
        !std::isfinite(aspectHeightOverWidth) ||
        verticalFovDegrees <= 0.0f || verticalFovDegrees >= 180.0f ||
        aspectHeightOverWidth <= 0.0f)
    {
        return false;
    }
    const float halfRadians = verticalFovDegrees * 0.5f * kPi / 180.0f;
    const float tangent = std::tan(halfRadians);
    if (!std::isfinite(tangent) || tangent <= 0.0f)
        return false;
    const float horizontal = std::atan(tangent / aspectHeightOverWidth);
    if (!std::isfinite(horizontal) || horizontal <= 0.0f)
        return false;
    outHorizontalDegrees = (horizontal + horizontal) * 180.0f / kPi;
    return std::isfinite(outHorizontalDegrees) &&
        outHorizontalDegrees > 0.0f && outHorizontalDegrees < 180.0f;
}

// The headset cover for one Saber eye pass: the smallest symmetric
// horizontal/vertical pair that contains both eyes' native frusta, each
// axis solved on its own because 0x1C82C0 takes the two fields
// independently (no aspect lock). Half-angles in radians for the
// compositor, full angles in degrees for the camera record.
struct Halo2SaberEyeCover
{
    float halfHorizontalRadians = 0.0f;
    float halfVerticalRadians = 0.0f;
    float horizontalDegrees = 0.0f;
    float verticalDegrees = 0.0f;
};

inline bool Halo2DeriveSaberEyeCover(
    const float leftFov[4], const float rightFov[4],
    Halo2SaberEyeCover& out) noexcept
{
    constexpr float kPi = 3.14159265f;
    constexpr float kMaximumAngle = 1.56f;
    constexpr float kCoverMargin = 1.002f;
    if (!leftFov || !rightFov)
        return false;
    float requiredTanX = 0.0f;
    float requiredTanY = 0.0f;
    const float* views[2] = {leftFov, rightFov};
    for (const float* fov : views)
    {
        // OpenXR order: left (<0), right (>0), up (>0), down (<0).
        if (!(fov[0] < 0.0f) || !(fov[1] > 0.0f) || !(fov[2] > 0.0f) ||
            !(fov[3] < 0.0f))
        {
            return false;
        }
        for (int index = 0; index < 4; ++index)
            if (!std::isfinite(fov[index]) ||
                std::fabs(fov[index]) >= kMaximumAngle)
                return false;
        requiredTanX = (std::max)(requiredTanX,
            (std::max)(std::fabs(std::tan(fov[0])),
                       std::fabs(std::tan(fov[1]))));
        requiredTanY = (std::max)(requiredTanY,
            (std::max)(std::fabs(std::tan(fov[2])),
                       std::fabs(std::tan(fov[3]))));
    }
    Halo2SaberEyeCover candidate{};
    candidate.halfHorizontalRadians =
        std::atan(requiredTanX * kCoverMargin);
    candidate.halfVerticalRadians =
        std::atan(requiredTanY * kCoverMargin);
    candidate.horizontalDegrees =
        2.0f * candidate.halfHorizontalRadians * 180.0f / kPi;
    candidate.verticalDegrees =
        2.0f * candidate.halfVerticalRadians * 180.0f / kPi;
    if (!std::isfinite(candidate.halfHorizontalRadians) ||
        !std::isfinite(candidate.halfVerticalRadians) ||
        candidate.halfHorizontalRadians <= 0.01f ||
        candidate.halfVerticalRadians <= 0.01f ||
        candidate.halfHorizontalRadians >= 1.55f ||
        candidate.halfVerticalRadians >= 1.55f)
    {
        return false;
    }
    out = candidate;
    return true;
}

// ---------------------------------------------------------------------------
// The remastered renderer's per-view record. Byte-verified at 0x2DF2C5:
//   record = *(halo2 + kHalo2SaberViewCollectionSlotRva)
//          + kHalo2SaberViewRecordArrayOffset
//          + viewIndex * kHalo2SaberViewRecordStride
// The scene render latches the embedded camera copy at record+0x20 and the
// record itself into its render context, and never reads the Saber camera
// object. A per-eye camera therefore lives here, not in that object.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kHalo2SaberViewCollectionSlotRva = 0x01A250F8;
inline constexpr uint32_t kHalo2SaberViewCountOffset = 0x148;
inline constexpr uint32_t kHalo2SaberViewRecordArrayOffset = 0x150;
inline constexpr uint32_t kHalo2SaberViewRecordStride = 0x758;
inline constexpr uint32_t kHalo2SaberViewRecordCapacity = 0x50;
inline constexpr uint32_t kHalo2SaberViewRecordCameraOffset = 0x20;
inline constexpr uint32_t kHalo2SaberViewRecordCameraBytes = 0x398;
inline constexpr uint32_t kHalo2SaberViewRecordViewIdOffset = 0x10;

// The engine's own rebuild of every derived matrix in the record. It takes
// FOUR arguments: 0x1C7740 clears r9 too (0x1C790D xor r9d,r9d) and
// 0x1C6D80 stores r9 (0x1C6DD7) and, when non-null, dereferences it as a
// float[4] clip plane and rewrites the projection. It must be called with
// all three pointers null. It reads +0x150/+0x154 in DEGREES through
// 0x1C82C0 (P[0][0] = 1/tan(h/2), P[1][1] = 1/tan(v/2)); no clamp exists.
inline constexpr uint32_t kHalo2SaberRebuildViewMatricesRva = 0x001C6D80;
inline constexpr uint8_t kHalo2SaberRebuildViewMatricesEntryBytes[] = {
    0x4C, 0x8B, 0xDC, 0x55, 0x49, 0x8D, 0xAB, 0xA8, 0xFB, 0xFF, 0xFF};
// Normalises the camera basis rows and produces the inverse at camera+0x40.
inline constexpr uint32_t kHalo2SaberCameraCommitRvaAlias =
    kHalo2SaberCameraCommitRva;

// The scene render consumes a once-per-frame latch and clears it to -1, so a
// second pass in the same frame must restore it or silently skip work.
inline constexpr uint32_t kHalo2SaberSceneOnceLatchRva = 0x00E21D30;
// Its callers zero this span of the render context before every call.
inline constexpr uint32_t kHalo2SaberSceneContextResetOffset = 0x10;
inline constexpr uint32_t kHalo2SaberSceneContextResetBytes = 0xC8;

// Screen-space reflections read a single frame-to-frame history colour target.
// One engine-owned byte disables that pass outright, which is the only way to
// keep two eyes in one frame from sharing one history.
inline constexpr uint32_t kHalo2SaberRenderSettingsSlotRva = 0x01A6E588;
inline constexpr uint32_t kHalo2SaberSsrEnableOffset = 0x108;
