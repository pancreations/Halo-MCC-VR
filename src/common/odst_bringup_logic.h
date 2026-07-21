#pragma once

#include <cstdint>

constexpr uint64_t kOdstCameraFreshMs = 500;
constexpr uint64_t kOdstCameraSoftTimeoutMs = 750;
constexpr uint64_t kOdstCameraHardTimeoutMs = 5000;
constexpr uint64_t kOdstCameraStableMs = 1000;

enum class OdstHeartbeatAction
{
    None,
    LevelUnloaded,
    NoFirstHeartbeat,
};

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
    }

    void Observe(bool titleActive, bool cameraActive)
    {
        if (!titleActive)
        {
            m_blocked = false;
            m_sawInactive = false;
            return;
        }
        if (!m_blocked)
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
};
