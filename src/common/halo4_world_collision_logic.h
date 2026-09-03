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
