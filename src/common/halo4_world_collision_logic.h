#pragma once

#include <cmath>

// Pure, engine-independent policy for the first Halo 4 world-contact
// experiment. The runtime query is a point sweep from the last accepted wrist
// position to the latest tracked target. This helper validates and resolves a
// reported hit while leaving the engine ABI and threading outside test code.
struct Halo4WorldCollisionResolution
{
    bool valid = false;
    bool contact = false;
    float accepted[3]{};
    float correction[3]{};
};

// One root plus the six authored min/max points selected from a hand or held
// model.  The right-hand publication can combine one hand set and one weapon
// set without inventing a title-independent capsule or box.
inline constexpr int kHalo4WorldCollisionExtremaCount = 7;
inline constexpr int kHalo4WorldCollisionMaxSamples = 13;

inline bool Halo4WorldCollisionFiniteVector(const float value[3]) noexcept
{
    return value && std::isfinite(value[0]) && std::isfinite(value[1]) &&
        std::isfinite(value[2]);
}

inline float Halo4WorldCollisionDistanceSquared(
    const float first[3], const float second[3]) noexcept
{
    if (!Halo4WorldCollisionFiniteVector(first) ||
        !Halo4WorldCollisionFiniteVector(second))
        return -1.0f;
    const float x = second[0] - first[0];
    const float y = second[1] - first[1];
    const float z = second[2] - first[2];
    const float value = x * x + y * y + z * z;
    return std::isfinite(value) ? value : -1.0f;
}

inline int Halo4SelectWorldCollisionExtrema(
    const float root[3], const float* authoredPoints, int authoredPointCount,
    float output[][3], int outputCapacity) noexcept
{
    if (!Halo4WorldCollisionFiniteVector(root) || !authoredPoints ||
        authoredPointCount <= 0 || !output || outputCapacity <= 0)
        return 0;

    for (int axis = 0; axis < 3; ++axis)
        output[0][axis] = root[axis];
    int outputCount = 1;
    int extrema[6]{-1, -1, -1, -1, -1, -1};
    float values[6]{INFINITY, -INFINITY, INFINITY, -INFINITY,
                    INFINITY, -INFINITY};
    for (int point = 0; point < authoredPointCount; ++point)
    {
        const float* candidate = authoredPoints + point * 3;
        if (!Halo4WorldCollisionFiniteVector(candidate)) continue;
        for (int axis = 0; axis < 3; ++axis)
        {
            const int minimum = axis * 2;
            const int maximum = minimum + 1;
            if (candidate[axis] < values[minimum])
            {
                values[minimum] = candidate[axis];
                extrema[minimum] = point;
            }
            if (candidate[axis] > values[maximum])
            {
                values[maximum] = candidate[axis];
                extrema[maximum] = point;
            }
        }
    }

    for (int extremum : extrema)
    {
        if (extremum < 0 || outputCount >= outputCapacity) continue;
        const float* candidate = authoredPoints + extremum * 3;
        bool duplicate = false;
        for (int existing = 0; existing < outputCount; ++existing)
        {
            const float distance = Halo4WorldCollisionDistanceSquared(
                output[existing], candidate);
            if (distance >= 0.0f && distance < 1.0e-10f)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        for (int axis = 0; axis < 3; ++axis)
            output[outputCount][axis] = candidate[axis];
        ++outputCount;
    }
    return outputCount;
}

inline bool Halo4BuildWorldCollisionPushVelocity(
    const float previous[3], const float desired[3], uint64_t elapsedMs,
    float worldScale, float output[3], float strength = 0.65f,
    float maximumMetresPerSecond = 2.0f) noexcept
{
    if (!Halo4WorldCollisionFiniteVector(previous) ||
        !Halo4WorldCollisionFiniteVector(desired) || !output ||
        elapsedMs == 0 || elapsedMs > 250 || !std::isfinite(worldScale) ||
        worldScale <= 0.0f || !std::isfinite(strength) || strength <= 0.0f ||
        !std::isfinite(maximumMetresPerSecond) ||
        maximumMetresPerSecond <= 0.0f)
        return false;
    const float inverseSeconds = 1000.0f / static_cast<float>(elapsedMs);
    float lengthSquared = 0.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        output[axis] = (desired[axis] - previous[axis]) *
            inverseSeconds * strength;
        lengthSquared += output[axis] * output[axis];
    }
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-8f)
        return false;
    const float maximum = maximumMetresPerSecond * worldScale;
    if (lengthSquared > maximum * maximum)
    {
        const float scale = maximum / std::sqrt(lengthSquared);
        for (int axis = 0; axis < 3; ++axis)
            output[axis] *= scale;
    }
    return Halo4WorldCollisionFiniteVector(output);
}

inline bool Halo4WorldCollisionMovementIsTeleport(
    const float accepted[3], const float desired[3], float worldScale,
    float maximumMetres = 1.5f) noexcept
{
    if (!std::isfinite(worldScale) || worldScale <= 0.0f ||
        !std::isfinite(maximumMetres) || maximumMetres <= 0.0f)
        return true;
    const float distanceSquared =
        Halo4WorldCollisionDistanceSquared(accepted, desired);
    const float maximum = maximumMetres * worldScale;
    return distanceSquared < 0.0f || distanceSquared > maximum * maximum;
}

inline Halo4WorldCollisionResolution Halo4ResolveWorldCollision(
    const float start[3], const float desired[3], bool hit,
    float hitFraction, float worldScale, float skinMetres = 0.015f) noexcept
{
    Halo4WorldCollisionResolution result{};
    if (!Halo4WorldCollisionFiniteVector(start) ||
        !Halo4WorldCollisionFiniteVector(desired) ||
        !std::isfinite(worldScale) || worldScale <= 0.0f ||
        !std::isfinite(skinMetres) || skinMetres < 0.0f)
        return result;

    const float distanceSquared =
        Halo4WorldCollisionDistanceSquared(start, desired);
    if (distanceSquared < 0.0f)
        return result;

    result.valid = true;
    if (!hit)
    {
        for (int axis = 0; axis < 3; ++axis)
            result.accepted[axis] = desired[axis];
        return result;
    }
    if (!std::isfinite(hitFraction) || hitFraction < 0.0f ||
        hitFraction > 1.0f)
    {
        result.valid = false;
        return result;
    }

    const float distance = std::sqrt(distanceSquared);
    float resolvedFraction = hitFraction;
    if (distance > 1.0e-6f)
    {
        const float skinFraction = skinMetres * worldScale / distance;
        resolvedFraction = std::fmax(0.0f, hitFraction - skinFraction);
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        result.accepted[axis] = start[axis] +
            (desired[axis] - start[axis]) * resolvedFraction;
        result.correction[axis] = result.accepted[axis] - desired[axis];
        if (!std::isfinite(result.accepted[axis]) ||
            !std::isfinite(result.correction[axis]))
        {
            result = Halo4WorldCollisionResolution{};
            return result;
        }
    }
    result.contact = true;
    return result;
}
