#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <windows.h>

#include "config.h"
#include "input_logic.h"
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
}

int main()
{
    const TitleDescriptor* halo3 = TitleRegistry_FromModuleName(L"halo3.dll");
    Check(halo3 != nullptr, "Halo 3 module is recognized");
    Check(halo3 && halo3->runtimeSupported, "Halo 3 is the supported baseline adapter");

    const TitleDescriptor* odst =
        TitleRegistry_FromModuleName(L"N:/MCC/HALO3ODST.DLL");
    Check(odst && odst->title == GameTitle::Halo3ODST,
        "ODST paths are matched case-insensitively");
    Check(odst && !odst->runtimeSupported,
        "ODST stays disabled until its adapter passes the title gate");

    const TitleDescriptor* reach = TitleRegistry_FromModuleName(L"haloreach.dll");
    Check(reach && reach->title == GameTitle::HaloReach, "Reach module is recognized");
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
