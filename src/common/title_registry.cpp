#include "title_registry.h"

#include <cwctype>

#ifndef HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
#define HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP 0
#endif
#ifndef HALOMCCVR_EXPERIMENTAL_REACH_BRINGUP
#define HALOMCCVR_EXPERIMENTAL_REACH_BRINGUP 0
#endif
#ifndef HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
#define HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO 0
#endif
#ifndef HALOMCCVR_HALO2_STEREO6DOF
#define HALOMCCVR_HALO2_STEREO6DOF 0
#endif

static_assert(HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP == 0 ||
              HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP == 1);
static_assert(HALOMCCVR_EXPERIMENTAL_REACH_BRINGUP == 0 ||
              HALOMCCVR_EXPERIMENTAL_REACH_BRINGUP == 1);

namespace
{
    constexpr uint32_t kHalo3Capabilities =
        TitleCapability_Stereo |
        TitleCapability_ControllerAim |
        TitleCapability_Hud |
        TitleCapability_ArmIk |
        TitleCapability_RuntimeModes |
        TitleCapability_RoomScale |
        TitleCapability_ControllerInput |
        TitleCapability_Haptics |
        TitleCapability_CutsceneTheater;
    constexpr uint32_t kHalo3AdmissionCapabilities =
        TitleCapability_ControllerInput;
    // Reach camera + motion core: stereo, tracked look/turn, locomotion input,
    // room-scale translation, controller aim, articulated first-person arms,
    // and haptics now have title-specific runtime/evidence paths. Native HUD
    // remains withheld until Reach's final CHUD anchor is proven and wired.
    constexpr uint32_t kReachCapabilities =
        TitleCapability_Stereo |
        TitleCapability_ControllerAim |
        TitleCapability_ArmIk |
        TitleCapability_Hud |
        TitleCapability_RuntimeModes |
        TitleCapability_RoomScale |
        TitleCapability_ControllerInput |
        TitleCapability_Haptics |
        TitleCapability_CutsceneTheater;
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    constexpr uint32_t kOdstAdmissionCapabilities =
        TitleCapability_ControllerInput;
#else
    constexpr uint32_t kOdstAdmissionCapabilities = TitleCapability_None;
#endif
#if HALOMCCVR_EXPERIMENTAL_REACH_BRINGUP
    constexpr uint32_t kReachAdmissionCapabilities =
        TitleCapability_ControllerInput;
#else
    constexpr uint32_t kReachAdmissionCapabilities = TitleCapability_None;
#endif
    // Halo 4 camera + motion core. C-H4-7/8/9 earned stereo, head tracking,
    // 6DOF and headset-owned pitch in the headset; C-H4-10 adds the closed-loop
    // controller aim, VR turn and haptics the other titles already have, on the
    // same shared code paths. C-H4-44's HUD-basis writer is headset-rejected
    // and dormant. C-H4-34 replaces the
    // rejected arm solve with rigid floating hands, so ArmIk is deliberately
    // absent. CutsceneTheater remains withheld: it has no Halo 4 evidence.
    constexpr uint32_t kHalo4Capabilities =
        TitleCapability_Stereo |
        TitleCapability_ControllerAim |
        TitleCapability_RuntimeModes |
        TitleCapability_RoomScale |
        TitleCapability_ControllerInput |
        TitleCapability_Haptics;
    constexpr uint32_t kHalo4AdmissionCapabilities =
        TitleCapability_ControllerInput;
#if HALOMCCVR_HALO2_STEREO6DOF
    // C-H2-6 owns same-frame binocular geometry and headset room-scale only;
    // unclaimed frames retain stock screen presentation, while a partially
    // claimed failed stereo transaction drops only that frame.
    // HUD and cutscene behavior remain denied.
    // C-H2-22: the virtual gamepad (VR controllers merged over any physical
    // pad) and rumble are granted. Without ControllerInput the XInput hook
    // passed the physical pad through untouched inside every Halo 2 level,
    // so the VR controllers only worked in the shell.
    // C-H2-50 grants ControllerAim for the floating hands + gun mesh and the
    // bullet direction that follows them. It is NOT permission to steer the
    // camera: `Game_ComputeAimStick` refuses Halo 2 before any capability is
    // read, so the physical right stick keeps ordinary character/camera
    // turning. The second gate is the final-palette build switch plus its live
    // hook state; the rejected interpolator-space switch remains false.
    constexpr uint32_t kHalo2Capabilities =
        TitleCapability_Stereo | TitleCapability_RoomScale |
        TitleCapability_RuntimeModes | TitleCapability_ControllerInput |
        TitleCapability_Haptics | TitleCapability_ControllerAim;
    constexpr uint32_t kHalo2AdmissionCapabilities =
        TitleCapability_ControllerInput;
#elif HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
    constexpr uint32_t kHalo2Capabilities = TitleCapability_Stereo;
    constexpr uint32_t kHalo2AdmissionCapabilities = TitleCapability_None;
#else
    constexpr uint32_t kHalo2Capabilities = TitleCapability_None;
    constexpr uint32_t kHalo2AdmissionCapabilities = TitleCapability_None;
#endif

    constexpr TitleDescriptor kTitles[] = {
        { GameTitle::Halo3, L"halo3.dll", "Halo 3", true,
          kHalo3Capabilities, kHalo3AdmissionCapabilities },
        { GameTitle::Halo3ODST, L"halo3odst.dll", "Halo 3: ODST", false,
          TitleCapability_None, kOdstAdmissionCapabilities },
        { GameTitle::HaloReach, L"haloreach.dll", "Halo: Reach", true,
          kReachCapabilities, kReachAdmissionCapabilities },
        { GameTitle::Halo4, L"halo4.dll", "Halo 4", false,
          kHalo4Capabilities, kHalo4AdmissionCapabilities },
        { GameTitle::HaloCE, L"halo1.dll", "Halo: CE Anniversary", false,
          TitleCapability_None, TitleCapability_None },
        { GameTitle::Halo2, L"halo2.dll", "Halo 2 Anniversary", false,
          kHalo2Capabilities, kHalo2AdmissionCapabilities },
    };

    bool EqualsModuleName(std::wstring_view left, std::wstring_view right)
    {
        size_t slash = left.find_last_of(L'/');
        const size_t backslash = left.find_last_of(static_cast<wchar_t>(92));
        if (backslash != std::wstring_view::npos &&
            (slash == std::wstring_view::npos || backslash > slash))
            slash = backslash;
        if (slash != std::wstring_view::npos)
            left.remove_prefix(slash + 1);
        if (left.size() != right.size())
            return false;
        for (size_t i = 0; i < left.size(); ++i)
        {
            if (std::towlower(left[i]) != std::towlower(right[i]))
                return false;
        }
        return true;
    }
}

const TitleDescriptor* TitleRegistry_All(size_t& count)
{
    count = sizeof(kTitles) / sizeof(kTitles[0]);
    return kTitles;
}

const TitleDescriptor* TitleRegistry_Find(GameTitle title)
{
    for (const auto& descriptor : kTitles)
    {
        if (descriptor.title == title)
            return &descriptor;
    }
    return nullptr;
}

const TitleDescriptor* TitleRegistry_FromModuleName(std::wstring_view moduleName)
{
    for (const auto& descriptor : kTitles)
    {
        if (EqualsModuleName(moduleName, descriptor.moduleName))
            return &descriptor;
    }
    return nullptr;
}

TitleHookPlan TitleRegistry_HookPlan(GameTitle title)
{
    switch (title)
    {
    case GameTitle::Halo3:
        return TitleHookPlan::Halo3Full;
    case GameTitle::Halo3ODST:
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
        return TitleHookPlan::OdstExperimentalCameraCore;
#else
        return TitleHookPlan::None;
#endif
    case GameTitle::HaloReach:
        return TitleHookPlan::ReachCameraCore;
    case GameTitle::Halo2:
#if HALOMCCVR_HALO2_STEREO6DOF
        return TitleHookPlan::Halo2StereoCore;
#elif HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
        return TitleHookPlan::Halo2TemporalStereo;
#else
        return TitleHookPlan::None;
#endif
    default:
        return TitleHookPlan::None;
    }
}

bool TitleRegistry_AllowsSharedGameplayFeatures(
    GameTitle activeTitle, bool halo3CameraOwned, bool cameraOnlyOwned)
{
    if (cameraOnlyOwned)
        return false;
    if (activeTitle == GameTitle::None || activeTitle == GameTitle::Halo3)
        return true;
    return activeTitle == GameTitle::Unknown && halo3CameraOwned;
}

bool TitleRegistry_AllowsSharedControllerInput(
    GameTitle activeTitle, bool resolvedOwnerAllowsControllerInput,
    bool cameraOnlyOwned,
    bool allowAmbiguousFrontend, bool explicitTitleAllowsControllerInput)
{
    if (activeTitle == GameTitle::Unknown)
        return resolvedOwnerAllowsControllerInput ||
            (allowAmbiguousFrontend && !cameraOnlyOwned);
    if (activeTitle == GameTitle::Halo3ODST)
        return explicitTitleAllowsControllerInput;
    if (cameraOnlyOwned)
        return false;
    if (activeTitle == GameTitle::None)
        return true;
    return explicitTitleAllowsControllerInput;
}

bool TitleRegistry_AllowsGenericDrawDistance(
    GameTitle activeTitle, bool activeLevelRunning)
{
    if (!activeLevelRunning)
        return false;
    // C-H2-1 is read-only and C-H2-2 owns only two scoped camera-position
    // writes. The generic draw-distance helper scans the active image and may
    // write an unrelated debug variable, so it remains unreachable in both.
    return activeTitle != GameTitle::None &&
        activeTitle != GameTitle::Unknown &&
        activeTitle != GameTitle::Halo2;
}

bool TitleRegistry_Halo3CameraOwnsAmbiguousState(
    uint64_t now, uint64_t lastCamera, uint64_t titleTransition)
{
    return titleTransition != 0 && lastCamera > titleTransition &&
        now >= lastCamera &&
        now - lastCamera < kHalo3AmbiguousCameraOwnershipMs;
}

const char* RuntimeModeName(RuntimeMode mode)
{
    switch (mode)
    {
    case RuntimeMode::Shell: return "shell";
    case RuntimeMode::Loading: return "loading";
    case RuntimeMode::Gameplay: return "gameplay";
    case RuntimeMode::Paused: return "paused";
    case RuntimeMode::Cutscene: return "cutscene";
    case RuntimeMode::Vehicle: return "vehicle";
    case RuntimeMode::Turret: return "turret";
    case RuntimeMode::Dead: return "death";
    case RuntimeMode::Unsupported: return "unsupported";
    default: return "unknown";
    }
}
