#pragma once

#include <cstdint>

enum class Halo2AdapterStage : uint8_t
{
    Disabled = 0,
    // C-H2-1: title identity, a read-only game-time liveness gate, and a
    // generation-tagged loaded-image preflight. No controller admission, hook,
    // camera write, or other engine write is permitted.
    ColdObservationOnly,
    // C-H2-2: one stock player-window render per game frame, with only the
    // render/raster camera position fields offset for alternating eyes. The
    // completed backbuffer is paired across adjacent prepared serials. No head
    // pose, controller, aim, HUD, haptics, or generic engine write is admitted.
    TemporalStereoPositionOnly,
    // C-H2-5: both eyes are rendered from one current OpenXR serial inside the
    // same game frame. Headset orientation/translation plus per-eye pose own
    // only position/forward/up in the proven render and raster cameras.
    SameFrameStereoSixDof,
};

struct Halo2EvidenceIdentity
{
    const wchar_t* moduleName;
    const char* moduleSha256Steam;
    const char* moduleSha256Store;
    uint32_t peTimestamp;
    uint32_t sizeOfImage;
    const char* h2ekBuild;
    const char* h2ekTagTestSha256;
};

Halo2AdapterStage Halo2Adapter_GetStage();
const Halo2EvidenceIdentity& Halo2Adapter_GetEvidenceIdentity();
bool Halo2Adapter_RuntimeHooksPermitted();
bool Halo2Adapter_EngineWritesPermitted();
