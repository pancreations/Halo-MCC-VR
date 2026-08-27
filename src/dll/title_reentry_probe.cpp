#include "title_reentry_probe.h"

#include <array>
#include <atomic>
#include <windows.h>

#include "../common/halo2_render_logic.h"
#include "../common/title_registry.h"

namespace
{
    constexpr uint64_t kPresentHintFreshMs = 250;
    constexpr uint64_t kActivityFreshMs = 250;
    constexpr uint8_t kAlreadyRunningSamples = 120; // 6 s at 50 ms worker.

    struct ProbeEvidence
    {
        GameTitle title;
        size_t slot;
        uintptr_t playerViewRva;
        size_t stride;
    };

    constexpr ProbeEvidence kEvidence[] = {
        { GameTitle::Halo3, 0, 0x02D2F680, 0x2820 },
        { GameTitle::Halo3ODST, 1, 0x02D73590, 0x2810 },
        { GameTitle::HaloReach, 2, 0x029F2B90, 0x0A40 },
        { GameTitle::Halo4, 3, 0x030AD1C0, 0x0AD0 },
        { GameTitle::Halo2, 5, 0, 0 },
    };

    struct ActivityState
    {
        uintptr_t moduleBase = 0;
        uint64_t fingerprint = 0;
        uint64_t lastChangeMs = 0;
        uint8_t sawStill = 0;
        uint8_t changeRun = 0;
    };

    std::array<ActivityState, std::size(kEvidence)> g_activity{};
    std::atomic<GameTitle> g_presentHint{GameTitle::None};
    std::atomic<uint64_t> g_presentHintMs{0};

    bool ReadableCommittedRange(uintptr_t address, size_t bytes,
                                uintptr_t expectedAllocationBase) noexcept
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!address || !bytes ||
            VirtualQuery(reinterpret_cast<const void*>(address), &mbi,
                         sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT ||
            mbi.AllocationBase != reinterpret_cast<void*>(expectedAllocationBase) ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        return address >= begin && address <= end && bytes <= end - address;
    }

    bool FingerprintPlayerView(uintptr_t moduleBase, uintptr_t rva,
                               size_t stride, uint64_t& out) noexcept
    {
        const uintptr_t address = moduleBase + rva;
        if (!ReadableCommittedRange(address, stride, moduleBase))
            return false;
        uint64_t hash = 1469598103934665603ull;
        __try
        {
            const auto* bytes = reinterpret_cast<const uint8_t*>(address);
            for (size_t i = 0; i < stride; ++i)
            {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        out = hash ? hash : 1;
        return true;
    }

    bool FingerprintHalo2(uintptr_t moduleBase, uint64_t& out,
                          bool& explicitlyStill) noexcept
    {
        explicitlyStill = false;
        const uintptr_t slot = moduleBase + kHalo2GameTimeSlotRva;
        if (!ReadableCommittedRange(slot, sizeof(uintptr_t), moduleBase))
            return false;
        uintptr_t object = 0;
        __try { object = *reinterpret_cast<const uintptr_t*>(slot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!object)
            return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(object), &mbi,
                         sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (object < begin || object + kHalo2GameTimeCurrentTickOffset + 4 > end)
            return false;
        uint8_t initialized = 0;
        uint32_t tick = 0;
        __try
        {
            initialized = *reinterpret_cast<const uint8_t*>(
                object + kHalo2GameTimeInitializedOffset);
            tick = *reinterpret_cast<const uint32_t*>(
                object + kHalo2GameTimeCurrentTickOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (initialized > 1)
            return false;
        if (!initialized)
        {
            explicitlyStill = true;
            out = object ? object : 1;
            return true;
        }
        out = (static_cast<uint64_t>(tick) << 32) ^ object;
        if (!out) out = 1;
        return true;
    }

    bool SampleEvidence(size_t index, uintptr_t moduleBase, uint64_t nowMs)
    {
        ActivityState& state = g_activity[index];
        if (!moduleBase)
        {
            state = {};
            return false;
        }
        if (state.moduleBase != moduleBase)
        {
            state = {};
            state.moduleBase = moduleBase;
        }

        uint64_t fingerprint = 0;
        bool explicitStill = false;
        const ProbeEvidence& evidence = kEvidence[index];
        const bool readable = evidence.title == GameTitle::Halo2
            ? FingerprintHalo2(moduleBase, fingerprint, explicitStill)
            : FingerprintPlayerView(moduleBase, evidence.playerViewRva,
                                    evidence.stride, fingerprint);
        if (!readable)
        {
            state.changeRun = 0;
            return false;
        }
        if (!state.fingerprint)
        {
            state.fingerprint = fingerprint;
            if (explicitStill) state.sawStill = 1;
            return false;
        }

        const bool changed = fingerprint != state.fingerprint;
        state.fingerprint = fingerprint;
        if (explicitStill || !changed)
        {
            state.sawStill = 1;
            state.changeRun = 0;
            return false;
        }

        state.lastChangeMs = nowMs;
        if (state.changeRun < kAlreadyRunningSamples)
            ++state.changeRun;
        return state.sawStill || state.changeRun >= kAlreadyRunningSamples;
    }
}

void TitleReentryProbe_PublishPresentCaller(const void* caller,
                                             uint64_t nowMs) noexcept
{
    if (!caller || !nowMs)
        return;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(caller, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        !mbi.AllocationBase)
        return;
    const uintptr_t allocation = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    size_t count = 0;
    const TitleDescriptor* titles = TitleRegistry_All(count);
    for (size_t i = 0; i < count; ++i)
    {
        HMODULE module = GetModuleHandleW(titles[i].moduleName);
        if (reinterpret_cast<uintptr_t>(module) != allocation)
            continue;
        if (titles[i].title == GameTitle::HaloCE)
            return;
        // Publish the timestamp last. Clearing it first makes a concurrent
        // worker either see no hint or a fully paired title+timestamp, never a
        // newly written title with the previous title's still-fresh stamp.
        g_presentHintMs.store(0, std::memory_order_release);
        g_presentHint.store(titles[i].title, std::memory_order_release);
        g_presentHintMs.store(nowMs, std::memory_order_release);
        return;
    }
}

GameTitle TitleReentryProbe_Resolve(const TitleRuntimeModuleSet& modules,
                                    uint64_t nowMs,
                                    GameTitle retainedTitle) noexcept
{
    const GameTitle hintBefore =
        g_presentHint.load(std::memory_order_acquire);
    const uint64_t hintMs =
        g_presentHintMs.load(std::memory_order_acquire);
    const GameTitle hintAfter =
        g_presentHint.load(std::memory_order_acquire);
    const size_t hintSlot = TitleRuntimeSlotIndex(hintAfter);
    if (hintBefore == hintAfter && hintMs != 0 &&
        hintSlot < kTitleRuntimeSlotCount && modules.moduleBases[hintSlot] &&
        nowMs >= hintMs && nowMs - hintMs <= kPresentHintFreshMs)
        return hintAfter;

    GameTitle winner = GameTitle::None;
    unsigned candidates = 0;
    for (size_t i = 0; i < std::size(kEvidence); ++i)
    {
        const ProbeEvidence& evidence = kEvidence[i];
        const bool active = SampleEvidence(
            i, modules.moduleBases[evidence.slot], nowMs);
        const ActivityState& state = g_activity[i];
        if (!active || !state.lastChangeMs || nowMs < state.lastChangeMs ||
            nowMs - state.lastChangeMs > kActivityFreshMs)
            continue;
        winner = evidence.title;
        ++candidates;
    }
    if (candidates == 1)
        return winner;

    // Keep the already selected raw adapter stable while its module remains
    // resident. This is intentionally weaker than runtime ownership: the
    // existing lifecycle/heartbeat resolver can still revoke all capabilities
    // immediately when the title stops running. A different title's unique
    // fresh liveness evidence above always preempts this retention.
    const size_t retainedSlot = TitleRuntimeSlotIndex(retainedTitle);
    if (retainedTitle != GameTitle::HaloCE &&
        retainedTitle != GameTitle::None &&
        retainedTitle != GameTitle::Unknown &&
        retainedSlot < kTitleRuntimeSlotCount &&
        modules.moduleBases[retainedSlot])
    {
        return retainedTitle;
    }
    return GameTitle::None;
}
