#include "input_logic.h"

#include <cmath>

void MenuChordDetector::Reset()
{
    m_firstDownMs = 0;
    m_firstSide = 0;
    m_latched = false;
    m_expired = false;
}

MenuChordResult MenuChordDetector::Update(uint64_t nowMs, bool leftClick, bool rightClick)
{
    if (m_latched)
    {
        if (!leftClick && !rightClick)
            Reset();
        else
            return { false, true };
        return {};
    }

    if (!leftClick && !rightClick)
    {
        Reset();
        return {};
    }

    if (m_firstSide == 0)
    {
        m_firstDownMs = nowMs;
        m_firstSide = leftClick && rightClick ? 3 : (leftClick ? 1 : 2);
    }

    if (!m_expired && nowMs - m_firstDownMs > 250)
        m_expired = true;
    if (!m_expired && leftClick && rightClick)
    {
        m_latched = true;
        return { true, true };
    }
    return {};
}

MenuPointerHit IntersectMenuQuad(const float origin[3], const float direction[3],
    float distance, float width, float height, float centerY)
{
    MenuPointerHit result{};
    if (!origin || !direction || distance <= 0.0f || width <= 0.0f || height <= 0.0f)
        return result;
    if (std::fabs(direction[2]) < 1e-5f)
        return result;

    const float t = (-distance - origin[2]) / direction[2];
    if (t <= 0.0f)
        return result;
    const float x = origin[0] + direction[0] * t;
    const float y = origin[1] + direction[1] * t;
    const float halfWidth = width * 0.5f;
    const float halfHeight = height * 0.5f;
    if (x < -halfWidth || x > halfWidth ||
        y < centerY - halfHeight || y > centerY + halfHeight)
        return result;

    result.hit = true;
    result.u = x / width + 0.5f;
    result.v = 0.5f - (y - centerY) / height;
    return result;
}

float BlendXInputMotors(uint16_t lowFrequencyMotor, uint16_t highFrequencyMotor)
{
    constexpr float inverseMax = 1.0f / 65535.0f;
    const float low = static_cast<float>(lowFrequencyMotor) * inverseMax;
    const float high = static_cast<float>(highFrequencyMotor) * inverseMax;
    const float blended = low * 0.65f + high * 0.35f;
    return blended > 1.0f ? 1.0f : blended;
}

void PauseLevelRecovery::Reset()
{
    m_sawLoading = false;
}

bool PauseLevelRecovery::Update(bool pausePresentation, bool cameraStale,
                                bool levelStable)
{
    if (!pausePresentation)
    {
        Reset();
        return false;
    }
    if (cameraStale)
        m_sawLoading = true;
    if (m_sawLoading && levelStable)
    {
        Reset();
        return true;
    }
    return false;
}
