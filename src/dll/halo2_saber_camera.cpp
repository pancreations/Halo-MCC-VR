#include "halo2_saber_camera.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace
{
    bool ReadPointerGuarded(uintptr_t address, uintptr_t& value) noexcept
    {
        if (!address)
            return false;
        __try
        {
            value = *reinterpret_cast<const volatile uintptr_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadInt32Guarded(uintptr_t address, int32_t& value) noexcept
    {
        if (!address)
            return false;
        __try
        {
            value = *reinterpret_cast<const volatile int32_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadFloatsGuarded(
        uintptr_t address, float* out, size_t count) noexcept
    {
        if (!address || !out)
            return false;
        __try
        {
            for (size_t index = 0; index < count; ++index)
            {
                out[index] = *reinterpret_cast<const volatile float*>(
                    address + index * sizeof(float));
                if (!std::isfinite(out[index]))
                    return false;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool WriteFloatsGuarded(
        uintptr_t address, const float* values, size_t count) noexcept
    {
        if (!address || !values)
            return false;
        for (size_t index = 0; index < count; ++index)
            if (!std::isfinite(values[index]))
                return false;
        __try
        {
            for (size_t index = 0; index < count; ++index)
            {
                *reinterpret_cast<volatile float*>(
                    address + index * sizeof(float)) = values[index];
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadByteGuarded(uintptr_t address, uint8_t& value) noexcept
    {
        if (!address)
            return false;
        __try
        {
            value = *reinterpret_cast<const volatile uint8_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool WriteByteGuarded(uintptr_t address, uint8_t value) noexcept
    {
        if (!address)
            return false;
        __try
        {
            *reinterpret_cast<volatile uint8_t*>(address) = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

bool Halo2SaberCamera_Resolve(
    uintptr_t moduleBase, uint32_t user,
    Halo2SaberCameraBinding& out) noexcept
{
    if (!moduleBase)
        return false;

    uintptr_t scene = 0;
    if (!ReadPointerGuarded(moduleBase + kHalo2SaberSceneSlotRva, scene) ||
        !scene)
    {
        return false;
    }

    int32_t count = 0;
    if (!ReadInt32Guarded(
            scene + kHalo2SaberSceneCameraCountOffset, count) ||
        count <= 0 || count > 8 ||
        user >= static_cast<uint32_t>(count))
    {
        return false;
    }

    uintptr_t list = 0;
    if (!ReadPointerGuarded(
            scene + kHalo2SaberSceneCameraListOffset, list) ||
        !list)
    {
        return false;
    }

    uintptr_t camera = 0;
    if (!ReadPointerGuarded(
            list + static_cast<uintptr_t>(user) * sizeof(uintptr_t),
            camera) ||
        !camera)
    {
        return false;
    }

    Halo2SaberCameraBinding candidate{};
    candidate.camera = camera;
    candidate.user = user;
    candidate.cameraCount = static_cast<uint32_t>(count);
    out = candidate;
    return true;
}

bool Halo2SaberCamera_ReadConstants(
    uintptr_t moduleBase, Halo2SaberCameraConstants& out) noexcept
{
    if (!moduleBase)
        return false;
    Halo2SaberCameraConstants candidate{};
    if (!ReadFloatsGuarded(
            moduleBase + kHalo2SaberOffsetXRva, &candidate.offsetX, 1) ||
        !ReadFloatsGuarded(
            moduleBase + kHalo2SaberOffsetZRva, &candidate.offsetZ, 1) ||
        !ReadFloatsGuarded(
            moduleBase + kHalo2SaberOffsetYRva, &candidate.offsetY, 1) ||
        !ReadFloatsGuarded(
            moduleBase + kHalo2SaberForwardScaleRva,
            &candidate.forwardScale, 1))
    {
        return false;
    }
    if (!Halo2SaberCameraConstantsFinite(candidate))
        return false;
    out = candidate;
    return true;
}

bool Halo2SaberCamera_Save(
    const Halo2SaberCameraBinding& binding,
    Halo2SaberCameraState& out) noexcept
{
    if (!binding.camera)
        return false;
    Halo2SaberCameraState candidate{};
    if (!ReadFloatsGuarded(
            binding.camera + kHalo2SaberCameraMatrixOffset,
            candidate.matrix, 16) ||
        !ReadFloatsGuarded(
            binding.camera + kHalo2SaberCameraVerticalFovDegreesOffset,
            &candidate.verticalFovDegrees, 1) ||
        !ReadFloatsGuarded(
            binding.camera + kHalo2SaberCameraHorizontalFovDegreesOffset,
            &candidate.horizontalFovDegrees, 1) ||
        !ReadFloatsGuarded(
            binding.camera + kHalo2SaberCameraAspectOffset,
            &candidate.aspect, 1) ||
        !ReadByteGuarded(
            binding.camera + kHalo2SaberCameraFovChangedOffset,
            candidate.fovChanged))
    {
        return false;
    }
    candidate.valid = true;
    out = candidate;
    return true;
}

bool Halo2SaberCamera_Restore(
    const Halo2SaberCameraBinding& binding,
    const Halo2SaberCameraState& saved) noexcept
{
    if (!binding.camera || !saved.valid)
        return false;
    // Restore in the reverse order of the write so a torn restore still leaves
    // the matrix consistent with the field of view the engine last derived.
    const bool ok =
        WriteFloatsGuarded(
            binding.camera + kHalo2SaberCameraAspectOffset,
            &saved.aspect, 1) &&
        WriteFloatsGuarded(
            binding.camera + kHalo2SaberCameraHorizontalFovDegreesOffset,
            &saved.horizontalFovDegrees, 1) &&
        WriteFloatsGuarded(
            binding.camera + kHalo2SaberCameraVerticalFovDegreesOffset,
            &saved.verticalFovDegrees, 1) &&
        WriteFloatsGuarded(
            binding.camera + kHalo2SaberCameraMatrixOffset,
            saved.matrix, 16);
    return ok &&
        WriteByteGuarded(
            binding.camera + kHalo2SaberCameraFovChangedOffset,
            saved.fovChanged);
}

bool Halo2SaberCamera_WriteEye(
    const Halo2SaberCameraBinding& binding,
    const Halo2CameraBasis& basis,
    const Halo2SaberCameraConstants& constants,
    float horizontalFovDegrees) noexcept
{
    if (!binding.camera)
        return false;

    Halo2SaberViewMatrix matrix{};
    if (!Halo2BuildSaberViewMatrix(basis, constants, matrix))
        return false;

    if (!WriteFloatsGuarded(
            binding.camera + kHalo2SaberCameraMatrixOffset, matrix.m, 16))
    {
        return false;
    }

    if (horizontalFovDegrees <= 0.0f)
        return true;

    // The engine derives the vertical field of view from the horizontal one and
    // the aspect ratio, so only a symmetric frustum is expressible through this
    // object. Reproduce that derivation rather than writing an inconsistent
    // pair, and mark the field of view changed exactly as 0xBC560 does.
    float aspect = 0.0f;
    if (!ReadFloatsGuarded(
            binding.camera + kHalo2SaberCameraAspectOffset, &aspect, 1) ||
        aspect <= 0.0f)
    {
        return false;
    }
    float verticalDegrees = 0.0f;
    if (!Halo2SaberVerticalFovDegrees(
            horizontalFovDegrees, aspect, verticalDegrees))
    {
        return false;
    }
    return WriteFloatsGuarded(
               binding.camera +
                   kHalo2SaberCameraHorizontalFovDegreesOffset,
               &horizontalFovDegrees, 1) &&
        WriteFloatsGuarded(
               binding.camera + kHalo2SaberCameraVerticalFovDegreesOffset,
               &verticalDegrees, 1) &&
        WriteByteGuarded(
               binding.camera + kHalo2SaberCameraFovChangedOffset, 1);
}
