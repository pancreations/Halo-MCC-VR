#include "scope_logic.h"

namespace
{
    void RotateVector(const float q[4], const float v[3], float out[3])
    {
        // q * v * conjugate(q), expanded for a unit quaternion.
        const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
        const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
        const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
        out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
        out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
        out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
    }
}

ScopeToggleUpdate ScopeToggleDetector::Update(bool enabled, bool rightClick,
                                               bool cancelGesture)
{
    if (!enabled)
    {
        const bool changed = m_active;
        m_active = false;
        m_rightDown = rightClick;
        m_cancelled = rightClick;
        return {changed, false};
    }

    if (rightClick && !m_rightDown)
    {
        m_rightDown = true;
        m_cancelled = cancelGesture;
    }
    else if (rightClick && cancelGesture)
    {
        m_cancelled = true;
    }

    if (!rightClick && m_rightDown)
    {
        const bool toggle = !m_cancelled && !cancelGesture;
        m_rightDown = false;
        m_cancelled = false;
        if (toggle)
        {
            m_active = !m_active;
            return {true, m_active};
        }
    }
    return {false, m_active};
}

void ScopeToggleDetector::Reset()
{
    m_active = false;
    m_rightDown = false;
    m_cancelled = false;
}

bool ScopeRefreshScheduler::Advance(bool active, int divisor)
{
    if (!active)
    {
        m_frame = 0;
        return false;
    }
    if (divisor < 1) divisor = 1;
    if (divisor > 4) divisor = 4;
    return (++m_frame % static_cast<unsigned>(divisor)) == 0;
}

ScopeQuadTransform ComputeScopeQuadTransform(const float orientation[4],
                                             const float origin[3],
                                             float rightMeters,
                                             float upMeters,
                                             float forwardMeters,
                                             float widthMeters)
{
    const float localOffset[3] = {rightMeters, upMeters, -forwardMeters};
    float worldOffset[3]{};
    RotateVector(orientation, localOffset, worldOffset);

    ScopeQuadTransform result{};
    result.position[0] = origin[0] + worldOffset[0];
    result.position[1] = origin[1] + worldOffset[1];
    result.position[2] = origin[2] + worldOffset[2];
    result.width = widthMeters;
    result.height = widthMeters * 0.75f;
    return result;
}
