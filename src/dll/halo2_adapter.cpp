#include "halo2_adapter.h"

#include "../common/halo2_render_logic.h"

#ifndef HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION
#define HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION 0
#endif
#ifndef HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
#define HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO 0
#endif
#ifndef HALOMCCVR_HALO2_STEREO6DOF
#define HALOMCCVR_HALO2_STEREO6DOF 0
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
#if HALOMCCVR_HALO2_STEREO6DOF
    return Halo2AdapterStage::SameFrameStereoSixDof;
#elif HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
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
#if HALOMCCVR_HALO2_STEREO6DOF || \
    HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
    return true;
#else
    return false;
#endif
}

bool Halo2Adapter_EngineWritesPermitted()
{
#if HALOMCCVR_HALO2_STEREO6DOF || \
    HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
    // C-H2-5 permits only scoped camera pose writes; dormant C-H2-2 permitted
    // only position. Generic draw distance remains hard-denied in either case.
    return true;
#else
    return false;
#endif
}
