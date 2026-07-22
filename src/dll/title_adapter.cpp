#include "title_adapter.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <windows.h>

#include "../common/log.h"

namespace
{
    std::atomic<GameTitle> g_activeTitle{ GameTitle::None };
    std::atomic<uint64_t> g_activeTitleEpochMs{ 0 };
    std::atomic<RuntimeMode> g_runtimeMode{ RuntimeMode::Shell };
}

const TitleDescriptor* TitleAdapter_GetActive()
{
    return TitleRegistry_Find(g_activeTitle.load(std::memory_order_acquire));
}

GameTitle TitleAdapter_GetActiveTitle()
{
    return g_activeTitle.load(std::memory_order_acquire);
}

uint64_t TitleAdapter_GetActiveTitleEpochMs()
{
    return g_activeTitleEpochMs.load(std::memory_order_acquire);
}

RuntimeMode TitleAdapter_GetRuntimeMode()
{
    return g_runtimeMode.load(std::memory_order_acquire);
}

void TitleAdapter_SetRuntimeMode(RuntimeMode mode)
{
    const RuntimeMode previous = g_runtimeMode.exchange(mode, std::memory_order_acq_rel);
    if (previous != mode)
        LOG("Runtime mode: %s -> %s", RuntimeModeName(previous), RuntimeModeName(mode));
}

const TitleDescriptor* TitleAdapter_PollLoaded()
{
    size_t count = 0;
    const TitleDescriptor* titles = TitleRegistry_All(count);
    const TitleDescriptor* detected = nullptr;
    size_t detectedCount = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (GetModuleHandleW(titles[i].moduleName))
        {
            if (!detected)
                detected = &titles[i];
            ++detectedCount;
        }
    }

    const bool ambiguous = detectedCount > 1;
    const GameTitle next = ambiguous ? GameTitle::Unknown :
        (detected ? detected->title : GameTitle::None);
    const GameTitle previous = g_activeTitle.load(std::memory_order_acquire);
    if (previous == next)
        return ambiguous ? nullptr : detected;
    // Publish the transition epoch before the new title. An acquire load that
    // observes Unknown can therefore never inherit a Halo 3 camera heartbeat
    // from before the resident-module ambiguity began.
    g_activeTitleEpochMs.store(GetTickCount64(), std::memory_order_release);
    g_activeTitle.store(next, std::memory_order_release);

    if (ambiguous)
    {
        // Name the resident modules so the log shows exactly which titles MCC
        // kept loaded (e.g. halo3.dll left resident after switching to ODST).
        // This is the evidence for a heartbeat-based retention fix so a
        // resident-but-idle second module cannot disable the active title.
        char names[256];
        names[0] = '\0';
        for (size_t i = 0; i < count; ++i)
        {
            if (GetModuleHandleW(titles[i].moduleName))
            {
                const size_t used = strlen(names);
                _snprintf_s(names + used, sizeof(names) - used, _TRUNCATE,
                            "%s%ls", used ? "," : "", titles[i].moduleName);
            }
        }
        LOG("Title adapter: ambiguous MCC state (%zu game modules loaded: %s); "
            "disabling game hooks", detectedCount, names);
        TitleAdapter_SetRuntimeMode(RuntimeMode::Unsupported);
        return nullptr;
    }
    if (!detected)
    {
        LOG("Title adapter: no MCC game module is loaded");
        TitleAdapter_SetRuntimeMode(RuntimeMode::Shell);
    }
    else if (!detected->runtimeSupported)
    {
        if (TitleRegistry_HookPlan(detected->title) ==
            TitleHookPlan::OdstExperimentalCameraCore)
        {
            LOG("Title adapter: detected %s (%ls); public adapter remains "
                "unsupported, private camera-only bring-up is build-enabled",
                detected->displayName, detected->moduleName);
        }
        else
        {
            LOG("Title adapter: detected %s (%ls); adapter not implemented, "
                "leaving stock game untouched",
                detected->displayName, detected->moduleName);
        }
        TitleAdapter_SetRuntimeMode(RuntimeMode::Unsupported);
    }
    else
    {
        LOG("Title adapter: detected supported title %s (%ls)",
            detected->displayName, detected->moduleName);
        TitleAdapter_SetRuntimeMode(RuntimeMode::Loading);
    }
    return detected;
}
