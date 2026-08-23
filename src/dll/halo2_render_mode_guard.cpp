#include "halo2_render_mode_guard.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/halo2_render_logic.h"
#include "../common/log.h"
#include "game.h"

namespace
{
    using ApplierFn = void(__fastcall*)();

    std::atomic<bool> g_installed{false};
    std::atomic<uint32_t> g_generation{0};
    std::atomic<uintptr_t> g_moduleBase{0};
    std::atomic<uintptr_t> g_original{0};
    std::atomic<uint32_t> g_activeCallbacks{0};
    std::atomic<uint64_t> g_calls{0};
    std::atomic<uint64_t> g_honoured{0};
    std::atomic<uint64_t> g_suppressed{0};
    void* g_target = nullptr;
    HMODULE g_moduleReference = nullptr;
    uint32_t g_rejectedGeneration = 0;
    uint64_t g_lastSuppressLogMs = 0;
    char g_lastSwitch[160] = "none";

    template <typename T>
    bool ReadGuarded(uintptr_t address, T& value) noexcept
    {
        if (!address)
            return false;
        __try
        {
            value = *reinterpret_cast<const volatile T*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    template <typename T>
    bool WriteGuarded(uintptr_t address, T value) noexcept
    {
        if (!address)
            return false;
        __try
        {
            *reinterpret_cast<volatile T*>(address) = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool EntryBytesMatch(uintptr_t address, const uint8_t* expected, size_t bytes) noexcept
    {
        uint8_t actual[32]{};
        if (!address || bytes > sizeof(actual))
            return false;
        __try { std::memcpy(actual, reinterpret_cast<const void*>(address), bytes); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return std::memcmp(actual, expected, bytes) == 0;
    }

    bool IsExecutable(uintptr_t address) noexcept
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)))
            return false;
        if (info.State != MEM_COMMIT)
            return false;
        const DWORD mask = info.Protect & 0xFF;
        return !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mask == PAGE_EXECUTE_READ || mask == PAGE_EXECUTE_READWRITE ||
             mask == PAGE_EXECUTE_WRITECOPY || mask == PAGE_EXECUTE);
    }

    void DescribeEvidence(
        uintptr_t base, const Halo2SwitchInputEvidence& input, char* out,
        size_t bytes) noexcept
    {
        uint8_t forcedFading = 0xFF;
        uint8_t fadeTarget = 0xFF;
        uintptr_t fadeState = 0;
        (void)ReadGuarded(base + kHalo2ForcedLegacyFadingFlagRva, forcedFading);
        if (ReadGuarded(base + kHalo2ForcedLegacyFadeStateSlotRva, fadeState) && fadeState)
            (void)ReadGuarded(fadeState + kHalo2ForcedLegacyFadeTargetOffset, fadeTarget);
        _snprintf_s(out, bytes, _TRUNCATE,
                    "keyboard Tab %s, physical pad Back %s (last seen %llu ms ago, "
                    "pass-through %s), mod Back fed %llu ms ago, forced-legacy-fading "
                    "flag 0x%02X, fade target byte 0x%02X",
                    input.tabDown ? "DOWN" : "up",
                    input.physicalBackDown ? "DOWN" : "up",
                    static_cast<unsigned long long>(input.physicalBackAgeMs),
                    g_config.halo2_gamepad_graphics_switch ? "on" : "off",
                    static_cast<unsigned long long>(input.virtualBackAgeMs),
                    forcedFading, fadeTarget);
    }

    __declspec(noinline) void __fastcall ApplierDetour()
    {
        const auto original =
            reinterpret_cast<ApplierFn>(g_original.load(std::memory_order_acquire));
        if (!original)
            return;
        g_activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        g_calls.fetch_add(1, std::memory_order_relaxed);
        const uintptr_t base = g_moduleBase.load(std::memory_order_acquire);
        int32_t requested = 0;
        int32_t applied = 0;
        if (base && g_installed.load(std::memory_order_acquire) &&
            ReadGuarded(base + kHalo2RequestedRenderModeRva, requested) &&
            ReadGuarded(base + kHalo2AppliedRenderModeRva, applied))
        {
            Halo2SwitchInputEvidence input{};
            Input_Halo2SwitchInputEvidence(input);
            switch (Halo2DecideRenderModeSwitch(
                requested, applied, Halo2SwitchInputPresent(input)))
            {
            case Halo2RenderModeSwitchDecision::Honour:
            {
                g_honoured.fetch_add(1, std::memory_order_relaxed);
                char evidence[256];
                DescribeEvidence(base, input, evidence, sizeof(evidence));
                _snprintf_s(g_lastSwitch, sizeof(g_lastSwitch), _TRUNCATE,
                            "honoured (%s)", evidence);
                LOG("Halo 2 renderer switch HONOURED: request %d -> applied %d; %s",
                    requested, applied, evidence);
                break;
            }
            case Halo2RenderModeSwitchDecision::Suppress:
            {
                const uint64_t n = g_suppressed.fetch_add(1, std::memory_order_relaxed) + 1;
                (void)WriteGuarded(base + kHalo2RequestedRenderModeRva, applied);
                const uint64_t now = GetTickCount64();
                if (now - g_lastSuppressLogMs >= 1000)
                {
                    g_lastSuppressLogMs = now;
                    char evidence[256];
                    DescribeEvidence(base, input, evidence, sizeof(evidence));
                    LOG("Halo 2 renderer switch SUPPRESSED (#%llu): the engine "
                        "requested mode %d while mode %d is applied with no switch "
                        "input present - %s; the request was written back to the "
                        "applied mode",
                        static_cast<unsigned long long>(n), requested, applied,
                        evidence);
                }
                break;
            }
            case Halo2RenderModeSwitchDecision::NoChange:
            default:
                break;
            }
        }
        __try { original(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    bool Remove(const char* reason) noexcept
    {
        g_installed.store(false, std::memory_order_release);
        if (g_target)
        {
            if (MH_DisableHook(g_target) != MH_OK)
                return false;
            for (int i = 0; i < 200 && g_activeCallbacks.load(std::memory_order_acquire); ++i)
                Sleep(10);
            if (g_activeCallbacks.load(std::memory_order_acquire))
                return false;
            (void)MH_RemoveHook(g_target);
            g_target = nullptr;
        }
        g_original.store(0, std::memory_order_release);
        g_moduleBase.store(0, std::memory_order_release);
        g_generation.store(0, std::memory_order_release);
        // Non-owning identity; MCC alone owns the title DLL lifetime.
        g_moduleReference = nullptr;
        if (reason)
            LOG("Halo 2 renderer switch guard removed (%s)", reason);
        return true;
    }

    bool Install(uintptr_t base, uint32_t generation) noexcept
    {
        if (g_rejectedGeneration == generation)
            return false;
        const uintptr_t applier = base + kHalo2RenderModeApplierRva;
        if (!IsExecutable(applier) ||
            !EntryBytesMatch(applier, kHalo2RenderModeApplierEntryBytes,
                             sizeof(kHalo2RenderModeApplierEntryBytes)))
        {
            LOG("Halo 2 renderer switch guard WITHHELD: the applier's entry bytes "
                "at +0x%X do not match the proven module",
                static_cast<unsigned>(kHalo2RenderModeApplierRva));
            g_rejectedGeneration = generation;
            return false;
        }
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(base), &module) || !module)
            return false;
        void* trampoline = nullptr;
        const MH_STATUS created = MH_CreateHook(
            reinterpret_cast<void*>(applier),
            reinterpret_cast<void*>(&ApplierDetour), &trampoline);
        if (created != MH_OK || !trampoline)
        {
            LOG("Halo 2 renderer switch guard WITHHELD: hook create=%d",
                static_cast<int>(created));
            if (created == MH_OK)
                (void)MH_RemoveHook(reinterpret_cast<void*>(applier));
            g_rejectedGeneration = generation;
            return false;
        }
        g_moduleReference = module;
        g_target = reinterpret_cast<void*>(applier);
        g_original.store(reinterpret_cast<uintptr_t>(trampoline), std::memory_order_release);
        g_moduleBase.store(base, std::memory_order_release);
        g_generation.store(generation, std::memory_order_release);
        if (MH_EnableHook(reinterpret_cast<void*>(applier)) != MH_OK)
        {
            LOG("Halo 2 renderer switch guard WITHHELD: hook enable failed");
            (void)Remove("install rollback");
            g_rejectedGeneration = generation;
            return false;
        }
        g_installed.store(true, std::memory_order_release);
        LOG("Halo 2 renderer switch guard installed on the render-mode applier "
            "+0x%X: a Classic/Anniversary switch request is honoured only while "
            "keyboard Tab, the physical pad's Back (halo2_gamepad_graphics_switch "
            "= 1) or the mod's own head-gesture held click is present; anything "
            "else is written back and logged with the evidence",
            static_cast<unsigned>(kHalo2RenderModeApplierRva));
        return true;
    }
}

bool Halo2RenderModeGuard_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed) noexcept
{
    const bool desired = moduleBase && generation &&
        moduleSize == kHalo2RetailImageSize && activeAndRange &&
        levelRunning && coldPassed;
    const bool foreignModule = g_target &&
        (g_generation.load(std::memory_order_acquire) != generation ||
         g_moduleBase.load(std::memory_order_acquire) != moduleBase);
    if (!desired || foreignModule)
    {
        if (g_target)
            (void)Remove(foreignModule ? "module generation changed"
                : (!levelRunning ? "level liveness closed"
                                 : "title no longer eligible"));
        if (generation != g_rejectedGeneration)
            g_rejectedGeneration = 0;
        return false;
    }
    if (!g_installed.load(std::memory_order_acquire))
        return Install(moduleBase, generation);
    return true;
}

bool Halo2RenderModeGuard_Installed() noexcept
{
    return g_installed.load(std::memory_order_acquire);
}

void Halo2RenderModeGuard_DescribeLastSwitch(char* buffer, size_t bytes) noexcept
{
    if (!buffer || !bytes)
        return;
    _snprintf_s(buffer, bytes, _TRUNCATE, "%s; applier calls %llu, honoured %llu, "
                "suppressed %llu",
                g_lastSwitch,
                static_cast<unsigned long long>(g_calls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_honoured.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_suppressed.load(std::memory_order_relaxed)));
}
