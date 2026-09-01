#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "runtime_types.h"

inline constexpr uint64_t kCutsceneTheaterFadeMs = 100;
inline constexpr uint64_t kCinematicControlFreshMs = 500;

struct CinematicControlPublication
{
    GameTitle title = GameTitle::None;
    uint32_t generation = 0;
    CinematicControlState state = CinematicControlState::Unknown;
    uint64_t heartbeatMs = 0;
};

struct CutsceneTheaterProjectionPublication
{
    GameTitle title = GameTitle::None;
    uint32_t generation = 0;
    float authoredAspect = 0.0f;
    uint64_t heartbeatMs = 0;
};

inline float CutsceneTheaterAspectFromTangents(
    float horizontalTangent, float verticalTangent) noexcept
{
    if (!std::isfinite(horizontalTangent) ||
        !std::isfinite(verticalTangent) ||
        horizontalTangent <= 0.0001f || verticalTangent <= 0.0001f)
    {
        return 0.0f;
    }
    const float aspect = horizontalTangent / verticalTangent;
    return std::isfinite(aspect) && aspect >= 0.25f && aspect <= 4.0f
        ? aspect : 0.0f;
}

inline float CutsceneTheaterAspectFromProjectionScales(
    float projectionX, float projectionY) noexcept
{
    if (!std::isfinite(projectionX) || !std::isfinite(projectionY) ||
        std::fabs(projectionX) <= 0.0001f ||
        std::fabs(projectionY) <= 0.0001f)
    {
        return 0.0f;
    }
    // tan(horizontal half-FOV) / tan(vertical half-FOV) = P[5] / P[0].
    return CutsceneTheaterAspectFromTangents(
        std::fabs(projectionY), std::fabs(projectionX));
}

inline float CutsceneTheaterAspectFromDimensions(
    uint32_t width, uint32_t height) noexcept
{
    if (!width || !height)
        return 0.0f;
    const float aspect = static_cast<float>(width) /
        static_cast<float>(height);
    return std::isfinite(aspect) && aspect >= 0.25f && aspect <= 4.0f
        ? aspect : 0.0f;
}

inline bool CutsceneTheaterProjectionMatchesAspect(
    float authoredAspect, float projectionX, float projectionY,
    float tolerance = 0.001f) noexcept
{
    const float actualAspect =
        CutsceneTheaterAspectFromProjectionScales(projectionX, projectionY);
    return std::isfinite(authoredAspect) && authoredAspect >= 0.25f &&
        authoredAspect <= 4.0f && actualAspect > 0.0f &&
        std::isfinite(tolerance) && tolerance >= 0.0f &&
        std::fabs(actualAspect - authoredAspect) <= tolerance;
}

inline bool CutsceneTheaterRequested(
    bool enabled, CinematicControlState state) noexcept
{
    return enabled && state == CinematicControlState::AuthoredLocked;
}

inline bool ShouldDisableWidescreenCinematicFov(
    bool modEnabled, bool stereoEnabled, bool theaterActive) noexcept
{
    // The legacy widening remains exactly the immersive-VR policy. Theatre
    // restores the engine's stock authored cinematic FOV instead.
    return modEnabled && stereoEnabled && !theaterActive;
}

inline bool UseAuthoredCutsceneProjection(
    bool theaterActive, float authoredAspect) noexcept
{
    return theaterActive && std::isfinite(authoredAspect) &&
        authoredAspect >= 0.25f && authoredAspect <= 4.0f;
}

inline CinematicControlState ClassifyHalo3FamilyCinematicControl(
    bool globalsAvailable, bool cinematicInProgress,
    bool shotStateAvailable) noexcept
{
    if (!globalsAvailable)
        return CinematicControlState::Unknown;
    if (!cinematicInProgress)
        return CinematicControlState::PlayerControlled;
    return shotStateAvailable
        ? CinematicControlState::AuthoredLocked
        : CinematicControlState::Unknown;
}

inline CinematicControlState ClassifyOdstCinematicControl(
    CinematicControlState cinematicState,
    bool userInputConstraintStateAvailable,
    const float maximumLookAngles[4],
    const float maximumLookAngleRates[4],
    int32_t interpolationTicksRemaining) noexcept
{
    // ODST's authored cinematic tag can leave camera look enabled for a shot.
    // Preserve weaker common evidence as-is, but never call an active cinematic
    // locked unless its live title-native look constraints prove zero freedom.
    if (cinematicState != CinematicControlState::AuthoredLocked)
        return cinematicState;
    if (!userInputConstraintStateAvailable || !maximumLookAngles ||
        !maximumLookAngleRates || interpolationTicksRemaining < 0 ||
        interpolationTicksRemaining > 360000)
    {
        return CinematicControlState::Unknown;
    }

    constexpr float kLookFreedomEpsilon = 0.0001f;
    bool currentLookFreedom = false;
    bool pendingLookFreedom = false;
    for (int axis = 0; axis < 4; ++axis)
    {
        const float angle = maximumLookAngles[axis];
        const float rate = maximumLookAngleRates[axis];
        if (!std::isfinite(angle) || !std::isfinite(rate))
            return CinematicControlState::Unknown;
        currentLookFreedom |= std::fabs(angle) > kLookFreedomEpsilon;
        pendingLookFreedom |= interpolationTicksRemaining > 0 &&
            std::fabs(rate) > kLookFreedomEpsilon;
    }
    return currentLookFreedom || pendingLookFreedom
        ? CinematicControlState::PlayerControlled
        : CinematicControlState::AuthoredLocked;
}

inline constexpr uint16_t kHalo4CinematicCameraType = 6;
inline constexpr int32_t kHalo4CinematicMaximumTicks = 360000;
inline constexpr float kHalo4CinematicLookFreedomEpsilon = 0.0001f;

// Stage 3BX's H4EK/retail-proven look-constraint rule. Halo 4's post-link
// implementation intentionally treated non-finite angle/rate bits as look
// freedom: malformed live camera data must remain immersive, never admit a
// room-fixed theatre screen. Rates are not required when no interpolation is
// pending because the engine does not consume them in that state either.
inline CinematicControlState ClassifyHalo4CinematicControl(
    bool cinematicGlobalsAvailable, bool cinematicInProgress,
    bool cameraStateAvailable, uint16_t cameraType,
    const float maximumLookAngles[4],
    const float maximumLookAngleRates[4],
    int32_t interpolationTicksRemaining) noexcept
{
    if (!cinematicGlobalsAvailable)
        return CinematicControlState::Unknown;
    if (!cinematicInProgress)
        return CinematicControlState::PlayerControlled;
    if (!cameraStateAvailable || cameraType != kHalo4CinematicCameraType ||
        !maximumLookAngles || interpolationTicksRemaining < 0 ||
        interpolationTicksRemaining > kHalo4CinematicMaximumTicks)
    {
        return CinematicControlState::Unknown;
    }

    for (int axis = 0; axis < 4; ++axis)
    {
        const float angle = maximumLookAngles[axis];
        if (!std::isfinite(angle) ||
            std::fabs(angle) > kHalo4CinematicLookFreedomEpsilon)
        {
            return CinematicControlState::PlayerControlled;
        }
    }
    if (interpolationTicksRemaining == 0)
        return CinematicControlState::AuthoredLocked;
    if (!maximumLookAngleRates)
        return CinematicControlState::Unknown;
    for (int axis = 0; axis < 4; ++axis)
    {
        const float rate = maximumLookAngleRates[axis];
        if (!std::isfinite(rate) ||
            std::fabs(rate) > kHalo4CinematicLookFreedomEpsilon)
        {
            return CinematicControlState::PlayerControlled;
        }
    }
    return CinematicControlState::AuthoredLocked;
}

inline CinematicControlState ClassifyReachCinematicControl(
    bool cinematicGlobalsProven, uint32_t stateWord) noexcept
{
    if (!cinematicGlobalsProven)
        return CinematicControlState::Unknown;
    return (stateWord & 0xFFu) != 0
        ? CinematicControlState::AuthoredLocked
        : CinematicControlState::PlayerControlled;
}

inline CinematicControlState ResolveCinematicControl(
    const CinematicControlPublication& publication,
    GameTitle activeTitle, uint32_t activeGeneration,
    uint32_t activeCapabilities, uint64_t nowMs,
    uint64_t freshForMs = kCinematicControlFreshMs) noexcept
{
    if ((activeCapabilities & TitleCapability_CutsceneTheater) == 0 ||
        activeTitle == GameTitle::None || activeTitle == GameTitle::Unknown ||
        publication.title != activeTitle || !activeGeneration ||
        publication.generation != activeGeneration ||
        !publication.heartbeatMs || nowMs < publication.heartbeatMs ||
        nowMs - publication.heartbeatMs > freshForMs)
    {
        return CinematicControlState::Unknown;
    }
    return publication.state;
}

inline bool ResolveCutsceneTheaterProjection(
    const CutsceneTheaterProjectionPublication& publication,
    GameTitle activeTitle, uint32_t activeGeneration,
    uint32_t activeCapabilities, uint64_t nowMs,
    float& authoredAspect,
    uint64_t freshForMs = kCinematicControlFreshMs) noexcept
{
    authoredAspect = 0.0f;
    if ((activeCapabilities & TitleCapability_CutsceneTheater) == 0 ||
        activeTitle == GameTitle::None || activeTitle == GameTitle::Unknown ||
        publication.title != activeTitle || !activeGeneration ||
        publication.generation != activeGeneration ||
        !publication.heartbeatMs || nowMs < publication.heartbeatMs ||
        nowMs - publication.heartbeatMs > freshForMs ||
        !std::isfinite(publication.authoredAspect) ||
        publication.authoredAspect < 0.25f ||
        publication.authoredAspect > 4.0f)
    {
        return false;
    }
    authoredAspect = publication.authoredAspect;
    return true;
}

inline void ApplyCutsceneTheaterEyeTransform(
    bool active, float depth,
    float position[3], float orientation[4]) noexcept
{
    if (!active || !position || !orientation)
        return;
    const float scale = std::clamp(
        std::isfinite(depth) ? depth : 1.0f, 0.0f, 2.0f);
    for (int axis = 0; axis < 3; ++axis)
        position[axis] *= scale;
    // A virtual stereo screen needs parallel cameras. Runtime eye cant belongs
    // to immersive projection submission, not to the movie encoded in a quad.
    orientation[0] = 0.0f;
    orientation[1] = 0.0f;
    orientation[2] = 0.0f;
    orientation[3] = 1.0f;
}

inline uint32_t CutsceneTheaterImageIndex(
    uint32_t visibleEye, bool flipDepth) noexcept
{
    visibleEye = visibleEye > 1 ? 1 : visibleEye;
    return flipDepth ? 1u - visibleEye : visibleEye;
}

struct CutsceneTheaterClipVertex
{
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
};

// Project a room-fixed theatre rectangle into one ordinary OpenXR projection
// view. This deliberately uses no compositor eye-selection behavior: each
// physical eye gets a complete projection image, just like immersive gameplay.
// Quaternion arrays use OpenXR's x/y/z/w order and all positions are in the
// same LOCAL reference space.
inline bool BuildCutsceneTheaterProjectionQuad(
    const float eyePosition[3], const float eyeOrientation[4],
    const float screenCenter[3], const float screenOrientation[4],
    float widthMeters, float heightMeters,
    const float fov[4], CutsceneTheaterClipVertex (&vertices)[4]) noexcept
{
    if (!eyePosition || !eyeOrientation || !screenCenter ||
        !screenOrientation || !fov ||
        !std::isfinite(widthMeters) || !std::isfinite(heightMeters) ||
        widthMeters <= 0.0f || heightMeters <= 0.0f)
        return false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(eyePosition[i]) || !std::isfinite(screenCenter[i]))
            return false;
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(eyeOrientation[i]) ||
            !std::isfinite(screenOrientation[i]) || !std::isfinite(fov[i]))
            return false;

    // fov = left/right/up/down, matching XrFovf.
    const float tanLeft = std::tan(fov[0]);
    const float tanRight = std::tan(fov[1]);
    const float tanUp = std::tan(fov[2]);
    const float tanDown = std::tan(fov[3]);
    if (!std::isfinite(tanLeft) || !std::isfinite(tanRight) ||
        !std::isfinite(tanUp) || !std::isfinite(tanDown) ||
        tanRight - tanLeft <= 1e-5f || tanUp - tanDown <= 1e-5f)
        return false;

    auto normalize = [](const float q[4], float out[4]) noexcept {
        const float lengthSquared =
            q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (!std::isfinite(lengthSquared) || lengthSquared < 1e-8f)
            return false;
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        for (int i = 0; i < 4; ++i)
            out[i] = q[i] * inverseLength;
        return true;
    };
    auto rotate = [](const float q[4], const float v[3],
                     float out[3]) noexcept {
        const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
        const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
        const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
        out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
        out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
        out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
    };

    float eyeQ[4]{}, screenQ[4]{};
    if (!normalize(eyeOrientation, eyeQ) || !normalize(screenOrientation, screenQ))
        return false;
    const float inverseEyeQ[4]{-eyeQ[0], -eyeQ[1], -eyeQ[2], eyeQ[3]};
    const float localRight[3]{1.0f, 0.0f, 0.0f};
    const float localUp[3]{0.0f, 1.0f, 0.0f};
    float screenRight[3]{}, screenUp[3]{};
    rotate(screenQ, localRight, screenRight);
    rotate(screenQ, localUp, screenUp);

    const float halfWidth = widthMeters * 0.5f;
    const float halfHeight = heightMeters * 0.5f;
    // Triangle-strip order and matching UVs:
    // top-left, top-right, bottom-left, bottom-right.
    constexpr float signs[4][2]{
        {-1.0f,  1.0f}, { 1.0f,  1.0f},
        {-1.0f, -1.0f}, { 1.0f, -1.0f}};
    for (int vertex = 0; vertex < 4; ++vertex)
    {
        float delta[3]{};
        for (int axis = 0; axis < 3; ++axis)
        {
            const float corner = screenCenter[axis] +
                screenRight[axis] * halfWidth * signs[vertex][0] +
                screenUp[axis] * halfHeight * signs[vertex][1];
            delta[axis] = corner - eyePosition[axis];
        }
        float view[3]{};
        rotate(inverseEyeQ, delta, view);
        const float clipW = -view[2];
        if (!std::isfinite(clipW) || clipW <= 1e-4f)
            return false;
        const float tangentX = view[0] / clipW;
        const float tangentY = view[1] / clipW;
        const float ndcX =
            2.0f * (tangentX - tanLeft) / (tanRight - tanLeft) - 1.0f;
        const float ndcY =
            2.0f * (tangentY - tanDown) / (tanUp - tanDown) - 1.0f;
        if (!std::isfinite(ndcX) || !std::isfinite(ndcY))
            return false;
        vertices[vertex] = {ndcX * clipW, ndcY * clipW, clipW};
    }
    return true;
}

// The authored cinematic is rasterized into the headset's own render shape, so
// its frame is taller than the 16:9 frame the same shot fills on a monitor:
// both share the engine's horizontal tangent, and only the vertical one grows.
// Everything in that extra height is scene the flat game never shows. The matte
// is the black cine bar pair that hides it again, and it is a pure crop - the
// retained picture keeps its exact size and scale, so this can never rescale,
// stretch or letterbox a picture that is already at or wider than the target.
struct CutsceneTheaterMatte
{
    bool active = false;
    float vMin = 0.0f;
    float vMax = 1.0f;
};

inline CutsceneTheaterMatte ComputeCutsceneTheaterMatte(
    float sourceAspect, float targetAspect, float verticalOffset) noexcept
{
    CutsceneTheaterMatte result{};
    if (!std::isfinite(sourceAspect) || !std::isfinite(targetAspect) ||
        !std::isfinite(verticalOffset) ||
        sourceAspect < 0.25f || sourceAspect > 4.0f ||
        targetAspect < 0.25f || targetAspect > 4.0f ||
        // Nothing to hide: the authored frame is already at or wider than the
        // requested window. Never scale the picture down to manufacture bars.
        sourceAspect >= targetAspect)
    {
        return result;
    }
    const float span = sourceAspect / targetAspect;
    if (!std::isfinite(span) || span <= 0.0f || span >= 1.0f)
        return result;

    // Texture V grows downward, so a positive offset lifts the retained window.
    const float clampedOffset = std::clamp(verticalOffset, -0.5f, 0.5f);
    float vMin = 0.5f - clampedOffset - span * 0.5f;
    // Slide, never shrink: an offset that would run off an edge stops there
    // with the whole window still the requested size.
    vMin = std::clamp(vMin, 0.0f, 1.0f - span);
    result.active = true;
    result.vMin = vMin;
    result.vMax = vMin + span;
    return result;
}

inline float CutsceneTheaterHeight(
    float widthMeters, uint32_t imageWidth, uint32_t imageHeight) noexcept
{
    if (!std::isfinite(widthMeters) || widthMeters <= 0.0f ||
        !imageWidth || !imageHeight)
    {
        return 0.0f;
    }
    return widthMeters * static_cast<float>(imageHeight) /
        static_cast<float>(imageWidth);
}

inline float CutsceneTheaterHeightFromAspect(
    float widthMeters, float authoredAspect) noexcept
{
    if (!std::isfinite(widthMeters) || widthMeters <= 0.0f ||
        !std::isfinite(authoredAspect) || authoredAspect < 0.25f ||
        authoredAspect > 4.0f)
    {
        return 0.0f;
    }
    return widthMeters / authoredAspect;
}

struct CutsceneTheaterTransitionOutput
{
    bool active = false;
    bool switched = false;
    float fadeAlpha = 0.0f;
};

// A continuous fade controller: take 100 ms to black, switch presentation
// there, then take 100 ms back to clear. Retargeting while in flight changes
// direction from the current alpha instead of jumping.
class CutsceneTheaterTransition
{
public:
    CutsceneTheaterTransitionOutput Update(
        uint64_t nowMs, bool requested) noexcept
    {
        if (!m_haveTime)
        {
            m_haveTime = true;
            m_lastMs = nowMs;
        }
        const uint64_t elapsed = nowMs >= m_lastMs ? nowMs - m_lastMs : 0;
        m_lastMs = nowMs;
        const float step = std::min(
            1.0f, static_cast<float>(elapsed) /
                static_cast<float>(kCutsceneTheaterFadeMs));

        CutsceneTheaterTransitionOutput result{};
        if (requested != m_active)
        {
            m_alpha = std::min(1.0f, m_alpha + step);
            if (m_alpha >= 1.0f)
            {
                m_active = requested;
                result.switched = true;
            }
        }
        else
        {
            m_alpha = std::max(0.0f, m_alpha - step);
        }
        result.active = m_active;
        result.fadeAlpha = m_alpha;
        return result;
    }

    void Reset(bool active = false) noexcept
    {
        m_active = active;
        m_alpha = 0.0f;
        m_lastMs = 0;
        m_haveTime = false;
    }

private:
    bool m_active = false;
    bool m_haveTime = false;
    float m_alpha = 0.0f;
    uint64_t m_lastMs = 0;
};
