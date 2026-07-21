#pragma once

#include <cstdint>

struct MenuChordResult
{
    bool toggled = false;
    bool consumeClicks = false;
};

class MenuChordDetector
{
public:
    MenuChordResult Update(uint64_t nowMs, bool leftClick, bool rightClick);
    void Reset();

private:
    uint64_t m_firstDownMs = 0;
    uint8_t m_firstSide = 0;
    bool m_latched = false;
    bool m_expired = false;
};

struct MenuPointerHit
{
    bool hit = false;
    float u = 0.0f;
    float v = 0.0f;
};

MenuPointerHit IntersectMenuQuad(const float origin[3], const float direction[3],
    float distance, float width, float height, float centerY);

float BlendXInputMotors(uint16_t lowFrequencyMotor, uint16_t highFrequencyMotor);

// Hold-mode two-hand aiming uses the barrel zone only to acquire the support
// grip. Once acquired, ordinary movement of either hand must not drop the hold;
// releasing the grip is the explicit disengage action.
bool UpdateTwoHandHold(bool wasEngaged, bool gripHeld, bool inGrabZone);

// Tracks the loading gap caused by Restart Level while Halo's pause menu owns
// the screen. Restart bypasses the normal Start/unpause edge, so the next
// stable gameplay camera must clear the 2D pause presentation.
class PauseLevelRecovery
{
public:
    bool Update(bool pausePresentation, bool cameraStale, bool levelStable);
    void Reset();

private:
    bool m_sawLoading = false;
};
