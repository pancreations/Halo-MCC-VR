#include "halo2_render_probe.h"

#include <windows.h>
#include <d3d11.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>

#include "../common/halo2_render_logic.h"
#include "../common/log.h"

#ifndef HALOMCCVR_HALO2_RENDER_PROBE
#define HALOMCCVR_HALO2_RENDER_PROBE 0
#endif

#if HALOMCCVR_HALO2_RENDER_PROBE

namespace
{
    // Every signature below is the exact ABI recovered from the call sites, so
    // the detour preserves the engine's arguments. Functions whose argument
    // shape is not proven are deliberately NOT hooked: a counting detour with
    // the wrong prototype corrupts the caller's registers.
    using VoidFn = void(__fastcall*)();
    using ModeFn = void(__fastcall*)(uint8_t);
    using RenderFrameFn =
        void(__fastcall*)(uint32_t, uint32_t, uint32_t, uint32_t, void*);
    using PlayerWindowFn = void(__fastcall*)(void*, uint8_t);
    using ThisFn = void(__fastcall*)(void*);

    struct Counters
    {
        std::atomic<uint64_t> classicDriver{};      // 0x95FEC0
        std::atomic<uint64_t> setupClassic{};       // 0x960230(mode == 0)
        std::atomic<uint64_t> setupAlternate{};     // 0x960230(mode != 0)
        std::atomic<uint64_t> renderFrame{};        // 0x7E1600
        std::atomic<uint64_t> playerWindow{};       // 0x7E2130
        std::atomic<uint64_t> saberBridge{};        // 0x69540
        std::atomic<uint64_t> saberFrame{};         // 0x2DC3D0
        std::atomic<uint64_t> saberScene{};         // 0x2DF190
    };

    Counters g_counts;
    Counters g_lastReported;

    std::atomic<bool> g_installed{false};
    std::atomic<uint32_t> g_generation{0};
    std::atomic<uintptr_t> g_moduleBase{0};
    std::atomic<uint32_t> g_activeCallbacks{0};
    std::atomic<uint32_t> g_censusRemaining{3};

    std::atomic<uintptr_t> g_originalClassicDriver{0};
    std::atomic<uintptr_t> g_originalSetup{0};
    std::atomic<uintptr_t> g_originalRenderFrame{0};
    std::atomic<uintptr_t> g_originalPlayerWindow{0};
    std::atomic<uintptr_t> g_originalSaberBridge{0};
    std::atomic<uintptr_t> g_originalSaberFrame{0};
    std::atomic<uintptr_t> g_originalSaberScene{0};

    uint64_t g_lastReportMs = 0;
    uint32_t g_armedLoggedGeneration = 0;
    uint32_t g_rejectedGeneration = 0;

    struct Target
    {
        uint32_t rva;
        const char* name;
        void* detour;
        std::atomic<uintptr_t>* original;
        void* installed;
    };

    // Describes one render-target pointer slot for the D3D census.
    struct SlotDescription
    {
        uint32_t rva;
        const char* name;
    };
    constexpr SlotDescription kCensusSlots[] = {
        {0x0197EE58, "backbuffer RTV slot 0x197EE58"},
        {0x0197EE60, "primary scene RTV slot 0x197EE60"},
        {0x0197EE88, "resolved scene RTV slot 0x197EE88"},
    };

    bool ReadPointer(uintptr_t address, uintptr_t& value) noexcept
    {
        __try
        {
            value = *reinterpret_cast<const volatile uintptr_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Resolves one RTV pointer to its underlying Texture2D and reports the
    // dimensions and format. This is what identifies the resource a per-eye
    // capture would have to read; it runs at most a few times per session.
    void DescribeSlot(uintptr_t base, const SlotDescription& slot) noexcept
    {
        uintptr_t viewPointer = 0;
        if (!ReadPointer(base + slot.rva, viewPointer))
        {
            LOG("H2PROBE   %s: unreadable", slot.name);
            return;
        }
        if (!viewPointer)
        {
            LOG("H2PROBE   %s: NULL", slot.name);
            return;
        }
        __try
        {
            auto* view = reinterpret_cast<ID3D11RenderTargetView*>(viewPointer);
            ID3D11Resource* resource = nullptr;
            view->GetResource(&resource);
            if (!resource)
            {
                LOG("H2PROBE   %s: view %p has no resource",
                    slot.name, reinterpret_cast<void*>(viewPointer));
                return;
            }
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(resource->QueryInterface(
                    __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&texture))) &&
                texture)
            {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                LOG("H2PROBE   %s: view %p tex %p %ux%u fmt %u samples %u "
                    "mips %u bind 0x%X usage %u",
                    slot.name, reinterpret_cast<void*>(viewPointer),
                    reinterpret_cast<void*>(texture), desc.Width, desc.Height,
                    static_cast<unsigned>(desc.Format),
                    desc.SampleDesc.Count, desc.MipLevels,
                    desc.BindFlags, static_cast<unsigned>(desc.Usage));
                texture->Release();
            }
            else
            {
                LOG("H2PROBE   %s: view %p resource %p is not a Texture2D",
                    slot.name, reinterpret_cast<void*>(viewPointer),
                    reinterpret_cast<void*>(resource));
            }
            resource->Release();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG("H2PROBE   %s: exception while describing the view",
                slot.name);
        }
    }

    // Called from inside the alternate-setup detour, i.e. while the Saber
    // bridge 0x69540 has its host-owned render target installed in the slots.
    // That window is exactly where a per-eye capture would read.
    void RunTargetCensus(uintptr_t base) noexcept
    {
        uint32_t remaining = g_censusRemaining.load(std::memory_order_acquire);
        while (remaining)
        {
            if (g_censusRemaining.compare_exchange_weak(
                    remaining, remaining - 1, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                break;
            }
        }
        if (!remaining)
            return;
        LOG("H2PROBE target census (inside 0x960230 with mode != 0, i.e. "
            "inside the 0x69540 host-render-target window):");
        for (const SlotDescription& slot : kCensusSlots)
            DescribeSlot(base, slot);
    }

    struct CallbackScope
    {
        CallbackScope() noexcept
        {
            g_activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        }
        ~CallbackScope() noexcept
        {
            g_activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        }
    };

    void __fastcall ClassicDriverDetour()
    {
        CallbackScope scope;
        const auto original =
            reinterpret_cast<VoidFn>(
                g_originalClassicDriver.load(std::memory_order_acquire));
        g_counts.classicDriver.fetch_add(1, std::memory_order_relaxed);
        if (original)
            original();
    }

    void __fastcall SetupDetour(uint8_t mode)
    {
        CallbackScope scope;
        const auto original =
            reinterpret_cast<ModeFn>(
                g_originalSetup.load(std::memory_order_acquire));
        if (mode)
        {
            g_counts.setupAlternate.fetch_add(1, std::memory_order_relaxed);
            const uintptr_t base =
                g_moduleBase.load(std::memory_order_acquire);
            if (base && g_censusRemaining.load(std::memory_order_acquire))
                RunTargetCensus(base);
        }
        else
        {
            g_counts.setupClassic.fetch_add(1, std::memory_order_relaxed);
        }
        if (original)
            original(mode);
    }

    void __fastcall RenderFrameDetour(
        uint32_t a, uint32_t b, uint32_t c, uint32_t d, void* windows)
    {
        CallbackScope scope;
        const auto original =
            reinterpret_cast<RenderFrameFn>(
                g_originalRenderFrame.load(std::memory_order_acquire));
        g_counts.renderFrame.fetch_add(1, std::memory_order_relaxed);
        if (original)
            original(a, b, c, d, windows);
    }

    void __fastcall PlayerWindowDetour(void* window, uint8_t flag)
    {
        CallbackScope scope;
        const auto original =
            reinterpret_cast<PlayerWindowFn>(
                g_originalPlayerWindow.load(std::memory_order_acquire));
        g_counts.playerWindow.fetch_add(1, std::memory_order_relaxed);
        if (original)
            original(window, flag);
    }

    void __fastcall SaberBridgeDetour(void* self)
    {
        CallbackScope scope;
        const auto original =
            reinterpret_cast<ThisFn>(
                g_originalSaberBridge.load(std::memory_order_acquire));
        g_counts.saberBridge.fetch_add(1, std::memory_order_relaxed);
        if (original)
            original(self);
    }

    void __fastcall SaberFrameDetour(void* job)
    {
        CallbackScope scope;
        const auto original =
            reinterpret_cast<ThisFn>(
                g_originalSaberFrame.load(std::memory_order_acquire));
        g_counts.saberFrame.fetch_add(1, std::memory_order_relaxed);
        if (original)
            original(job);
    }

    void __fastcall SaberSceneDetour(void* view)
    {
        CallbackScope scope;
        const auto original =
            reinterpret_cast<ThisFn>(
                g_originalSaberScene.load(std::memory_order_acquire));
        g_counts.saberScene.fetch_add(1, std::memory_order_relaxed);
        if (original)
            original(view);
    }

    Target g_targets[] = {
        {0x0095FEC0, "classic driver 0x95FEC0",
         reinterpret_cast<void*>(&ClassicDriverDetour),
         &g_originalClassicDriver, nullptr},
        {0x00960230, "setup dispatcher 0x960230",
         reinterpret_cast<void*>(&SetupDetour), &g_originalSetup, nullptr},
        {0x007E1600, "render_frame 0x7E1600",
         reinterpret_cast<void*>(&RenderFrameDetour),
         &g_originalRenderFrame, nullptr},
        {0x007E2130, "render_player_window 0x7E2130",
         reinterpret_cast<void*>(&PlayerWindowDetour),
         &g_originalPlayerWindow, nullptr},
        {0x00069540, "saber->blam bridge 0x69540",
         reinterpret_cast<void*>(&SaberBridgeDetour),
         &g_originalSaberBridge, nullptr},
        {0x002DC3D0, "saber frame render 0x2DC3D0",
         reinterpret_cast<void*>(&SaberFrameDetour),
         &g_originalSaberFrame, nullptr},
        {0x002DF190, "saber scene render 0x2DF190",
         reinterpret_cast<void*>(&SaberSceneDetour),
         &g_originalSaberScene, nullptr},
    };

    bool IsExecutable(uintptr_t address) noexcept
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(
                reinterpret_cast<const void*>(address), &info, sizeof(info)))
        {
            return false;
        }
        if (info.State != MEM_COMMIT)
            return false;
        const DWORD mask = info.Protect & 0xFF;
        return !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mask == PAGE_EXECUTE_READ || mask == PAGE_EXECUTE_READWRITE ||
             mask == PAGE_EXECUTE_WRITECOPY || mask == PAGE_EXECUTE);
    }

    void RemoveAll() noexcept
    {
        for (Target& target : g_targets)
        {
            if (!target.installed)
                continue;
            (void)MH_DisableHook(target.installed);
        }
        for (int attempt = 0;
             attempt < 200 &&
             g_activeCallbacks.load(std::memory_order_acquire);
             ++attempt)
        {
            Sleep(10);
        }
        for (Target& target : g_targets)
        {
            if (!target.installed)
                continue;
            (void)MH_RemoveHook(target.installed);
            target.installed = nullptr;
            target.original->store(0, std::memory_order_release);
        }
        g_installed.store(false, std::memory_order_release);
        g_moduleBase.store(0, std::memory_order_release);
        g_generation.store(0, std::memory_order_release);
    }

    bool InstallAll(uintptr_t base, uint32_t generation) noexcept
    {
        if (g_rejectedGeneration == generation)
            return false;
        unsigned installedCount = 0;
        for (Target& target : g_targets)
        {
            const uintptr_t address = base + target.rva;
            if (!IsExecutable(address))
            {
                LOG("H2PROBE: %s is not executable at RVA 0x%X; that target "
                    "stays stock", target.name, target.rva);
                continue;
            }
            void* trampoline = nullptr;
            const MH_STATUS created = MH_CreateHook(
                reinterpret_cast<void*>(address), target.detour, &trampoline);
            if (created != MH_OK || !trampoline)
            {
                LOG("H2PROBE: %s create=%d; that target stays stock",
                    target.name, static_cast<int>(created));
                if (created == MH_OK)
                    (void)MH_RemoveHook(reinterpret_cast<void*>(address));
                continue;
            }
            const MH_STATUS enabled =
                MH_EnableHook(reinterpret_cast<void*>(address));
            if (enabled != MH_OK)
            {
                LOG("H2PROBE: %s enable=%d; that target stays stock",
                    target.name, static_cast<int>(enabled));
                (void)MH_RemoveHook(reinterpret_cast<void*>(address));
                continue;
            }
            target.installed = reinterpret_cast<void*>(address);
            target.original->store(
                reinterpret_cast<uintptr_t>(trampoline),
                std::memory_order_release);
            ++installedCount;
        }
        if (!installedCount)
        {
            g_rejectedGeneration = generation;
            return false;
        }
        g_moduleBase.store(base, std::memory_order_release);
        g_generation.store(generation, std::memory_order_release);
        g_censusRemaining.store(3, std::memory_order_release);
        g_installed.store(true, std::memory_order_release);
        LOG("H2PROBE installed %u counting detours; every one calls the "
            "engine original and writes nothing", installedCount);
        return true;
    }

    void Report(uintptr_t base) noexcept
    {
        const uint64_t now = GetTickCount64();
        if (now - g_lastReportMs < 1000)
            return;
        g_lastReportMs = now;

        const uint64_t classicDriver =
            g_counts.classicDriver.load(std::memory_order_relaxed);
        const uint64_t setupClassic =
            g_counts.setupClassic.load(std::memory_order_relaxed);
        const uint64_t setupAlternate =
            g_counts.setupAlternate.load(std::memory_order_relaxed);
        const uint64_t renderFrame =
            g_counts.renderFrame.load(std::memory_order_relaxed);
        const uint64_t playerWindow =
            g_counts.playerWindow.load(std::memory_order_relaxed);
        const uint64_t saberBridge =
            g_counts.saberBridge.load(std::memory_order_relaxed);
        const uint64_t saberFrame =
            g_counts.saberFrame.load(std::memory_order_relaxed);
        const uint64_t saberScene =
            g_counts.saberScene.load(std::memory_order_relaxed);

        if (classicDriver ==
                g_lastReported.classicDriver.load(std::memory_order_relaxed) &&
            setupClassic ==
                g_lastReported.setupClassic.load(std::memory_order_relaxed) &&
            setupAlternate ==
                g_lastReported.setupAlternate.load(std::memory_order_relaxed) &&
            renderFrame ==
                g_lastReported.renderFrame.load(std::memory_order_relaxed) &&
            playerWindow ==
                g_lastReported.playerWindow.load(std::memory_order_relaxed) &&
            saberBridge ==
                g_lastReported.saberBridge.load(std::memory_order_relaxed) &&
            saberFrame ==
                g_lastReported.saberFrame.load(std::memory_order_relaxed) &&
            saberScene ==
                g_lastReported.saberScene.load(std::memory_order_relaxed))
        {
            return;
        }
        g_lastReported.classicDriver.store(
            classicDriver, std::memory_order_relaxed);
        g_lastReported.setupClassic.store(
            setupClassic, std::memory_order_relaxed);
        g_lastReported.setupAlternate.store(
            setupAlternate, std::memory_order_relaxed);
        g_lastReported.renderFrame.store(
            renderFrame, std::memory_order_relaxed);
        g_lastReported.playerWindow.store(
            playerWindow, std::memory_order_relaxed);
        g_lastReported.saberBridge.store(
            saberBridge, std::memory_order_relaxed);
        g_lastReported.saberFrame.store(saberFrame, std::memory_order_relaxed);
        g_lastReported.saberScene.store(saberScene, std::memory_order_relaxed);

        uintptr_t modeByte = 0;
        uintptr_t modeDword = 0;
        (void)ReadPointer(base + kHalo2ClassicRenderDisabledByteRva, modeByte);
        (void)ReadPointer(base + kHalo2AppliedRenderModeRva, modeDword);

        float observer[3]{};
        bool observerRead = false;
        __try
        {
            const auto* position = reinterpret_cast<const volatile float*>(
                base + kHalo2ObserverResultArrayRva +
                kHalo2ObserverResultPositionOffset);
            observer[0] = position[0];
            observer[1] = position[1];
            observer[2] = position[2];
            observerRead = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            observerRead = false;
        }

        LOG("H2PROBE counts: classicDriver=%llu setup(classic)=%llu "
            "setup(alternate)=%llu render_frame=%llu player_window=%llu "
            "saberBridge=%llu saberFrame=%llu saberScene=%llu | mode byte=%u "
            "dword=%u | observer0 pos=%s(%.3f, %.3f, %.3f)",
            static_cast<unsigned long long>(classicDriver),
            static_cast<unsigned long long>(setupClassic),
            static_cast<unsigned long long>(setupAlternate),
            static_cast<unsigned long long>(renderFrame),
            static_cast<unsigned long long>(playerWindow),
            static_cast<unsigned long long>(saberBridge),
            static_cast<unsigned long long>(saberFrame),
            static_cast<unsigned long long>(saberScene),
            static_cast<unsigned>(modeByte & 0xFF),
            static_cast<unsigned>(modeDword & 0xFFFFFFFF),
            observerRead ? "" : "UNREADABLE ",
            observer[0], observer[1], observer[2]);
    }
}

bool Halo2RenderProbe_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed) noexcept
{
    const bool desired = moduleBase && generation &&
        moduleSize == kHalo2RetailImageSize && activeAndRange && coldPassed;
    (void)levelRunning;

    const uint32_t owned = g_generation.load(std::memory_order_acquire);
    const bool ownsDifferentModule =
        g_installed.load(std::memory_order_acquire) &&
        (owned != generation ||
         g_moduleBase.load(std::memory_order_acquire) != moduleBase);

    if (!desired || ownsDifferentModule)
    {
        if (g_installed.load(std::memory_order_acquire))
            RemoveAll();
        if (generation != g_rejectedGeneration)
            g_rejectedGeneration = 0;
        return false;
    }

    if (!g_installed.load(std::memory_order_acquire))
    {
        if (!InstallAll(moduleBase, generation))
            return false;
        if (g_armedLoggedGeneration != generation)
        {
            g_armedLoggedGeneration = generation;
            LOG("H2PROBE armed for halo2.dll generation %u. This build is a "
                "diagnostic: it counts which render entries execute and "
                "describes the render targets that are live inside the "
                "host-render-target window. It changes no behavior",
                generation);
        }
    }
    Report(moduleBase);
    return true;
}

bool Halo2RenderProbe_Installed() noexcept
{
    return g_installed.load(std::memory_order_acquire);
}

#else

bool Halo2RenderProbe_Poll(
    uintptr_t, size_t, uint32_t, bool, bool, bool) noexcept
{
    return false;
}

bool Halo2RenderProbe_Installed() noexcept { return false; }

#endif
