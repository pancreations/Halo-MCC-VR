#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <windows.h>

#include "config.h"
#include "input_logic.h"
#include "odst_bringup_logic.h"
#include "scope_logic.h"
#include "title_registry.h"

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++g_failures;
        }
    }

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }

    size_t CountText(std::string_view text, std::string_view needle)
    {
        size_t count = 0;
        for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string_view::npos;
             pos += needle.size())
            ++count;
        return count;
    }
}

int main()
{
    {
        Check(!OdstCameraOnlyScopeRequired(false, true, false),
            "a public build never claims the private ODST camera core");
        Check(OdstCameraOnlyScopeRequired(true, true, false),
            "the adapter transition remains camera-only after module polling");
        Check(OdstCameraOnlyScopeRequired(false, false, true),
            "owned teardown state remains isolated until presentation detaches");
        Check(OdstManualArmEligible(true, true, true, false),
            "stable camera plus explicit head/stereo toggles permits manual arm");
        Check(!OdstManualArmEligible(false, true, true, false),
            "manual ODST arm still requires the fresh-camera debounce");
        Check(!OdstManualArmEligible(true, true, true, true),
            "teardown always vetoes manual ODST arm");
        OdstHalo3LookAngles look{};
        Check(ComputeOdstHalo3LookAngles(
                  1.0f, 0.25f, 0.5f, 0.2f, 0.4f, -1.0f, 1.0f, 0.1f, look),
            "ODST Halo 3 look ownership accepts finite tracked angles");
        Check(std::fabs(look.yaw - 0.75f) < 1e-5f &&
                  std::fabs(look.pitch - 0.3f) < 1e-5f &&
                  std::fabs(look.roll - 0.4f) < 1e-5f,
            "ODST look matches Halo 3 recentered-yaw and absolute pitch/roll");
        Check(ComputeOdstHalo3LookAngles(
                  0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f, 0.0f, look) &&
                  std::fabs(look.pitch - 1.5f) < 1e-5f,
            "ODST Halo 3 look retains the proven pitch safety clamp");
        Check(OdstVrOwnsLookStick(true, true),
            "tracked private ODST consumes the stock look-stick axes");
        Check(!OdstVrOwnsLookStick(true, false),
            "ODST menus retain ordinary look-stick input before head tracking");
        Check(!OdstVrOwnsLookStick(false, true),
            "the ODST input rule cannot affect Halo 3 or public title paths");
        Check(OdstMotionAimEligible(true, true, true, false),
            "owned+armed+tracked private ODST permits the narrow motion-aim gate");
        Check(!OdstMotionAimEligible(false, true, true, false),
            "ODST motion aim requires the camera-only context (never a public title)");
        Check(!OdstMotionAimEligible(true, false, true, false),
            "ODST motion aim stays closed until the camera hooks are armed");
        Check(!OdstMotionAimEligible(true, true, false, false),
            "ODST motion aim follows head tracking off");
        Check(!OdstMotionAimEligible(true, true, true, true),
            "teardown always vetoes ODST motion aim");
        Check(OdstShouldStereoRedirect(true, true, true, true),
            "a proven slot-0 camera is stereo-redirected");
        Check(!OdstShouldStereoRedirect(true, true, true, false),
            "a non-redirectable custom camera renders stock");
        Check(!OdstShouldStereoRedirect(false, true, true, true),
            "a foreign camera slot is never stereo-redirected");
        Check(!OdstShouldStereoRedirect(true, false, true, true),
            "a broken single-user tail is never stereo-redirected");
        Check(!OdstShouldStereoRedirect(true, true, false, true),
            "a mismatched nested FP source is never stereo-redirected");
        Check(OdstCamCopyRequestsTeardown(true, true, false),
            "a broken slot-0 single-user tail tears down (level unload/transition)");
        Check(!OdstCamCopyRequestsTeardown(true, true, true),
            "an active non-FP camera with a valid tail never tears down (3D recovers)");
        Check(!OdstCamCopyRequestsTeardown(false, true, false),
            "camera-copy teardown requires the core to be armed");
        Check(!OdstCamCopyRequestsTeardown(true, false, false),
            "camera-copy teardown only fires for our own primary slot");
        Check(PausePresentationInputAllowed(true),
            "proven Halo 3 gameplay may control pause presentation");
        Check(!PausePresentationInputAllowed(false),
            "MCC shell and private ODST Start edges cannot head-lock presentation");
        Check(OdstMustClearForeignPause(true, true, false) &&
                  OdstMustClearForeignPause(true, false, true),
            "private ODST entry clears either pending or active foreign pause state");
        Check(!OdstMustClearForeignPause(false, true, true),
            "foreign pause cleanup cannot affect non-ODST title ownership");
        Check(OdstNestedSourceIsCompatible(0, 0x1234),
            "ODST installation may precede the first nested FP source publish");
        Check(OdstNestedSourceIsCompatible(0x1234, 0x1234),
            "ODST accepts the proven nested FP source pointer");
        Check(!OdstNestedSourceIsCompatible(0x5678, 0x1234),
            "ODST rejects a nested FP source owned by another camera");
        Check(OdstInactiveCameraSlotsAreSafe(false, false, false),
            "constructed but inactive ODST split-screen slots are safe");
        Check(!OdstInactiveCameraSlotsAreSafe(false, true, false),
            "another active ODST camera blocks the single-user bring-up");
        Check(EvaluateOdstStereoFrame(false) ==
                  OdstStereoFrameAction::RenderStockWithoutCapture,
            "OpenXR no-render frames preserve ODST hooks without eye validation");
        Check(EvaluateOdstStereoFrame(true) ==
                  OdstStereoFrameAction::RenderStereoAndValidate,
            "active OpenXR frames still require validated stereo eye redirects");

        OdstHalo3FovMatch matchedFov{};
        Check(ComputeOdstHalo3FovMatch(
                  std::atan(1.8418f), std::atan(1.3290f), matchedFov),
            "ODST accepts the live Halo 3 headset FOV pair");
        Check(std::fabs(matchedFov.compactVerticalInput - 1.8418f) < 0.0001f &&
                  std::fabs(matchedFov.compactReferenceInput - 1.3290f) < 0.0001f,
            "ODST feeds both compact FOV inputs from Halo 3's matched pair");
        Check(std::fabs(matchedFov.projectionX - 0.54295f) < 0.0001f &&
                  std::fabs(matchedFov.projectionY - 0.75244f) < 0.0001f,
            "ODST reproduces the live Halo 3 projection scales");
        Check(!ComputeOdstHalo3FovMatch(0.0f, 0.9f, matchedFov),
            "ODST rejects an invalid headset FOV pair");

        Check(TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::None, false, false),
            "the MCC shell retains shared controller behavior");
        Check(TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Halo3, false, false),
            "an explicitly detected Halo 3 session retains shared features");
        Check(TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Unknown, true, false),
            "a fresh Halo 3 camera heartbeat resolves resident-module ambiguity");
        Check(!TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Halo3ODST, false, false),
            "public ODST XInput and presentation remain pass-through");
        Check(!TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Halo3ODST, true, false),
            "an explicit unsupported title beats a stale Halo 3 heartbeat");
        Check(!TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Unknown, false, false),
            "an ambiguous title without camera ownership fails closed");
        Check(TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Unknown, false, false, true, false),
            "the private frontend exception retains controller input");
        Check(!TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Unknown, false, false, false, false),
            "the normal build keeps ambiguous title input fail-closed");
        Check(TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Halo3ODST, false, true, false, true),
            "private ODST camera ownership permits ordinary gamepad input");
        Check(!TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Halo3ODST, false, true, false, false),
            "public ODST camera ownership keeps controller input stock");
        Check(!TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::HaloCE, false, false, true, true),
            "an explicitly detected unsupported title keeps stock input");
        Check(!TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Unknown, false, true, true, true),
            "owned ODST teardown beats resident-module ambiguity");
        Check(!TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Halo3, true, true),
            "private camera ownership overrides a stale Halo 3 signal");
        Check(!TitleRegistry_Halo3CameraOwnsAmbiguousState(
                  1050, 999, 1000),
            "an ambiguous title cannot inherit a pre-transition heartbeat");
        Check(!TitleRegistry_Halo3CameraOwnsAmbiguousState(
                  1050, 1000, 1000),
            "a heartbeat at the transition boundary is not new ownership");
        Check(TitleRegistry_Halo3CameraOwnsAmbiguousState(
                  1099, 1001, 1000),
            "a post-transition Halo 3 heartbeat owns a short ambiguous window");
        Check(!TitleRegistry_Halo3CameraOwnsAmbiguousState(
                  1101, 1001, 1000),
            "ambiguous Halo 3 ownership expires at the dedicated 100 ms limit");
        Check(!TitleRegistry_Halo3CameraOwnsAmbiguousState(
                  999, 1001, 1000),
            "a non-monotonic camera timestamp fails closed");

        OdstCameraRearmGate gate;
        Check(gate.CanAttemptInstall(), "ODST camera install begins unblocked");
        gate.BlockUntilReload(true);
        Check(!gate.CanAttemptInstall(),
            "stale active camera memory cannot immediately rearm ODST hooks");
        gate.Observe(true, true);
        Check(!gate.CanAttemptInstall(),
            "an active-to-active observation is not a reload edge");
        gate.Observe(true, false);
        Check(!gate.CanAttemptInstall(),
            "an inactive camera waits for the next active level");
        gate.Observe(true, true);
        Check(gate.CanAttemptInstall(),
            "inactive-to-active camera transition rearms ODST hooks");
        gate.BlockUntilReload(true);
        gate.Observe(false, false);
        Check(gate.CanAttemptInstall(),
            "a genuine title exit clears the ODST rearm gate");

        gate.BlockUntilTitleExit();
        gate.Observe(true, false);
        gate.Observe(true, true);
        Check(!gate.CanAttemptInstall(),
            "unsupported/menu camera transitions cannot rearm in-session");
        gate.Observe(false, false);
        Check(gate.CanAttemptInstall(),
            "title exit clears the unsupported-camera session latch");

        OdstPauseRearmGate pauseGate;
        pauseGate.Block();
        pauseGate.Observe(100, true, true, true);
        Check(!pauseGate.CanAttemptInstall(),
            "native ODST pause blocks camera-hook reinstallation");
        pauseGate.Observe(200, true, false, true);
        pauseGate.Observe(1200, true, false, true);
        Check(!pauseGate.CanAttemptInstall(),
            "pause exit requires more than the full stable-camera interval");
        pauseGate.Observe(1201, true, false, true);
        Check(pauseGate.CanAttemptInstall(),
            "stable gameplay after pause exit permits camera-hook reinstall");
        pauseGate.Block();
        pauseGate.Observe(5000, true, false, true);
        pauseGate.Observe(5500, true, false, false);
        pauseGate.Observe(7000, true, false, true);
        Check(!pauseGate.CanAttemptInstall(),
            "camera loss resets the pause-exit stability interval");
        pauseGate.Observe(8001, true, false, true);
        Check(pauseGate.CanAttemptInstall(),
            "a new continuous camera interval clears the pause gate");
        pauseGate.Block();
        pauseGate.Observe(9000, false, false, false);
        Check(pauseGate.CanAttemptInstall(),
            "title exit clears the native-pause reinstall gate");

        OdstFreshCameraDebounce debounce;
        Check(!debounce.Update(100, true),
            "ODST camera does not arm on its first fresh frame");
        Check(!debounce.Update(1100, true),
            "ODST camera requires more than the full stability interval");
        Check(debounce.Update(1101, true),
            "ODST camera arms only after a continuous stability interval");
        debounce.Reset();
        Check(!debounce.Update(5000, true),
            "ODST session re-entry resets the stability debounce");

        Check(EvaluateOdstHeartbeat(1700, 1000, 0, false, false) ==
                  OdstHeartbeatAction::None,
            "ODST tolerates a short delay before its first heartbeat");
        Check(EvaluateOdstHeartbeat(1800, 1000, 0, false, false) ==
                  OdstHeartbeatAction::LevelUnloaded,
            "an inactive camera after install is treated as an unload");
        Check(EvaluateOdstHeartbeat(6100, 1000, 0, false, true) ==
                  OdstHeartbeatAction::NoFirstHeartbeat,
            "active-looking memory cannot retain a hook without a heartbeat");
        Check(EvaluateOdstHeartbeat(7000, 1000, 6200, true, true) ==
                  OdstHeartbeatAction::None,
            "a short heartbeat gap with a ready camera is tolerated");
        Check(EvaluateOdstHeartbeat(6701, 1000, 6000, true, false) ==
                  OdstHeartbeatAction::None,
            "a transient heartbeat gap does not detach presentation even when camera readiness flickers");
        Check(EvaluateOdstHeartbeat(6751, 1000, 6000, true, false) ==
                  OdstHeartbeatAction::LevelUnloaded,
            "an unready camera must exceed the soft timeout before presentation detaches");
        Check(EvaluateOdstHeartbeat(12001, 1000, 7000, true, true) ==
                  OdstHeartbeatAction::LevelUnloaded,
            "a hard heartbeat timeout falls back even with stale ready bytes");
    }

    const TitleDescriptor* halo3 = TitleRegistry_FromModuleName(L"halo3.dll");
    Check(halo3 != nullptr, "Halo 3 module is recognized");
    Check(halo3 && halo3->runtimeSupported, "Halo 3 is the supported baseline adapter");

    const TitleDescriptor* odst =
        TitleRegistry_FromModuleName(L"N:/MCC/HALO3ODST.DLL");
    Check(odst && odst->title == GameTitle::Halo3ODST,
        "ODST paths are matched case-insensitively");
    Check(odst && !odst->runtimeSupported,
        "ODST stays disabled until its adapter passes the title gate");
    Check(odst && odst->capabilities == TitleCapability_None,
        "ODST advertises no public capabilities during private bring-up");
    Check(TitleRegistry_HookPlan(GameTitle::Halo3) == TitleHookPlan::Halo3Full,
        "Halo 3 keeps the full headset-confirmed hook plan");
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    Check(TitleRegistry_HookPlan(GameTitle::Halo3ODST) ==
              TitleHookPlan::OdstExperimentalCameraCore,
        "The explicit private build enables only the ODST camera core");
#else
    Check(TitleRegistry_HookPlan(GameTitle::Halo3ODST) == TitleHookPlan::None,
        "A normal build leaves ODST completely stock");
#endif

    const TitleDescriptor* reach = TitleRegistry_FromModuleName(L"haloreach.dll");
    Check(reach && reach->title == GameTitle::HaloReach, "Reach module is recognized");
    const GameTitle unsupportedTitles[] = {
        GameTitle::HaloReach, GameTitle::Halo4, GameTitle::HaloCE,
        GameTitle::Halo2, GameTitle::Unknown, GameTitle::None,
    };
    for (GameTitle title : unsupportedTitles)
        Check(TitleRegistry_HookPlan(title) == TitleHookPlan::None,
            "Unsupported titles never receive game hooks");
    Check(TitleRegistry_FromModuleName(L"MCC-Win64-Shipping.exe") == nullptr,
        "The MCC host is not mistaken for a game title");
    Check(std::string_view(RuntimeModeName(RuntimeMode::Vehicle)) == "vehicle",
        "Runtime modes have stable diagnostic names");

    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    const std::filesystem::path configDir = std::filesystem::path(tempPath) /
        (L"halomccvr-config-tests-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(configDir);
    const std::filesystem::path primary = configDir / L"halomccvr.cfg";
    const std::filesystem::path legacy = configDir / L"halo3xr.cfg";
    {
        std::ofstream file(legacy);
        file << "screen_width_m = 6.25\n";
        file << "haptic_intensity = malformed\n";
    }
    ConfigLoadMigrating(primary.c_str(), legacy.c_str());
    Check(std::filesystem::exists(primary), "Legacy config migration creates halomccvr.cfg");
    Check(std::filesystem::exists(legacy), "Legacy config migration retains halo3xr.cfg");
    Check(g_config.screen_width_m == 6.25f, "Legacy values survive migration");
    Check(g_config.haptic_intensity == 0.70f,
        "Malformed new values retain their individual default");
    const std::string organizedConfig = ReadTextFile(primary);
    const size_t openXrSection = organizedConfig.find("#  OPENXR & COMFORT");
    const size_t controlsSection = organizedConfig.find("#  CONTROLS & TURNING");
    const size_t aimingSection = organizedConfig.find("#  RETICLE & AIMING");
    const size_t weaponSection = organizedConfig.find("#  WEAPON CALIBRATION");
    const size_t scopeSection = organizedConfig.find("#  EXPERIMENTAL SCOPE");
    const size_t displaySection = organizedConfig.find("#  HUD, PRESENTATION & PERFORMANCE");
    const size_t handsSection = organizedConfig.find("#  GAMEPLAY, HANDS & IK");
    const size_t diagnosticsSection = organizedConfig.find("#  DEVELOPMENT DIAGNOSTICS");
    Check(openXrSection < controlsSection && controlsSection < aimingSection &&
          aimingSection < weaponSection && weaponSection < scopeSection &&
          scopeSection < displaySection && displaySection < handsSection &&
          handsSection < diagnosticsSection,
        "The generated universal config has stable, readable section ordering");
    Check(organizedConfig.find("This ONE file is shared by every supported MCC game") !=
              std::string::npos,
        "The generated config explains that preferences are shared across titles");
    constexpr const char* universalKeys[] = {
        "config_version", "haptic_intensity", "headset_smoothing",
        "aim_stabilization", "screen_width_m", "screen_distance_m",
        "turn_smooth", "turn_snap_deg", "turn_smooth_deg_s", "dpad_hand",
        "crosshair", "crosshair_distance_m", "crosshair_size_deg",
        "reticle_r", "reticle_g", "reticle_b", "kill_reticle",
        "gun_scale", "left_hand_scale", "gun_pitch_deg", "gun_yaw_deg",
        "gun_roll_deg", "gun_forward_m", "scope_enabled", "scope_zoom",
        "scope_screen_width_m", "scope_screen_right_m", "scope_screen_up_m",
        "scope_screen_forward_m", "scope_refresh_divisor", "game_brightness",
        "resolution_scale", "hud_size", "hud_aspect", "hud_curvature",
        "hud_vertical_offset", "motion_blur", "auto_vr", "two_handed_aim",
        "two_hand_toggle", "left_hand_forward_m", "two_hand_zone_right_m",
        "left_grip_forward_m", "arm_ik", "floating_hands",
        "right_shoulder_drop", "shoulder_level", "body_wip", "weapon_probe",
        "hud_probe", "bullet_probe", "right_eye_first"
    };
    for (const char* key : universalKeys)
    {
        const std::string assignment = std::string("\n") + key + " = ";
        const std::string message = std::string("Generated config writes exactly one '") +
            key + "' assignment";
        Check(CountText(organizedConfig, assignment) == 1, message.c_str());
    }
    {
        std::ofstream file(primary);
        file << "config_version = 1\n";
        file << "haptic_intensity = 2.0\n";
        file << "headset_smoothing = 1.0\n";
        file << "aim_stabilization = -1.0\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.haptic_intensity == 1.0f, "Haptic intensity is safely clamped");
    Check(g_config.headset_smoothing == 0.10f,
        "Headset smoothing is capped at the low-latency maximum");
    Check(g_config.aim_stabilization == 0.0f, "Aim stabilization is safely clamped");

    // resolution_scale is free-form: a hand-typed value must survive exactly,
    // not snap to one of the six installer tiers (the pre-2026-07-20 behavior).
    {
        std::ofstream file(primary);
        file << "resolution_scale = 0.90\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.resolution_scale == 0.90f,
        "A custom resolution scale is honored exactly, not snapped to a preset");
    ConfigSave();
    const std::string savedResolutionConfig = ReadTextFile(primary);
    Check(CountText(savedResolutionConfig, "resolution_scale = 0.90") == 1,
        "Organized config keeps the launcher's resolution_scale line compatible");
    ConfigLoad(primary.c_str());
    Check(g_config.resolution_scale == 0.90f,
        "A custom resolution scale survives a save/reload round trip");
    {
        std::ofstream file(primary);
        file << "resolution_scale = 0.05\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.resolution_scale == kResolutionScaleMin,
        "A too-small resolution scale is pulled up to the minimum");
    {
        std::ofstream file(primary);
        file << "resolution_scale = 5.0\n";
    }
    ConfigLoad(primary.c_str());
    Check(!UpdateTwoHandHold(false, true, false) &&
          UpdateTwoHandHold(false, true, true) &&
          UpdateTwoHandHold(true, true, false) &&
          !UpdateTwoHandHold(true, false, true), __func__);

    Check(g_config.resolution_scale == kResolutionScaleMax,
        "A too-large resolution scale is pulled down to the maximum");

    {
        std::ofstream file(primary);
        file << "hud_height = 1.25\n"; // compatibility alias from the first test build
        file << "hud_aspect = 1.35\n";
        file << "hud_vertical_offset = -125\n";
    }
    ConfigLoad(primary.c_str());
    const float migratedCurvature = (0.30f - 1.25f * 0.1f) / 0.60f;
    Check(std::abs(g_config.hud_curvature - migratedCurvature) < 0.0001f &&
          g_config.hud_aspect == 1.35f &&
          g_config.hud_vertical_offset == -125.0f,
        "Legacy HUD curvature, aspect trim, and height migrate exactly");
    ConfigSave();
    ConfigLoad(primary.c_str());
    Check(std::abs(g_config.hud_curvature - migratedCurvature) < 0.01f &&
          g_config.hud_aspect == 1.35f &&
          g_config.hud_vertical_offset == -125.0f,
        "Normalized HUD curvature, aspect, and height survive save/reload");
    {
        std::ofstream file(primary);
        file << "config_version = 2\n";
        file << "hud_curvature = 99\n";
        file << "hud_aspect = 99\n";
        file << "hud_vertical_offset = -999\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.hud_curvature == kHudCurvatureMax &&
          g_config.hud_aspect == kHudAspectMax &&
          g_config.hud_vertical_offset == kHudHeightMin,
        "Excessive HUD curvature, aspect, and height values are safely clamped");

    {
        std::ofstream file(primary);
        file << "config_version = 2\n";
        file << "scope_zoom = 3.39\n";
    }
    ConfigLoad(primary.c_str());
    Check(std::fabs(g_config.scope_zoom - 11.865f) < 1e-4f,
        "Version 2 scope zoom migrates into the tighter gameplay-origin lens");
    ConfigSave();
    ConfigLoad(primary.c_str());
    Check(std::fabs(g_config.scope_zoom - 11.87f) < 1e-4f,
        "Migrated scope zoom is not strengthened again after saving version 4");

    {
        std::ofstream file(primary);
        file << "scope_enabled = 1\n";
        file << "scope_zoom = 99\n";
        file << "scope_screen_width_m = 1\n";
        file << "scope_screen_right_m = -1\n";
        file << "scope_screen_up_m = 1\n";
        file << "scope_screen_forward_m = 0\n";
        file << "scope_refresh_divisor = 99\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.scope_enabled && g_config.scope_zoom == 24.0f &&
          g_config.scope_screen_width_m == 0.25f &&
          g_config.scope_screen_right_m == -0.30f &&
          g_config.scope_screen_up_m == 0.30f &&
          g_config.scope_screen_forward_m == 0.05f &&
          g_config.scope_refresh_divisor == 4,
        "Universal scope settings are safely clamped");
    g_config.scope_zoom = 8.25f;
    g_config.scope_screen_width_m = 0.12f;
    g_config.scope_screen_right_m = 0.03f;
    g_config.scope_screen_up_m = 0.09f;
    g_config.scope_screen_forward_m = 0.35f;
    g_config.scope_refresh_divisor = 3;
    ConfigSave();
    ConfigLoad(primary.c_str());
    Check(g_config.scope_zoom == 8.25f &&
          g_config.scope_screen_width_m == 0.12f &&
          g_config.scope_screen_right_m == 0.03f &&
          g_config.scope_screen_up_m == 0.09f &&
          g_config.scope_screen_forward_m == 0.35f &&
          g_config.scope_refresh_divisor == 3,
        "Universal scope settings survive a save/reload round trip");

    // Deleting the file is the documented "put everything back" escape hatch.
    {
        std::ofstream file(primary);
        file << "gun_scale = 2.5\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.gun_scale == 2.5f, "A tuned value loads before the reset test");
    std::filesystem::remove(primary);
    ConfigLoad(primary.c_str());
    Check(std::filesystem::exists(primary),
        "Deleting the config file recreates it on the next load");
    const Config defaults{};
    Check(g_config.gun_scale == defaults.gun_scale &&
          g_config.resolution_scale == defaults.resolution_scale &&
          g_config.hud_size == defaults.hud_size &&
          g_config.hud_aspect == defaults.hud_aspect &&
          g_config.hud_curvature == defaults.hud_curvature &&
          g_config.hud_vertical_offset == defaults.hud_vertical_offset &&
          g_config.scope_enabled &&
          g_config.scope_zoom == 12.0f &&
          g_config.scope_screen_width_m == 0.182f &&
          g_config.scope_screen_right_m == -0.081f &&
          g_config.scope_screen_up_m == 0.207f &&
          g_config.scope_screen_forward_m == 0.222f &&
          g_config.scope_refresh_divisor == 2,
        "The recreated config file carries the struct defaults");
    std::filesystem::remove_all(configDir);

    MenuChordDetector chord;
    MenuChordResult chordResult = chord.Update(1000, true, false);
    Check(!chordResult.toggled, "One stick click does not toggle the menu");
    chordResult = chord.Update(1249, true, true);
    Check(chordResult.toggled && chordResult.consumeClicks,
        "L3+R3 toggles inside the 250 ms window and consumes both clicks");
    Check(!chord.Update(1300, true, true).toggled,
        "A held chord toggles only once");
    Check(chord.Update(1350, false, true).consumeClicks,
        "Chord clicks stay consumed until both are released");
    chord.Update(1400, false, false);
    chord.Update(2000, true, false);
    Check(!chord.Update(2251, true, true).toggled,
        "A chord outside the 250 ms window does not toggle");
    chord.Update(2300, false, false);
    Check(chord.Update(2400, true, true).toggled,
        "A simultaneous chord works after release");

    ScopeToggleDetector scopeToggle;
    Check(!scopeToggle.Update(true, true, false).changed,
        "R3 press arms the scope without toggling early");
    ScopeToggleUpdate scopeResult = scopeToggle.Update(true, false, false);
    Check(scopeResult.changed && scopeResult.active,
        "R3 release opens the universal scope");
    scopeToggle.Update(true, true, false);
    scopeResult = scopeToggle.Update(true, false, false);
    Check(scopeResult.changed && !scopeResult.active,
        "A second R3 click closes the universal scope");

    scopeToggle.Reset();
    scopeToggle.Update(true, true, false);      // R3 begins first
    scopeToggle.Update(true, true, true);       // L3 joins; menu consumes chord
    scopeResult = scopeToggle.Update(true, false, true);
    Check(!scopeResult.changed && !scopeResult.active,
        "A staggered L3+R3 menu chord cancels the pending scope toggle");
    scopeToggle.Reset();
    scopeToggle.Update(true, true, true);       // simultaneous menu chord
    scopeResult = scopeToggle.Update(true, false, true);
    Check(!scopeResult.changed && !scopeResult.active,
        "A simultaneous L3+R3 menu chord never toggles the scope");

    scopeToggle.Update(true, true, false);
    scopeToggle.Update(true, false, false);
    scopeResult = scopeToggle.Update(false, false, false);
    Check(scopeResult.changed && !scopeResult.active,
        "Losing gameplay or disabling the feature resets an active scope");
    scopeToggle.Update(false, true, false);
    scopeToggle.Update(true, true, false);
    scopeResult = scopeToggle.Update(true, false, false);
    Check(!scopeResult.changed && !scopeResult.active,
        "R3 held across gameplay entry cannot cause a surprise toggle");

    const float identity[4] = {0, 0, 0, 1};
    const float scopeOrigin[3] = {1, 2, 3};
    const ScopeQuadTransform quad = ComputeScopeQuadTransform(
        identity, scopeOrigin, 0.04f, 0.08f, 0.30f, 0.10f);
    Check(std::fabs(quad.position[0] - 1.04f) < 1e-5f &&
          std::fabs(quad.position[1] - 2.08f) < 1e-5f &&
          std::fabs(quad.position[2] - 2.70f) < 1e-5f,
        "Scope offsets follow the gun's local right/up/forward axes");
    Check(std::fabs(quad.width - 0.10f) < 1e-5f &&
          std::fabs(quad.height - 0.075f) < 1e-5f,
        "Scope is fixed-size 4:3 geometry independent of headset distance");

    const float gameBasis[9] = {
        1, 0, 0,  // forward
        0, 1, 0,  // left
        0, 0, 1}; // up
    const float safeCameraOrigin[3] = {4.0f, 5.0f, 6.0f};
    const float bulletForward[3] = {1.0f, 0.02f, -0.01f};
    ScopeCameraPose scopeCamera{};
    Check(ComputeScopeCameraPose(gameBasis, safeCameraOrigin,
                                 bulletForward, scopeCamera),
        "Scope camera accepts valid gameplay-camera and bullet inputs");
    Check(std::fabs(scopeCamera.position[0] - 4.0f) < 1e-5f &&
          std::fabs(scopeCamera.position[1] - 5.0f) < 1e-5f &&
          std::fabs(scopeCamera.position[2] - 6.0f) < 1e-5f,
        "Scope camera keeps Halo's collision-safe gameplay origin");
    const float bulletLength = std::sqrt(1.0f + 0.02f * 0.02f + 0.01f * 0.01f);
    Check(std::fabs(scopeCamera.forward[0] - bulletForward[0] / bulletLength) < 1e-5f &&
          std::fabs(scopeCamera.forward[1] - bulletForward[1] / bulletLength) < 1e-5f &&
          std::fabs(scopeCamera.forward[2] - bulletForward[2] / bulletLength) < 1e-5f,
        "Remote scope center continues along Halo's actual bullet direction");

    const ScopeProjectionTangents scopeLens =
        ComputeScopeProjectionTangents(2.5f, 16.0f / 9.0f);
    Check(std::fabs(scopeLens.horizontal / scopeLens.vertical - 16.0f / 9.0f) < 1e-5f,
        "Scope render projection matches its source surface before cropping");
    const float croppedHorizontal = scopeLens.horizontal * (4.0f / 3.0f) / (16.0f / 9.0f);
    Check(std::fabs(croppedHorizontal / scopeLens.vertical - 4.0f / 3.0f) < 1e-5f &&
          std::fabs(croppedHorizontal - 0.70020754f / 2.5f) < 1e-5f,
        "Center-cropped scope image is an undistorted 4:3 2.5x lens");

    ScopeRefreshScheduler refresh;
    Check(!refresh.Advance(true, 2) && refresh.Advance(true, 2),
        "Scope refresh divisor 2 renders every second active frame");
    Check(!refresh.Advance(false, 2) && !refresh.Advance(true, 2) &&
          refresh.Advance(true, 2),
        "Closing the scope resets its image refresh schedule");
    Check(refresh.Advance(true, 0),
        "Scope refresh divisor clamps safely to one");

    ScopeZoomController runtimeZoom;
    Check(std::fabs(runtimeZoom.Update(true, 1.0f, 0.5f, 8.0f) - 8.0f) < 1e-5f,
        "Opening the scope restores configured zoom before applying stick input");
    Check(std::fabs(runtimeZoom.Update(true, 1.0f, 0.10f, 8.0f) - 9.0f) < 1e-5f,
        "Right-stick up increases runtime scope zoom");
    Check(std::fabs(runtimeZoom.Update(true, -1.0f, 0.10f, 8.0f) - 8.0f) < 1e-5f,
        "Right-stick down decreases runtime scope zoom");
    Check(std::fabs(runtimeZoom.Update(true, 0.15f, 0.10f, 8.0f) - 8.0f) < 1e-5f,
        "Scope zoom ignores right-stick deadzone noise");
    runtimeZoom.Update(false, 0.0f, 0.0f, 8.0f);
    Check(std::fabs(runtimeZoom.Update(true, 0.0f, 0.0f, 12.0f) - 12.0f) < 1e-5f,
        "Reopening the scope resets temporary zoom to the configured default");

    ScopeZoomResolver zoomResolver;
    zoomResolver.RequestToggle();
    Check(!zoomResolver.Update(true, false) && zoomResolver.Update(true, false),
        "A weapon with no native zoom opens the fallback scope after detection");
    zoomResolver.RequestToggle();
    Check(zoomResolver.Update(true, false) && !zoomResolver.Update(true, false),
        "A second non-zoom weapon click closes the fallback scope");
    zoomResolver.Reset();
    Check(zoomResolver.Update(true, true),
        "Halo native zoom immediately owns scope visibility");
    zoomResolver.RequestToggle();
    Check(zoomResolver.Update(true, true),
        "A second authored zoom stage keeps the scope visible");
    zoomResolver.RequestToggle();
    Check(!zoomResolver.Update(true, false) && !zoomResolver.Update(true, false),
        "Leaving Halo native zoom closes without enabling fallback");
    zoomResolver.Reset();
    zoomResolver.Update(true, true);
    Check(!zoomResolver.Update(true, false),
        "Native zoom can close before its R3 release is observed");
    zoomResolver.RequestToggle();
    Check(!zoomResolver.Update(true, false) && !zoomResolver.Update(true, false),
        "The late release from native zoom-off is not mistaken for fallback");

    PauseLevelRecovery pauseRecovery;
    Check(!pauseRecovery.Update(true, false, false),
        "Pause recovery stays armed during an ordinary pause");
    Check(!pauseRecovery.Update(true, true, false),
        "Pause recovery records loading without resuming early");
    Check(pauseRecovery.Update(true, false, true),
        "Pause recovery restores 3D when restarted level becomes stable");
    Check(!pauseRecovery.Update(true, false, true),
        "Pause recovery fires only once per loading gap");
    pauseRecovery.Update(true, true, false);
    Check(!pauseRecovery.Update(false, false, true),
        "Leaving pause resets an incomplete restart recovery");

    const float rayOrigin[3] = { 0.0f, 0.0f, 0.0f };
    const float rayForward[3] = { 0.0f, 0.0f, -1.0f };
    MenuPointerHit hit = IntersectMenuQuad(rayOrigin, rayForward,
        1.2f, 1.1f, 0.825f, -0.08f);
    Check(hit.hit && hit.u == 0.5f, "Forward controller ray hits the menu center column");
    const float rayAway[3] = { 0.0f, 0.0f, 1.0f };
    Check(!IntersectMenuQuad(rayOrigin, rayAway, 1.2f, 1.1f, 0.825f, -0.08f).hit,
        "Controller rays pointing away from the menu miss");
    Check(BlendXInputMotors(0, 0) == 0.0f, "Zero XInput rumble stops haptics");
    Check(BlendXInputMotors(65535, 65535) == 1.0f,
        "Both full XInput motors produce full portable haptics");
    const float lowOnly = BlendXInputMotors(65535, 0);
    const float highOnly = BlendXInputMotors(0, 65535);
    Check(lowOnly > highOnly && highOnly > 0.0f,
        "Both motor bands contribute to the blended haptic amplitude");

    if (g_failures == 0)
        std::cout << "HaloMCCVR core tests passed\n";
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
