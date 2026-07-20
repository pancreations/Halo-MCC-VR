#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <windows.h>

#include "config.h"
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
        file << "aim_stabilization = -1.0\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.haptic_intensity == 1.0f, "Haptic intensity is safely clamped");
    Check(g_config.aim_stabilization == 0.0f, "Aim stabilization is safely clamped");
    std::filesystem::remove_all(configDir);

    if (g_failures == 0)
        std::cout << "HaloMCCVR core tests passed\n";
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
