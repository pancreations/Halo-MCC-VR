#pragma once

#include <cstddef>
#include <string_view>

#include "runtime_types.h"

struct TitleDescriptor
{
    GameTitle title;
    const wchar_t* moduleName;
    const char* displayName;
    bool runtimeSupported;
    uint32_t capabilities;
    // Static transport admission; never published as a runtime capability.
    uint32_t admissionCapabilities;
};

enum class TitleHookPlan : uint8_t
{
    None,
    Halo3Full,
    OdstExperimentalCameraCore,
    ReachCameraCore,
    Halo4CameraCore,
    Halo2TemporalStereo,
    Halo2StereoCore,
};

constexpr uint64_t kHalo3AmbiguousCameraOwnershipMs = 100;

const TitleDescriptor* TitleRegistry_All(size_t& count);
const TitleDescriptor* TitleRegistry_Find(GameTitle title);
const TitleDescriptor* TitleRegistry_FromModuleName(std::wstring_view moduleName);
TitleHookPlan TitleRegistry_HookPlan(GameTitle title);
bool TitleRegistry_AllowsSharedGameplayFeatures(
    GameTitle activeTitle, bool halo3CameraOwned, bool cameraOnlyOwned);
bool TitleRegistry_AllowsSharedControllerInput(
    GameTitle activeTitle, bool resolvedOwnerAllowsControllerInput,
    bool cameraOnlyOwned,
    bool allowAmbiguousFrontend, bool explicitTitleAllowsControllerInput);
bool TitleRegistry_AllowsGenericDrawDistance(
    GameTitle activeTitle, bool activeLevelRunning);
bool TitleRegistry_Halo3CameraOwnsAmbiguousState(
    uint64_t now, uint64_t lastCamera, uint64_t titleTransition);
const char* RuntimeModeName(RuntimeMode mode);
