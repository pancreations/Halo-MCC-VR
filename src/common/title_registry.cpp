#include "title_registry.h"

#include <cwctype>

namespace
{
    constexpr uint32_t kHalo3Capabilities =
        TitleCapability_Stereo |
        TitleCapability_ControllerAim |
        TitleCapability_Hud |
        TitleCapability_ArmIk;

    constexpr TitleDescriptor kTitles[] = {
        { GameTitle::Halo3, L"halo3.dll", "Halo 3", true, kHalo3Capabilities },
        { GameTitle::Halo3ODST, L"halo3odst.dll", "Halo 3: ODST", false, TitleCapability_None },
        { GameTitle::HaloReach, L"haloreach.dll", "Halo: Reach", false, TitleCapability_None },
        { GameTitle::Halo4, L"halo4.dll", "Halo 4", false, TitleCapability_None },
        { GameTitle::HaloCE, L"halo1.dll", "Halo: CE Anniversary", false, TitleCapability_None },
        { GameTitle::Halo2, L"halo2.dll", "Halo 2 Anniversary", false, TitleCapability_None },
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
