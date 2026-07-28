#include "pose_inertia_filter.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kLongFrameGapSeconds = 0.25f;
    constexpr float kMinQuaternionLength = 1.0e-6f;
    constexpr float kMaxLinearVelocity = 10.0f;
    constexpr float kMaxAngularVelocity = 50.0f;
    constexpr float kMaxPositionCatchupFollow = 80.0f;
    constexpr float kMaxRotationCatchupFollow = 90.0f;
    constexpr float kCatchupStartRatio = 0.20f;
    constexpr float kPositionCatchupDistanceM = 0.15f;
    constexpr float kRotationCatchupAngleRad = 20.0f * (kPi / 180.0f);

    bool Finite(float value) noexcept
    {
        return std::isfinite(value);
    }

    bool NormalizeQuaternion(float q[4]) noexcept
    {
        const float lengthSquared =
            q[0] * q[0] + q[1] * q[1] +
            q[2] * q[2] + q[3] * q[3];
        if (!Finite(lengthSquared) ||
            lengthSquared < kMinQuaternionLength * kMinQuaternionLength)
        {
            return false;
        }
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        for (int component = 0; component < 4; ++component)
            q[component] *= inverseLength;
        return Finite(q[0]) && Finite(q[1]) &&
               Finite(q[2]) && Finite(q[3]);
    }

    bool NormalizePose(const PoseInertiaPose& input,
                       PoseInertiaPose& output) noexcept
    {
        output = input;
        if (!Finite(output.position[0]) ||
            !Finite(output.position[1]) ||
            !Finite(output.position[2]))
        {
            return false;
        }
        return NormalizeQuaternion(output.orientation);
    }

    void MultiplyQuaternion(const float a[4], const float b[4],
                            float output[4]) noexcept
    {
        output[0] = a[3] * b[0] + a[0] * b[3] +
                    a[1] * b[2] - a[2] * b[1];
        output[1] = a[3] * b[1] - a[0] * b[2] +
                    a[1] * b[3] + a[2] * b[0];
        output[2] = a[3] * b[2] + a[0] * b[1] -
                    a[1] * b[0] + a[2] * b[3];
        output[3] = a[3] * b[3] - a[0] * b[0] -
                    a[1] * b[1] - a[2] * b[2];
    }

    void QuaternionErrorVector(const float target[4], const float current[4],
                               float output[3]) noexcept
    {
        const float inverseTarget[4] = {
            -target[0], -target[1], -target[2], target[3]};
        float error[4]{};
        MultiplyQuaternion(inverseTarget, current, error);
        if (error[3] < 0.0f)
        {
            for (float& component : error)
                component = -component;
        }
        NormalizeQuaternion(error);
        const float vectorLength = std::sqrt(
            error[0] * error[0] + error[1] * error[1] +
            error[2] * error[2]);
        if (vectorLength < 1.0e-6f)
        {
            output[0] = 2.0f * error[0];
            output[1] = 2.0f * error[1];
            output[2] = 2.0f * error[2];
            return;
        }
        const float angle = 2.0f * std::atan2(
            vectorLength, std::clamp(error[3], 0.0f, 1.0f));
        const float scale = angle / vectorLength;
        output[0] = error[0] * scale;
        output[1] = error[1] * scale;
        output[2] = error[2] * scale;
    }

    void QuaternionFromErrorVector(const float error[3],
                                   float output[4]) noexcept
    {
        const float angle = std::sqrt(
            error[0] * error[0] + error[1] * error[1] +
            error[2] * error[2]);
        if (angle < 1.0e-6f)
        {
            output[0] = error[0] * 0.5f;
            output[1] = error[1] * 0.5f;
            output[2] = error[2] * 0.5f;
            output[3] = 1.0f;
            NormalizeQuaternion(output);
            return;
        }
        const float halfAngle = angle * 0.5f;
        const float scale = std::sin(halfAngle) / angle;
        output[0] = error[0] * scale;
        output[1] = error[1] * scale;
        output[2] = error[2] * scale;
        output[3] = std::cos(halfAngle);
    }

    void CriticallyDampedStep(float target, float omega, float deltaSeconds,
                              float& value, float& velocity) noexcept
    {
        const float displacement = value - target;
        const float combined = velocity + omega * displacement;
        const float decay = std::exp(-omega * deltaSeconds);
        value = target + (displacement + combined * deltaSeconds) * decay;
        velocity = (velocity - omega * combined * deltaSeconds) * decay;
    }

    bool ClampVectorLength(float vector[3], float maximum) noexcept
    {
        const float lengthSquared =
            vector[0] * vector[0] + vector[1] * vector[1] +
            vector[2] * vector[2];
        if (!Finite(lengthSquared))
            return false;
        const float maximumSquared = maximum * maximum;
        if (lengthSquared <= maximumSquared)
            return false;
        if (maximum <= 0.0f || lengthSquared < 1.0e-12f)
        {
            vector[0] = vector[1] = vector[2] = 0.0f;
            return true;
        }
        const float scale = maximum / std::sqrt(lengthSquared);
        vector[0] *= scale;
        vector[1] *= scale;
        vector[2] *= scale;
        return true;
    }

    float VectorLength(const float vector[3]) noexcept
    {
        const float lengthSquared =
            vector[0] * vector[0] + vector[1] * vector[1] +
            vector[2] * vector[2];
        return Finite(lengthSquared) && lengthSquared > 0.0f
            ? std::sqrt(lengthSquared)
            : 0.0f;
    }

    float CatchupFollow(float baseFollow, float maximumFollow, float lag,
                        float lagEnvelope, float catchupSpeed) noexcept
    {
        if (!Finite(lag) || !Finite(lagEnvelope) || lagEnvelope <= 0.0f)
            return baseFollow;

        const float ratio = std::clamp(lag / lagEnvelope, 0.0f, 1.0f);
        const float normalized = std::clamp(
            (ratio - kCatchupStartRatio) / (1.0f - kCatchupStartRatio),
            0.0f, 1.0f);
        const float smooth = normalized * normalized *
            (3.0f - 2.0f * normalized);
        const float strength = std::clamp(catchupSpeed, 0.0f, 1.0f);
        return baseFollow +
            (maximumFollow - baseFollow) * strength * smooth;
    }

    // Retained inert from the rejected hard-boundary candidate. Normal pose
    // filtering no longer projects position or orientation onto a leash.
    [[maybe_unused]] void RemoveOutwardVelocity(
        const float boundaryError[3], float velocity[3]) noexcept
    {
        const float lengthSquared =
            boundaryError[0] * boundaryError[0] +
            boundaryError[1] * boundaryError[1] +
            boundaryError[2] * boundaryError[2];
        if (!Finite(lengthSquared) || lengthSquared < 1.0e-12f)
            return;

        const float outward =
            velocity[0] * boundaryError[0] +
            velocity[1] * boundaryError[1] +
            velocity[2] * boundaryError[2];
        if (!Finite(outward) || outward <= 0.0f)
            return;

        const float scale = outward / lengthSquared;
        for (int axis = 0; axis < 3; ++axis)
            velocity[axis] -= boundaryError[axis] * scale;
    }

    bool FiniteState(const PoseInertiaPose& pose,
                     const float linearVelocity[3],
                     const float angularVelocity[3]) noexcept
    {
        for (float component : pose.position)
            if (!Finite(component))
                return false;
        for (float component : pose.orientation)
            if (!Finite(component))
                return false;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!Finite(linearVelocity[axis]) ||
                !Finite(angularVelocity[axis]))
            {
                return false;
            }
        }
        return true;
    }
}

void PoseInertiaFilter::Reset() noexcept
{
    initialized_ = false;
    pose_ = {};
    pose_.orientation[3] = 1.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        linearVelocity_[axis] = 0.0f;
        angularVelocity_[axis] = 0.0f;
    }
}

bool PoseInertiaFilter::Update(
    bool enabled, bool trackingValid, const PoseInertiaPose& rawPose,
    float deltaSeconds, const PoseInertiaSettings& settings,
    PoseInertiaPose& output) noexcept
{
    PoseInertiaPose target{};
    if (!trackingValid || !NormalizePose(rawPose, target))
    {
        Reset();
        return false;
    }

    if (!enabled)
    {
        Reset();
        output = rawPose;
        return true;
    }

    if (!initialized_ || !Finite(deltaSeconds) || deltaSeconds <= 0.0f ||
        deltaSeconds > kLongFrameGapSeconds)
    {
        Reset();
        initialized_ = true;
        pose_ = target;
        output = pose_;
        return true;
    }

    const float basePositionFollow = std::clamp(
        settings.positionFollow, 2.0f, 40.0f);
    const float baseRotationFollow = std::clamp(
        settings.rotationFollow, 2.0f, 45.0f);

    float positionError[3] = {
        pose_.position[0] - target.position[0],
        pose_.position[1] - target.position[1],
        pose_.position[2] - target.position[2]};
    const float positionFollow = CatchupFollow(
        basePositionFollow, kMaxPositionCatchupFollow,
        VectorLength(positionError), kPositionCatchupDistanceM,
        settings.catchupSpeed);
    for (int axis = 0; axis < 3; ++axis)
    {
        CriticallyDampedStep(
            target.position[axis], positionFollow, deltaSeconds,
            pose_.position[axis], linearVelocity_[axis]);
    }

    float rotationError[3]{};
    QuaternionErrorVector(
        target.orientation, pose_.orientation, rotationError);
    const float rotationFollow = CatchupFollow(
        baseRotationFollow, kMaxRotationCatchupFollow,
        VectorLength(rotationError), kRotationCatchupAngleRad,
        settings.catchupSpeed);
    for (int axis = 0; axis < 3; ++axis)
    {
        CriticallyDampedStep(
            0.0f, rotationFollow, deltaSeconds,
            rotationError[axis], angularVelocity_[axis]);
    }
    float errorQuaternion[4]{};
    QuaternionFromErrorVector(rotationError, errorQuaternion);
    MultiplyQuaternion(
        target.orientation, errorQuaternion, pose_.orientation);
    if (!NormalizeQuaternion(pose_.orientation))
    {
        Reset();
        initialized_ = true;
        pose_ = target;
        output = pose_;
        return true;
    }

    ClampVectorLength(linearVelocity_, kMaxLinearVelocity);
    ClampVectorLength(angularVelocity_, kMaxAngularVelocity);
    if (!FiniteState(pose_, linearVelocity_, angularVelocity_))
    {
        Reset();
        initialized_ = true;
        pose_ = target;
    }
    output = pose_;
    return true;
}
