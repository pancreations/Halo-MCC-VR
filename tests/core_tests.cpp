#include <cstdlib>
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
    Check(g_config.resolution_scale == kResolutionScaleMax,
        "A too-large resolution scale is pulled down to the maximum");

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
    Check(g_config.scope_enabled && g_config.scope_zoom == 8.0f &&
          g_config.scope_screen_width_m == 0.25f &&
          g_config.scope_screen_right_m == -0.30f &&
          g_config.scope_screen_up_m == 0.30f &&
          g_config.scope_screen_forward_m == 0.05f &&
          g_config.scope_refresh_divisor == 4,
        "Universal scope settings are safely clamped");
    g_config.scope_zoom = 3.25f;
    g_config.scope_screen_width_m = 0.12f;
    g_config.scope_screen_right_m = 0.03f;
    g_config.scope_screen_up_m = 0.09f;
    g_config.scope_screen_forward_m = 0.35f;
    g_config.scope_refresh_divisor = 3;
    ConfigSave();
    ConfigLoad(primary.c_str());
    Check(g_config.scope_zoom == 3.25f &&
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
          g_config.hud_size == defaults.hud_size,
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

    ScopeRefreshScheduler refresh;
    Check(!refresh.Advance(true, 2) && refresh.Advance(true, 2),
        "Scope refresh divisor 2 renders every second active frame");
    Check(!refresh.Advance(false, 2) && !refresh.Advance(true, 2) &&
          refresh.Advance(true, 2),
        "Closing the scope resets its image refresh schedule");
    Check(refresh.Advance(true, 0),
        "Scope refresh divisor clamps safely to one");

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
