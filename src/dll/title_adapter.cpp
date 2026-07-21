#include "title_adapter.h"

#include <atomic>
#include <windows.h>

#include "../common/log.h"

namespace
{
    std::atomic<GameTitle> g_activeTitle{ GameTitle::None };
    std::atomic<RuntimeMode> g_runtimeMode{ RuntimeMode::Shell };
}

const TitleDescriptor* TitleAdapter_GetActive()
{
    return TitleRegistry_Find(g_activeTitle.load(std::memory_order_acquire));
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
    const GameTitle previous = g_activeTitle.exchange(next, std::memory_order_acq_rel);
    if (previous == next)
        return ambiguous ? nullptr : detected;

    if (ambiguous)
    {
        LOG("Title adapter: ambiguous MCC state (%zu game modules loaded); disabling game hooks",
            detectedCount);
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
        LOG("Title adapter: detected %s (%ls); adapter not implemented, leaving stock game untouched",
            detected->displayName, detected->moduleName);
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
