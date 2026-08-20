#include "halo2_adapter.h"

#include "../common/halo2_render_logic.h"

#ifndef HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION
#define HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION 0
#endif
#ifndef HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
#define HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO 0
#endif

namespace
{
    constexpr Halo2EvidenceIdentity kHalo2RetailEvidence = {
        L"halo2.dll",
        kHalo2RetailModuleSha256[0],
        kHalo2RetailModuleSha256[1],
        kHalo2RetailPeTimestamp,
        static_cast<uint32_t>(kHalo2RetailImageSize),
        kHalo2KitBuildTag,
        kHalo2KitTagTestSha256,
    };
}

Halo2AdapterStage Halo2Adapter_GetStage()
{
#if HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
    return Halo2AdapterStage::TemporalStereoPositionOnly;
#elif HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION
    return Halo2AdapterStage::ColdObservationOnly;
#else
    return Halo2AdapterStage::Disabled;
#endif
}

const Halo2EvidenceIdentity& Halo2Adapter_GetEvidenceIdentity()
{
    return kHalo2RetailEvidence;
}

bool Halo2Adapter_RuntimeHooksPermitted()
{
#if HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
    return true;
#else
    return false;
#endif
}

bool Halo2Adapter_EngineWritesPermitted()
{
#if HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
    // This is deliberately narrower than the historical name: C-H2-2 permits
    // only two temporary 12-byte camera-position writes inside the proven
    // player-window transaction. Generic draw distance remains hard-denied by
    // TitleRegistry_AllowsGenericDrawDistance.
    return true;
#else
    return false;
#endif
}
