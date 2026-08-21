#pragma once

#include <cstddef>
#include <cstdint>

#include "../common/halo2_render_logic.h"

// The Anniversary (Saber GroundHog) renderer's per-user camera object, and the
// scoped write/restore a per-eye pass needs.
//
// halo2.dll fills this object once per frame from the Blam observer through the
// bridge 0x5F510: it converts the observer's world-unit position/forward/up into
// a 4x4 row-major matrix in METRES with the axis remap (x, z, -y), then sets the
// field of view in degrees. Everything here replicates that conversion exactly
// rather than inventing one; see docs/HALO2-SIGNATURE-EVIDENCE.md E-H2-4.
//
// Nothing in this header allocates, logs or calls the engine. It is pure state
// plumbing so the per-eye transaction stays auditable.

struct Halo2SaberCameraBinding
{
    uintptr_t camera = 0;   // the per-user camera object
    uint32_t user = 0;
    uint32_t cameraCount = 0;
};

// Resolves the Saber camera for one user from the scene singleton. Returns
// false and leaves `out` untouched when the chain is not fully populated, which
// is the normal state before the renderer has produced its first frame.
bool Halo2SaberCamera_Resolve(
    uintptr_t moduleBase, uint32_t user,
    Halo2SaberCameraBinding& out) noexcept;

// Reads the engine-owned translation constants 0x5F510 applies. They are module
// globals rather than literals, so they are read rather than baked in.
bool Halo2SaberCamera_ReadConstants(
    uintptr_t moduleBase, Halo2SaberCameraConstants& out) noexcept;

// The exact bytes a per-eye pass owns: the 16-float matrix plus the three field
// of view fields and the changed flag. Everything else in the object stays
// engine-owned, and every owned byte is restored.
struct Halo2SaberCameraState
{
    float matrix[16]{};
    float verticalFovDegrees = 0.0f;
    float horizontalFovDegrees = 0.0f;
    float aspect = 0.0f;
    uint8_t fovChanged = 0;
    bool valid = false;
};

bool Halo2SaberCamera_Save(
    const Halo2SaberCameraBinding& binding,
    Halo2SaberCameraState& out) noexcept;

bool Halo2SaberCamera_Restore(
    const Halo2SaberCameraBinding& binding,
    const Halo2SaberCameraState& saved) noexcept;

// Writes one eye. `basis` is the eye camera in Halo 2 world units, exactly as
// the observer stores it; the metre conversion and axis remap happen here.
// `horizontalFovDegrees` of zero leaves the stock field of view alone.
bool Halo2SaberCamera_WriteEye(
    const Halo2SaberCameraBinding& binding,
    const Halo2CameraBasis& basis,
    const Halo2SaberCameraConstants& constants,
    float horizontalFovDegrees) noexcept;
