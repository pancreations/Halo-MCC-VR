#pragma once

struct ScopeToggleUpdate
{
    bool changed = false;
    bool active = false;
};

// R3 toggles the universal VR scope on release. A menu chord can begin with
// either stick, so the whole R3 gesture is cancelled as soon as the chord is
// consumed. Disabled/unavailable scope input resets the latch without letting
// a held R3 toggle when gameplay resumes.
class ScopeToggleDetector
{
public:
    ScopeToggleUpdate Update(bool enabled, bool rightClick, bool cancelGesture);
    void Reset();
    bool IsActive() const { return m_active; }

private:
    bool m_active = false;
    bool m_rightDown = false;
    bool m_cancelled = false;
};

class ScopeRefreshScheduler
{
public:
    bool Advance(bool active, int divisor);
private:
    unsigned m_frame = 0;
};

constexpr int kMaximumWeaponZoomLevels = 16;

// Normalized live weapon-tag data. Runtime adapters populate this from the
// title currently loaded; the scope logic itself never knows weapon names or
// title-specific memory layouts.
struct WeaponZoomDescriptor
{
    unsigned weaponId = 0xFFFFFFFFu;
    int levelCount = 0;
    float magnifications[kMaximumWeaponZoomLevels]{};
    bool valid = false;
};

struct ScopeTierState
{
    bool active = false;
    float zoom = 1.0f;
    int tier = -1;
};

// R3 advances through the held weapon's authored stages, then closes. A valid
// zero-level weapon gets the configurable universal fallback. Missing identity
// never becomes fallback, and changing weapons closes stale scope state.
class ScopeTierController
{
public:
    void RequestAdvance();
    ScopeTierState Update(bool enabled,
                          const WeaponZoomDescriptor* weapon,
                          float fallbackZoom);
    void Reset();

private:
    unsigned m_weaponId = 0xFFFFFFFFu;
    int m_tier = -1;
    bool m_pendingAdvance = false;
};

struct ScopeQuadTransform
{
    float position[3]{};
    float width = 0.0f;
    float height = 0.0f;
};

// Applies direct controller-local right/up/forward offsets. The dimensions are
// fixed physical meters and intentionally do not depend on headset distance.
ScopeQuadTransform ComputeScopeQuadTransform(const float orientation[4],
                                             const float origin[3],
                                             float rightMeters,
                                             float upMeters,
                                             float forwardMeters,
                                             float widthMeters);

struct ScopeCameraPose
{
    float position[3]{};
    float forward[3]{};
    float up[3]{};
};

// Builds a remote camera at the bullet-aligned VR crosshair point. Its view
// direction is Halo's actual projectile direction while its roll follows the
// controller. The physical scope quad is positioned independently.
bool ComputeScopeCameraPose(const float controllerBasis[9],
                            const float weaponSeat[3],
                            const float bulletForward[3],
                            float worldScale,
                            float gunForwardMeters,
                            float crosshairDistanceMeters,
                            ScopeCameraPose& result);

struct ScopeProjectionTangents
{
    float horizontal = 0.0f;
    float vertical = 0.0f;
};

// A conventional 70-degree horizontal lens at 1x. The final image is 4:3;
// horizontal is widened only enough to compensate for the source center crop.
ScopeProjectionTangents ComputeScopeProjectionTangents(float zoom,
                                                        float sourceAspect);
