#pragma once

struct PoseInertiaPose
{
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float position[3]{};
};

struct PoseInertiaSettings
{
    float positionFollow = 14.0f;
    float rotationFollow = 17.0f;
    float catchupSpeed = 0.75f;
};

// Allocation-free pose spring. One instance owns one independently filtered
// pose (a tracked hand or the final weapon); callers should never share an
// instance between targets or between title owners.
class PoseInertiaFilter
{
public:
    void Reset() noexcept;

    // Returns false and resets when tracking/raw input is invalid. Disabled
    // mode returns the valid raw pose exactly and leaves no stale spring
    // state that could jump when the feature is enabled again.
    bool Update(bool enabled, bool trackingValid,
                const PoseInertiaPose& rawPose, float deltaSeconds,
                const PoseInertiaSettings& settings,
                PoseInertiaPose& output) noexcept;

private:
    bool initialized_ = false;
    PoseInertiaPose pose_{};
    float linearVelocity_[3]{};
    float angularVelocity_[3]{};
};
