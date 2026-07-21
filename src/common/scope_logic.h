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

// Runtime magnification for the universal scope. Opening the scope always
// restores the configured default; while it remains open, right-stick Y trims
// the lens continuously. Keeping this state outside Config avoids making a
// temporary in-game adjustment permanent.
class ScopeZoomController
{
public:
    float Update(bool active, float stickY, float deltaSeconds,
                 float defaultZoom);
    float Zoom() const { return m_zoom; }
private:
    float m_zoom = 1.25f;
    bool m_wasActive = false;
};

// Resolves one R3 stream into Halo-authored zoom behavior for scoped weapons
// and a delayed fallback toggle for weapons that do not react to native zoom.
class ScopeZoomResolver
{
public:
    void RequestToggle();
    bool Update(bool enabled, bool nativeZoomed);
    void Reset();
private:
    bool m_fallbackActive = false;
    bool m_nativeEngaged = false;
    unsigned m_pendingFrames = 0;
    unsigned m_ignoreRequestFrames = 0;
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

// Builds a bullet-aligned scope camera from Halo's collision-safe gameplay
// camera origin. Its roll follows the controller, while the physical scope quad
// remains positioned independently on the gun.
bool ComputeScopeCameraPose(const float controllerBasis[9],
                            const float cameraOrigin[3],
                            const float bulletForward[3],
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
