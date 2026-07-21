#pragma once

#include <cmath>
#include <cstdint>

constexpr uint64_t kOdstCameraFreshMs = 500;
constexpr uint64_t kOdstCameraSoftTimeoutMs = 750;
constexpr uint64_t kOdstCameraHardTimeoutMs = 5000;
constexpr uint64_t kOdstCameraStableMs = 1000;

struct OdstHalo3FovMatch
{
    float compactVerticalInput = 0.0f;
    float compactReferenceInput = 0.0f;
    float projectionX = 0.0f;
    float projectionY = 0.0f;
};

// Halo 3's headset-confirmed path feeds tan(half-FOV) for both compact camera
// scalars, then writes their reciprocals into the final projection. ODST's
// builder is instruction-identical, but its two source fields have different
// stock semantics. This private experiment matches Halo 3's numeric inputs as
// a pair instead of mixing a widened world FOV with ODST's stock FP reference.
inline bool ComputeOdstHalo3FovMatch(
    float halfX, float halfY, OdstHalo3FovMatch& out)
{
    if (!std::isfinite(halfX) || !std::isfinite(halfY) ||
        halfX <= 0.01f || halfX >= 1.55f ||
        halfY <= 0.01f || halfY >= 1.55f)
        return false;
    const float tanX = std::tan(halfX);
    const float tanY = std::tan(halfY);
    if (!std::isfinite(tanX) || !std::isfinite(tanY) ||
        tanX <= 0.01f || tanY <= 0.01f)
        return false;
    out.compactVerticalInput = tanX;
    out.compactReferenceInput = tanY;
    out.projectionX = 1.0f / tanX;
    out.projectionY = 1.0f / tanY;
    return true;
}

struct OdstHalo3LookAngles
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

// The headset-confirmed Halo 3 camera owns pitch and roll absolutely. Only yaw
// is relative to the recentered game heading; no stock pitch/roll is an input.
inline bool ComputeOdstHalo3LookAngles(
    float gameYawReference, float headYawReference, float headYaw,
    float headPitch, float headRoll, float yawSign, float pitchSign,
    float pitchTrim, OdstHalo3LookAngles& out)
{
    const float values[] = {
        gameYawReference, headYawReference, headYaw, headPitch, headRoll,
        yawSign, pitchSign, pitchTrim};
    for (float value : values)
        if (!std::isfinite(value))
            return false;
    float yawDelta = headYaw - headYawReference;
    while (yawDelta > 3.14159265f)
        yawDelta -= 6.2831853f;
    while (yawDelta < -3.14159265f)
        yawDelta += 6.2831853f;
    out.yaw = gameYawReference + yawSign * yawDelta;
    const float pitch = pitchSign * headPitch + pitchTrim;
    out.pitch = pitch < -1.5f ? -1.5f : (pitch > 1.5f ? 1.5f : pitch);
    out.roll = headRoll;
    return true;
}

enum class OdstHeartbeatAction
{
    None,
    LevelUnloaded,
    NoFirstHeartbeat,
};

enum class OdstStereoFrameAction
{
    RenderStockWithoutCapture,
    RenderStereoAndValidate,
};

inline OdstStereoFrameAction EvaluateOdstStereoFrame(bool runtimeShouldRender)
{
    return runtimeShouldRender
        ? OdstStereoFrameAction::RenderStereoAndValidate
        : OdstStereoFrameAction::RenderStockWithoutCapture;
}

inline bool OdstCameraOnlyScopeRequired(
    bool privateBuildEnabled, bool adapterReportsOdst,
    bool runtimeStateOwned)
{
    return runtimeStateOwned ||
        (privateBuildEnabled && adapterReportsOdst);
}

inline bool OdstManualArmEligible(
    bool cameraStable, bool headTracking, bool stereoEnabled,
    bool teardownRequested)
{
    return cameraStable && headTracking && stereoEnabled &&
        !teardownRequested;
}

inline bool OdstVrOwnsLookStick(bool cameraOnlyContext, bool headTracking)
{
    return cameraOnlyContext && headTracking;
}

inline bool OdstNestedSourceIsCompatible(
    uintptr_t nestedSource, uintptr_t expectedSource)
{
    return nestedSource == 0 || nestedSource == expectedSource;
}

inline bool OdstInactiveCameraSlotsAreSafe(
    bool slot1Active, bool slot2Active, bool slot3Active)
{
    return !slot1Active && !slot2Active && !slot3Active;
}

inline OdstHeartbeatAction EvaluateOdstHeartbeat(
    uint64_t now, uint64_t installedAt, uint64_t lastCamera,
    bool sawCamera, bool cameraReady)
{
    if (!installedAt || now < installedAt)
        return OdstHeartbeatAction::None;
    if (!sawCamera)
    {
        const uint64_t installedAge = now - installedAt;
        if (installedAge > kOdstCameraSoftTimeoutMs && !cameraReady)
            return OdstHeartbeatAction::LevelUnloaded;
        if (installedAge > kOdstCameraHardTimeoutMs)
            return OdstHeartbeatAction::NoFirstHeartbeat;
        return OdstHeartbeatAction::None;
    }
    if (!lastCamera || now < lastCamera)
        return OdstHeartbeatAction::None;
    const uint64_t cameraAge = now - lastCamera;
    if (cameraAge > kOdstCameraSoftTimeoutMs &&
        (!cameraReady || cameraAge > kOdstCameraHardTimeoutMs))
        return OdstHeartbeatAction::LevelUnloaded;
    return OdstHeartbeatAction::None;
}

class OdstFreshCameraDebounce
{
public:
    bool Update(uint64_t now, bool cameraFresh)
    {
        if (!cameraFresh)
        {
            m_freshSince = 0;
            return false;
        }
        if (!m_freshSince)
            m_freshSince = now;
        return now >= m_freshSince &&
            now - m_freshSince > kOdstCameraStableMs;
    }

    void Reset() { m_freshSince = 0; }

private:
    uint64_t m_freshSince = 0;
};

// A level-unload fallback must not accept the same stale camera bytes as a
// fresh level. Rearm only after observing the camera array inactive and then
// active again, or after the title DLL has genuinely left the process.
class OdstCameraRearmGate
{
public:
    void BlockUntilReload(bool cameraActiveNow)
    {
        m_blocked = true;
        m_sawInactive = !cameraActiveNow;
        m_requireTitleExit = false;
    }

    void BlockUntilTitleExit()
    {
        m_blocked = true;
        m_sawInactive = false;
        m_requireTitleExit = true;
    }

    void Observe(bool titleActive, bool cameraActive)
    {
        if (!titleActive)
        {
            m_blocked = false;
            m_sawInactive = false;
            m_requireTitleExit = false;
            return;
        }
        if (!m_blocked)
            return;
        if (m_requireTitleExit)
            return;
        if (!cameraActive)
            m_sawInactive = true;
        else if (m_sawInactive)
        {
            m_blocked = false;
            m_sawInactive = false;
        }
    }

    bool CanAttemptInstall() const { return !m_blocked; }
    bool IsBlocked() const { return m_blocked; }

private:
    bool m_blocked = false;
    bool m_sawInactive = false;
    bool m_requireTitleExit = false;
};
