#include "halo2_adapter.h"

#include "../common/halo2_render_logic.h"

#ifndef HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION
#define HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION 0
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
#if HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION
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
    return false;
}

bool Halo2Adapter_EngineWritesPermitted()
{
    // No H2 debug-variable binding or any other writable engine field has been
    // admitted. This remains false even when the cold observer is compiled out;
    // the generic draw-distance path must never scan/write an unproven title.
    return false;
}
