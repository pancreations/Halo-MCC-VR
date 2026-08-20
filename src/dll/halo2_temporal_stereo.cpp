#include "halo2_temporal_stereo.h"

#include <windows.h>

#include <MinHook.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <intrin.h>

#include "../common/config.h"
#include "../common/halo2_render_logic.h"
#include "../common/log.h"
#include "vr.h"

#ifndef HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO
#define HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO 0
#endif

#if HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO

#pragma intrinsic(_ReturnAddress)

namespace
{
    constexpr uint64_t kTelemetryPeriodMs = 2000;
    constexpr unsigned kCallbackDrainLimitMs = 2000;

    static_assert(std::atomic<uint64_t>::is_always_lock_free);

    enum class CoreState : uint8_t
    {
        StockFallback = 0,
        CleanupRequired,
        Installed,
    };

    struct HookTelemetry
    {
        std::atomic<uint64_t> callbacks{};
        std::atomic<uint64_t> stockPasses{};
        std::atomic<uint64_t> missingSnapshots{};
        std::atomic<uint64_t> foreignCallers{};
        std::atomic<uint64_t> invalidFlags{};
        std::atomic<uint64_t> splitWindows{};
        std::atomic<uint64_t> duplicateFrames{};
        std::atomic<uint64_t> invalidCameras{};
        std::atomic<uint64_t> claimedFrames{};
        std::atomic<uint64_t> completedFrames{};
        std::atomic<uint64_t> rejectedCompletions{};
        std::atomic<uint64_t> transactionExceptions{};
    };

    struct TelemetrySnapshot
    {
        uint64_t callbacks = 0;
        uint64_t stockPasses = 0;
        uint64_t missingSnapshots = 0;
        uint64_t foreignCallers = 0;
        uint64_t invalidFlags = 0;
        uint64_t splitWindows = 0;
        uint64_t duplicateFrames = 0;
        uint64_t invalidCameras = 0;
        uint64_t claimedFrames = 0;
        uint64_t completedFrames = 0;
        uint64_t rejectedCompletions = 0;
        uint64_t transactionExceptions = 0;
    };

    HookTelemetry g_telemetry;
    TelemetrySnapshot g_lastTelemetry{};
    uint64_t g_lastTelemetryMs = 0;

    std::atomic<bool> g_installed{false};
    std::atomic<bool> g_armed{false};
    std::atomic<uint32_t> g_generation{0};
    std::atomic<uintptr_t> g_moduleBase{0};
    std::atomic<uintptr_t> g_originalAddress{0};
    std::atomic<uint32_t> g_activeCallbacks{0};
    std::atomic<uint64_t> g_seenPrimarySerial{0};
    std::atomic<uint64_t> g_rejectedSerial{0};
    std::atomic<bool> g_stereoRequested{false};
    std::atomic<bool> g_levelLive{false};
    std::atomic<bool> g_coldObservationPassed{false};
    std::atomic<bool> g_teardownRequested{false};
    std::atomic<bool> g_rightEyeFirst{false};
    std::atomic<uint32_t> g_vrFailureGeneration{0};

    HMODULE g_moduleReference = nullptr;
    void* g_hookTarget = nullptr;
    CoreState g_coreState = CoreState::StockFallback;
    uint32_t g_rejectedGeneration = 0;
    uint32_t g_pinFailureLoggedGeneration = 0;

    TelemetrySnapshot ReadTelemetry() noexcept
    {
        TelemetrySnapshot out{};
        out.callbacks = g_telemetry.callbacks.load(std::memory_order_relaxed);
        out.stockPasses =
            g_telemetry.stockPasses.load(std::memory_order_relaxed);
        out.missingSnapshots =
            g_telemetry.missingSnapshots.load(std::memory_order_relaxed);
        out.foreignCallers =
            g_telemetry.foreignCallers.load(std::memory_order_relaxed);
        out.invalidFlags =
            g_telemetry.invalidFlags.load(std::memory_order_relaxed);
        out.splitWindows =
            g_telemetry.splitWindows.load(std::memory_order_relaxed);
        out.duplicateFrames =
            g_telemetry.duplicateFrames.load(std::memory_order_relaxed);
        out.invalidCameras =
            g_telemetry.invalidCameras.load(std::memory_order_relaxed);
        out.claimedFrames =
            g_telemetry.claimedFrames.load(std::memory_order_relaxed);
        out.completedFrames =
            g_telemetry.completedFrames.load(std::memory_order_relaxed);
        out.rejectedCompletions =
            g_telemetry.rejectedCompletions.load(std::memory_order_relaxed);
        out.transactionExceptions =
            g_telemetry.transactionExceptions.load(std::memory_order_relaxed);
        return out;
    }

    bool TelemetryChanged(
        const TelemetrySnapshot& left, const TelemetrySnapshot& right) noexcept
    {
        return left.callbacks != right.callbacks ||
            left.stockPasses != right.stockPasses ||
            left.missingSnapshots != right.missingSnapshots ||
            left.foreignCallers != right.foreignCallers ||
            left.invalidFlags != right.invalidFlags ||
            left.splitWindows != right.splitWindows ||
            left.duplicateFrames != right.duplicateFrames ||
            left.invalidCameras != right.invalidCameras ||
            left.claimedFrames != right.claimedFrames ||
            left.completedFrames != right.completedFrames ||
            left.rejectedCompletions != right.rejectedCompletions ||
            left.transactionExceptions != right.transactionExceptions;
    }

    void ReportTelemetry() noexcept
    {
        if (!g_installed.load(std::memory_order_acquire))
            return;
        const uint64_t now = GetTickCount64();
        if (g_lastTelemetryMs && now - g_lastTelemetryMs < kTelemetryPeriodMs)
            return;
        const TelemetrySnapshot current = ReadTelemetry();
        if (TelemetryChanged(current, g_lastTelemetry))
        {
            LOG("Halo 2 C-H2-2 hook: callbacks=%llu stock=%llu snapshotMiss=%llu "
                "foreign=%llu badFlag=%llu split=%llu duplicate=%llu "
                "invalidCamera=%llu "
                "claimed=%llu complete=%llu completionReject=%llu exception=%llu",
                static_cast<unsigned long long>(current.callbacks),
                static_cast<unsigned long long>(current.stockPasses),
                static_cast<unsigned long long>(current.missingSnapshots),
                static_cast<unsigned long long>(current.foreignCallers),
                static_cast<unsigned long long>(current.invalidFlags),
                static_cast<unsigned long long>(current.splitWindows),
                static_cast<unsigned long long>(current.duplicateFrames),
                static_cast<unsigned long long>(current.invalidCameras),
                static_cast<unsigned long long>(current.claimedFrames),
                static_cast<unsigned long long>(current.completedFrames),
                static_cast<unsigned long long>(current.rejectedCompletions),
                static_cast<unsigned long long>(current.transactionExceptions));
            g_lastTelemetry = current;
        }
        g_lastTelemetryMs = now;
    }

    Halo2RenderPlayerWindowFn OriginalPlayerWindow() noexcept
    {
        return reinterpret_cast<Halo2RenderPlayerWindowFn>(
            g_originalAddress.load(std::memory_order_acquire));
    }

    void RejectTemporalFrame(uint32_t generation, uint64_t serial) noexcept
    {
        if (!generation || !serial)
            return;
        uint64_t observed =
            g_rejectedSerial.load(std::memory_order_acquire);
        while (observed < serial &&
               !g_rejectedSerial.compare_exchange_weak(
                   observed, serial, std::memory_order_acq_rel,
                   std::memory_order_acquire))
        {
        }
        VR_InvalidateHalo2TemporalFrame(generation, serial);
    }

    void CallOriginalUnmodified(
        Halo2TemporalTransactionAction action, void* window, uint8_t flag,
        uint32_t rejectionGeneration, uint64_t serial) noexcept
    {
        if (action == Halo2TemporalTransactionAction::
                          RejectTemporalFrameAndCallStockOnce)
        {
            RejectTemporalFrame(rejectionGeneration, serial);
        }

        Halo2TemporalTransactionResult result{};
        if (const Halo2RenderPlayerWindowFn original = OriginalPlayerWindow())
        {
            ++result.originalCalls;
            g_telemetry.stockPasses.fetch_add(1, std::memory_order_relaxed);
            original(window, flag);
            result.originalReturned = true;
        }

        // There is nothing to publish on either unmodified path. Still run the
        // pure result proof so a future edit cannot silently turn one of these
        // branches into a second original call or an engine-camera write.
        if (!Halo2TemporalTransactionResultMatches(action, result) &&
            action == Halo2TemporalTransactionAction::
                          RejectTemporalFrameAndCallStockOnce)
        {
            RejectTemporalFrame(rejectionGeneration, serial);
        }
    }

    bool ReadPlayerIndex(void* window, int32_t& playerIndex) noexcept
    {
        if (!window)
            return false;
        __try
        {
            std::memcpy(
                &playerIndex,
                static_cast<const uint8_t*>(window) +
                    kHalo2WindowPlayerIndexOffset,
                sizeof(playerIndex));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    // Implemented below after the worker-only proof/cleanup machinery. Keeping
    // the hot callback in its own routine lets the entry wrapper bracket every
    // path with one callback counter and no C++ lock or allocation.
    void Halo2PlayerWindowDetourBody(
        void* window, uint8_t flag, uintptr_t callerReturn) noexcept;

    __declspec(noinline) void __fastcall Halo2PlayerWindowDetour(
        void* window, uint8_t flag)
    {
        const uintptr_t callerReturn =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        g_activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        __try
        {
            Halo2PlayerWindowDetourBody(window, flag, callerReturn);
        }
        __finally
        {
            g_activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    bool IsReadableProtection(DWORD protection) noexcept
    {
        if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        switch (protection & 0xFFu)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    bool IsExecutableProtection(DWORD protection) noexcept
    {
        if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        switch (protection & 0xFFu)
        {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    bool VerifyHookTargetExecutable(uintptr_t base, size_t size) noexcept
    {
        if (!base || base > UINTPTR_MAX - size ||
            kHalo2RetailRenderPlayerWindowRva >= size)
        {
            return false;
        }
        const uintptr_t target =
            base + kHalo2RetailRenderPlayerWindowRva;
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(
                reinterpret_cast<const void*>(target), &info,
                sizeof(info)) ||
            info.State != MEM_COMMIT ||
            reinterpret_cast<uintptr_t>(info.AllocationBase) != base ||
            !IsExecutableProtection(info.Protect))
        {
            return false;
        }
        const uintptr_t regionBase =
            reinterpret_cast<uintptr_t>(info.BaseAddress);
        return regionBase <= target &&
            regionBase <= UINTPTR_MAX - info.RegionSize &&
            target < regionBase + info.RegionSize;
    }

    constexpr std::array<uint8_t, 23> kPlayerWindowAnchorBytes = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x81, 0xEC, 0x00, 0x02, 0x00, 0x00,
    };

    bool CountPlayerWindowAnchors(
        uintptr_t base, size_t size, uintptr_t& first,
        uint32_t& count) noexcept
    {
        first = 0;
        count = 0;
        if (!base || size < kPlayerWindowAnchorBytes.size() ||
            base > UINTPTR_MAX - size)
        {
            return false;
        }

        const uintptr_t imageEnd = base + size;
        uintptr_t cursor = base;
        __try
        {
            while (cursor < imageEnd)
            {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(
                        reinterpret_cast<const void*>(cursor), &info,
                        sizeof(info)) ||
                    !info.RegionSize)
                {
                    return false;
                }
                const uintptr_t regionBase =
                    reinterpret_cast<uintptr_t>(info.BaseAddress);
                if (regionBase > UINTPTR_MAX - info.RegionSize)
                    return false;
                const uintptr_t regionEnd =
                    (regionBase + info.RegionSize < imageEnd)
                        ? regionBase + info.RegionSize
                        : imageEnd;
                if (regionEnd <= cursor)
                    return false;

                if (info.State == MEM_COMMIT &&
                    IsReadableProtection(info.Protect) &&
                    regionEnd - cursor >= kPlayerWindowAnchorBytes.size())
                {
                    const auto* bytes =
                        reinterpret_cast<const uint8_t*>(cursor);
                    const size_t last = static_cast<size_t>(
                        regionEnd - cursor - kPlayerWindowAnchorBytes.size());
                    for (size_t offset = 0; offset <= last; ++offset)
                    {
                        if (bytes[offset] != kPlayerWindowAnchorBytes[0] ||
                            std::memcmp(
                                bytes + offset, kPlayerWindowAnchorBytes.data(),
                                kPlayerWindowAnchorBytes.size()) != 0)
                        {
                            continue;
                        }
                        if (!first)
                            first = cursor + offset;
                        if (++count >= 2)
                            return true;
                    }
                }
                cursor = regionEnd;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            first = 0;
            count = 0;
            return false;
        }
        return true;
    }

    bool VerifyPeIdentity(uintptr_t base, size_t size) noexcept
    {
        if (!base || size != kHalo2RetailImageSize)
            return false;
        __try
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
                static_cast<size_t>(dos->e_lfanew) >
                    size - sizeof(IMAGE_NT_HEADERS64))
            {
                return false;
            }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                base + static_cast<uint32_t>(dos->e_lfanew));
            return nt->Signature == IMAGE_NT_SIGNATURE &&
                nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                nt->FileHeader.TimeDateStamp == kHalo2RetailPeTimestamp &&
                nt->OptionalHeader.SizeOfImage == kHalo2RetailImageSize;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool VerifyCallEdge(uintptr_t base, size_t size) noexcept
    {
        if (!base || base > UINTPTR_MAX - size ||
            kHalo2RetailRenderPlayerWindowCallRva + 5 > size)
        {
            return false;
        }
        __try
        {
            const auto* call = reinterpret_cast<const uint8_t*>(
                base + kHalo2RetailRenderPlayerWindowCallRva);
            if (call[0] != 0xE8)
                return false;
            int32_t displacement = 0;
            std::memcpy(&displacement, call + 1, sizeof(displacement));
            const uintptr_t returnAddress =
                base + kHalo2RetailRenderPlayerWindowReturnRva;
            return static_cast<int64_t>(returnAddress) + displacement ==
                static_cast<int64_t>(
                    base + kHalo2RetailRenderPlayerWindowRva);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool WaitForCallbacks() noexcept
    {
        for (unsigned waited = 0; waited < kCallbackDrainLimitMs; ++waited)
        {
            if (g_activeCallbacks.load(std::memory_order_acquire) == 0)
                return true;
            Sleep(1);
        }
        LOG("Halo 2 C-H2-2 cleanup: active callbacks did not drain to "
            "quiescence; retaining the hook, trampoline, and module pin");
        return false;
    }

    bool DisableStatusIsSafe(MH_STATUS status) noexcept
    {
        return status == MH_OK || status == MH_ERROR_DISABLED ||
            status == MH_ERROR_NOT_CREATED;
    }

    void ClearRuntimeState() noexcept
    {
        HMODULE module = g_moduleReference;
        g_moduleReference = nullptr;
        g_hookTarget = nullptr;
        g_originalAddress.store(0, std::memory_order_release);
        g_moduleBase.store(0, std::memory_order_release);
        g_generation.store(0, std::memory_order_release);
        g_seenPrimarySerial.store(0, std::memory_order_release);
        g_rejectedSerial.store(0, std::memory_order_release);
        g_installed.store(false, std::memory_order_release);
        g_armed.store(false, std::memory_order_release);
        g_coreState = CoreState::StockFallback;
        if (module)
            FreeLibrary(module);
    }

    bool RemoveCore(const char* reason) noexcept
    {
        g_teardownRequested.store(true, std::memory_order_release);
        g_armed.store(false, std::memory_order_release);
        // The VR side is atomic-only here. Cancel any completion publication
        // racing teardown and revoke both generation/serial cache stamps before
        // this module generation can close or be replaced.
        VR_ResetHalo2TemporalStereo();
        if (!g_hookTarget)
        {
            ClearRuntimeState();
            return true;
        }

        const MH_STATUS disabled = MH_DisableHook(g_hookTarget);
        if (!DisableStatusIsSafe(disabled))
        {
            g_coreState = CoreState::CleanupRequired;
            LOG("Halo 2 C-H2-2 cleanup: disable failed (%d) after %s; "
                "retaining ownership for worker retry",
                static_cast<int>(disabled), reason);
            return false;
        }
        if (!WaitForCallbacks())
        {
            g_coreState = CoreState::CleanupRequired;
            return false;
        }

        const MH_STATUS removed = MH_RemoveHook(g_hookTarget);
        if (removed != MH_OK && removed != MH_ERROR_NOT_CREATED)
        {
            g_coreState = CoreState::CleanupRequired;
            LOG("Halo 2 C-H2-2 cleanup: removal failed (%d) after %s; "
                "retaining ownership for worker retry",
                static_cast<int>(removed), reason);
            return false;
        }

        const uint32_t oldGeneration =
            g_generation.load(std::memory_order_acquire);
        ClearRuntimeState();
        LOG("Halo 2 C-H2-2 position-only temporal hook removed (generation "
            "%u; %s); Halo 2 is stock again",
            oldGeneration, reason);
        return true;
    }

    bool AcquireModulePin(uintptr_t base, HMODULE& module) noexcept
    {
        module = nullptr;
        if (!base || !GetModuleHandleExW(
                         GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                         reinterpret_cast<LPCWSTR>(base), &module) ||
            reinterpret_cast<uintptr_t>(module) != base ||
            GetModuleHandleW(L"halo2.dll") != module)
        {
            if (module)
                FreeLibrary(module);
            module = nullptr;
            return false;
        }
        return true;
    }

    bool InstallCore(
        uintptr_t base, size_t size, uint32_t generation) noexcept
    {
        HMODULE module = nullptr;
        if (!AcquireModulePin(base, module))
        {
            if (g_pinFailureLoggedGeneration != generation)
            {
                g_pinFailureLoggedGeneration = generation;
                LOG("Halo 2 C-H2-2 install: could not retain the exact "
                    "halo2.dll module; optional hook remains stock and will "
                    "retry");
            }
            return false;
        }

        uintptr_t uniqueHit = 0;
        uint32_t matchCount = 0;
        const bool peIdentity = VerifyPeIdentity(base, size);
        const bool anchorScan = peIdentity &&
            CountPlayerWindowAnchors(base, size, uniqueHit, matchCount);
        const bool callEdge = VerifyCallEdge(base, size);
        const bool executableTarget =
            VerifyHookTargetExecutable(base, size);
        const bool mappingStable = GetModuleHandleW(L"halo2.dll") == module;
        const bool proof = peIdentity && anchorScan && matchCount == 1 &&
            uniqueHit == base + kHalo2RetailRenderPlayerWindowRva &&
            callEdge && executableTarget && mappingStable;
        if (!proof)
        {
            LOG("Halo 2 C-H2-2 install WITHHELD: PE/range=%d anchorCount=%u "
                "anchorRva=0x%zX expected=0x%X callEdge=%d executable=%d "
                "mappingStable=%d; no hook or camera write occurred",
                peIdentity ? 1 : 0, matchCount,
                uniqueHit ? uniqueHit - base : 0,
                static_cast<unsigned>(
                    kHalo2RetailRenderPlayerWindowRva),
                callEdge ? 1 : 0, executableTarget ? 1 : 0,
                mappingStable ? 1 : 0);
            FreeLibrary(module);
            g_rejectedGeneration = generation;
            return false;
        }
        if (g_hookTarget || g_moduleReference ||
            g_originalAddress.load(std::memory_order_acquire))
        {
            LOG("Halo 2 C-H2-2 install WITHHELD: prior hook ownership was not "
                "cleared; worker cleanup must finish first");
            FreeLibrary(module);
            g_coreState = CoreState::CleanupRequired;
            return false;
        }
        void* const target =
            reinterpret_cast<void*>(
                base + kHalo2RetailRenderPlayerWindowRva);
        void* trampoline = nullptr;
        const MH_STATUS created = MH_CreateHook(
            target, reinterpret_cast<void*>(&Halo2PlayerWindowDetour),
            &trampoline);
        if (created != MH_OK || !trampoline)
        {
            MH_STATUS rollback = MH_ERROR_NOT_CREATED;
            if (created == MH_OK)
                rollback = MH_RemoveHook(target);
            if (created == MH_OK && rollback != MH_OK &&
                rollback != MH_ERROR_NOT_CREATED)
            {
                // MinHook owns a dormant record even though it supplied no
                // callable trampoline. Retain the module and target so the
                // worker can retry removal without an unload/use-after-free.
                g_moduleReference = module;
                g_hookTarget = target;
                g_moduleBase.store(base, std::memory_order_release);
                g_generation.store(generation, std::memory_order_release);
                g_installed.store(true, std::memory_order_release);
                g_teardownRequested.store(true, std::memory_order_release);
                g_coreState = CoreState::CleanupRequired;
                g_rejectedGeneration = generation;
                LOG("Halo 2 C-H2-2 install: MH_CreateHook returned no "
                    "trampoline and rollback failed (%d); retaining the "
                    "module pin and dormant hook record for worker retry",
                    static_cast<int>(rollback));
                return false;
            }
            LOG("Halo 2 C-H2-2 install: MH_CreateHook failed (%d, rollback "
                "%d); temporal stereo stays stock for generation %u",
                static_cast<int>(created), static_cast<int>(rollback),
                generation);
            FreeLibrary(module);
            g_rejectedGeneration = generation;
            return false;
        }

        g_moduleReference = module;
        g_hookTarget = target;
        g_originalAddress.store(
            reinterpret_cast<uintptr_t>(trampoline), std::memory_order_release);
        g_moduleBase.store(base, std::memory_order_release);
        g_generation.store(generation, std::memory_order_release);
        g_seenPrimarySerial.store(0, std::memory_order_release);
        g_rejectedSerial.store(0, std::memory_order_release);
        g_teardownRequested.store(false, std::memory_order_release);
        g_installed.store(true, std::memory_order_release);
        g_coreState = CoreState::CleanupRequired;

        const MH_STATUS enabled = MH_EnableHook(target);
        if (enabled != MH_OK)
        {
            LOG("Halo 2 C-H2-2 install: hook enable failed (%d); rolling back "
                "without touching the engine camera",
                static_cast<int>(enabled));
            if (!RemoveCore("install rollback"))
                g_coreState = CoreState::CleanupRequired;
            g_rejectedGeneration = generation;
            return false;
        }

        g_lastTelemetry = {};
        g_lastTelemetryMs = 0;
        g_coreState = CoreState::Installed;
        g_armed.store(true, std::memory_order_release);
        LOG("Halo 2 C-H2-2 installed: unique player-window hook +0x%X, exact "
            "caller return +0x%X, primary player only; one stock transaction "
            "per frame alternates position-only eyes. Both camera positions "
            "are restored independently; z_far and every unclaimed field stay "
            "engine-owned",
            static_cast<unsigned>(
                kHalo2RetailRenderPlayerWindowRva),
            static_cast<unsigned>(
                kHalo2RetailRenderPlayerWindowReturnRva));
        return true;
    }

    int NoteTransactionException(uint32_t generation, uint64_t serial) noexcept
    {
        g_telemetry.transactionExceptions.fetch_add(
            1, std::memory_order_relaxed);
        RejectTemporalFrame(generation, serial);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    bool GuardedCopyExact(
        void* destination, const void* source, size_t bytes) noexcept
    {
        if (!destination || !source || !bytes)
            return false;
        __try
        {
            std::memcpy(destination, source, bytes);
            return std::memcmp(destination, source, bytes) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    struct Halo2CameraReadback
    {
        Halo2CameraBasis basis{};
        float verticalFov = 0.0f;
        Halo2CameraRectangle viewport{};
        uint8_t asymmetricEnable = 0;
        uint8_t pixelOffsetEnable = 0;
    };

    bool ReadCamera(
        void* window, uint32_t cameraOffset,
        Halo2CameraReadback& out) noexcept
    {
        if (!window || reinterpret_cast<uintptr_t>(window) >
                UINTPTR_MAX - kHalo2RetailWindowStride)
        {
            return false;
        }

        Halo2CameraReadback candidate{};
        __try
        {
            const auto* camera = static_cast<const uint8_t*>(window) +
                cameraOffset;
            std::memcpy(
                &candidate.basis, camera + kHalo2CameraPositionOffset,
                sizeof(candidate.basis));
            std::memcpy(
                &candidate.verticalFov,
                camera + kHalo2CameraVerticalFovOffset,
                sizeof(candidate.verticalFov));
            std::memcpy(
                &candidate.viewport,
                camera + kHalo2CameraViewportRectangleOffset,
                sizeof(candidate.viewport));
            std::memcpy(
                &candidate.asymmetricEnable,
                camera + kHalo2CameraAsymmetricEnableOffset,
                sizeof(candidate.asymmetricEnable));
            std::memcpy(
                &candidate.pixelOffsetEnable,
                camera + kHalo2CameraPixelOffsetEnableOffset,
                sizeof(candidate.pixelOffsetEnable));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        out = candidate;
        return true;
    }

    bool ClaimPrimarySerial(uint64_t serial) noexcept
    {
        if (!serial)
            return false;
        uint64_t observed =
            g_seenPrimarySerial.load(std::memory_order_acquire);
        while (observed < serial)
        {
            if (g_seenPrimarySerial.compare_exchange_weak(
                    observed, serial, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return true;
            }
        }
        return false;
    }

    void Halo2PlayerWindowDetourBody(
        void* window, uint8_t flag, uintptr_t callerReturn) noexcept
    {
        g_telemetry.callbacks.fetch_add(1, std::memory_order_relaxed);

        Halo2TemporalTransactionInput input{};
        input.stereoRequested =
            g_stereoRequested.load(std::memory_order_acquire);
        input.hookArmed = g_installed.load(std::memory_order_acquire) &&
            g_armed.load(std::memory_order_acquire);
        input.coldObservationPassed =
            g_coldObservationPassed.load(std::memory_order_acquire);
        input.flagValid = flag == 0;
        input.levelLive = g_levelLive.load(std::memory_order_acquire);
        input.teardownRequested =
            g_teardownRequested.load(std::memory_order_acquire);
        input.activeGeneration =
            g_generation.load(std::memory_order_acquire);
        input.rightEyeFirst =
            g_rightEyeFirst.load(std::memory_order_acquire);

        const uintptr_t moduleBase =
            g_moduleBase.load(std::memory_order_acquire);
        input.exactCaller = moduleBase &&
            callerReturn ==
                moduleBase + kHalo2RetailRenderPlayerWindowReturnRva;

        // The title has one other proven caller. It remains completely stock:
        // no snapshot consumption, camera read, invalidation, or publication.
        if (!input.stereoRequested || !input.hookArmed ||
            !input.exactCaller)
        {
            if (input.stereoRequested && input.hookArmed &&
                !input.exactCaller)
            {
                g_telemetry.foreignCallers.fetch_add(
                    1, std::memory_order_relaxed);
            }
            const Halo2TemporalTransactionAction action =
                SelectHalo2TemporalTransactionAction(input);
            CallOriginalUnmodified(
                action, window, flag, input.activeGeneration, 0);
            return;
        }

        Halo2VrRenderSnapshot snapshot{};
        input.captureReady = VR_Halo2GetRenderSnapshot(snapshot);
        if (input.captureReady)
        {
            input.snapshotGeneration = snapshot.generation;
            input.preparedSerial = snapshot.preparedSerial;
            input.eye = Halo2TemporalEyeForSerial(
                snapshot.preparedSerial, input.rightEyeFirst);
        }
        else
        {
            g_telemetry.missingSnapshots.fetch_add(
                1, std::memory_order_relaxed);
        }

        input.windowReadable = ReadPlayerIndex(window, input.playerIndex);
        if (input.windowReadable && input.playerIndex != 0)
        {
            g_telemetry.splitWindows.fetch_add(
                1, std::memory_order_relaxed);
        }
        if (!input.flagValid)
        {
            g_telemetry.invalidFlags.fetch_add(
                1, std::memory_order_relaxed);
        }

        if (input.flagValid && input.captureReady && input.windowReadable &&
            input.playerIndex == 0 && input.preparedSerial &&
            input.snapshotGeneration == input.activeGeneration &&
            input.coldObservationPassed && input.levelLive &&
            !input.teardownRequested)
        {
            const bool firstClaim = ClaimPrimarySerial(input.preparedSerial);
            const bool alreadyRejected =
                g_rejectedSerial.load(std::memory_order_acquire) >=
                input.preparedSerial;
            input.serialAlreadyClaimed = !firstClaim || alreadyRejected;
            if (!firstClaim)
            {
                g_telemetry.duplicateFrames.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }

        Halo2CameraReadback renderCamera{};
        Halo2CameraReadback rasterCamera{};
        Halo2TemporalEyePositions eyePositions{};
        Halo2SymmetricHalfFovs halfFovs{};
        if (input.flagValid && input.captureReady && input.windowReadable &&
            input.playerIndex == 0 && !input.serialAlreadyClaimed &&
            input.snapshotGeneration == input.activeGeneration &&
            input.coldObservationPassed && input.levelLive &&
            !input.teardownRequested &&
            input.eye >= kHalo2LeftEye && input.eye <= kHalo2RightEye)
        {
            const bool renderRead =
                ReadCamera(window, kHalo2RenderCameraOffset, renderCamera);
            const bool rasterRead =
                ReadCamera(window, kHalo2RasterCameraOffset, rasterCamera);
            input.renderCameraValid = renderRead &&
                Halo2ValidateCameraBasis(renderCamera.basis);
            input.rasterCameraValid = rasterRead &&
                Halo2ValidateCameraBasis(rasterCamera.basis);
            if (renderRead && rasterRead)
            {
                input.stockProjectionSymmetric =
                    Halo2StockProjectionIsSymmetric(
                        renderCamera.asymmetricEnable,
                        renderCamera.pixelOffsetEnable,
                        rasterCamera.asymmetricEnable,
                        rasterCamera.pixelOffsetEnable);
                input.halfFovsValid = Halo2DeriveSymmetricHalfFovs(
                    rasterCamera.verticalFov, rasterCamera.viewport, halfFovs);
            }
            input.eyePositionValid =
                Halo2BuildTemporalEyePositions(
                    renderCamera.basis, rasterCamera.basis,
                    snapshot.eyes[input.eye].position, eyePositions) &&
                SelectHalo2CameraPositionWrite(
                    kHalo2WindowRenderPositionOffset,
                    kHalo2CameraVectorBytes) ==
                    Halo2CameraPositionWrite::RenderPosition &&
                SelectHalo2CameraPositionWrite(
                    kHalo2WindowRasterPositionOffset,
                    kHalo2CameraVectorBytes) ==
                    Halo2CameraPositionWrite::RasterPosition;
        }

        const Halo2TemporalTransactionAction action =
            SelectHalo2TemporalTransactionAction(input);
        const uint32_t rejectionGeneration = input.snapshotGeneration
            ? input.snapshotGeneration
            : input.activeGeneration;
        if (action !=
            Halo2TemporalTransactionAction::ScopedPositionsAndCallOnce)
        {
            if (action == Halo2TemporalTransactionAction::
                              RejectTemporalFrameAndCallStockOnce &&
                input.captureReady && input.windowReadable &&
                input.playerIndex == 0 && input.flagValid &&
                !input.serialAlreadyClaimed)
            {
                g_telemetry.invalidCameras.fetch_add(
                    1, std::memory_order_relaxed);
            }
            CallOriginalUnmodified(
                action, window, flag, rejectionGeneration,
                input.preparedSerial);
            return;
        }

        const Halo2RenderPlayerWindowFn original = OriginalPlayerWindow();
        if (!original)
        {
            RejectTemporalFrame(rejectionGeneration, input.preparedSerial);
            return;
        }

        auto* const windowBytes = static_cast<uint8_t*>(window);
        void* const renderPosition =
            windowBytes + kHalo2WindowRenderPositionOffset;
        void* const rasterPosition =
            windowBytes + kHalo2WindowRasterPositionOffset;
        float savedRenderPosition[3]{};
        float savedRasterPosition[3]{};
        std::memcpy(
            savedRenderPosition, renderCamera.basis.position,
            kHalo2CameraVectorBytes);
        std::memcpy(
            savedRasterPosition, rasterCamera.basis.position,
            kHalo2CameraVectorBytes);

        Halo2TemporalTransactionResult result{};
        bool renderWriteAttempted = false;
        bool rasterWriteAttempted = false;
        g_telemetry.claimedFrames.fetch_add(1, std::memory_order_relaxed);
        __try
        {
            __try
            {
                renderWriteAttempted = true;
                result.renderPositionWritten = GuardedCopyExact(
                    renderPosition, eyePositions.render,
                    kHalo2CameraVectorBytes);
                if (result.renderPositionWritten)
                {
                    rasterWriteAttempted = true;
                    result.rasterPositionWritten = GuardedCopyExact(
                        rasterPosition, eyePositions.raster,
                        kHalo2CameraVectorBytes);
                }

                // A partial camera transaction never calls into the engine.
                // The finally below restores every position whose write was
                // attempted before the result is rejected.
                if (result.renderPositionWritten &&
                    result.rasterPositionWritten)
                {
                    __try
                    {
                        ++result.originalCalls;
                        original(window, flag);
                        result.originalReturned = true;
                    }
                    __except (NoteTransactionException(
                        rejectionGeneration, input.preparedSerial))
                    {
                    }
                }
            }
            __finally
            {
                // Restore only the two independently saved 12-byte positions.
                // In particular, never overwrite render-camera z_far +0x44:
                // the stock transaction legitimately changes that field.
                if (renderWriteAttempted)
                {
                    result.renderPositionRestored = GuardedCopyExact(
                        renderPosition, savedRenderPosition,
                        kHalo2CameraVectorBytes);
                }
                if (rasterWriteAttempted)
                {
                    result.rasterPositionRestored = GuardedCopyExact(
                        rasterPosition, savedRasterPosition,
                        kHalo2CameraVectorBytes);
                }
            }
        }
        __except (NoteTransactionException(
            rejectionGeneration, input.preparedSerial))
        {
        }

        if ((renderWriteAttempted &&
             (!result.renderPositionWritten ||
              !result.renderPositionRestored)) ||
            (rasterWriteAttempted &&
             (!result.rasterPositionWritten ||
              !result.rasterPositionRestored)))
        {
            g_telemetry.transactionExceptions.fetch_add(
                1, std::memory_order_relaxed);
        }

        if (!Halo2TemporalTransactionResultMatches(action, result) ||
            !g_armed.load(std::memory_order_acquire) ||
            g_teardownRequested.load(std::memory_order_acquire) ||
            !g_stereoRequested.load(std::memory_order_acquire) ||
            !g_levelLive.load(std::memory_order_acquire) ||
            !g_coldObservationPassed.load(std::memory_order_acquire) ||
            g_generation.load(std::memory_order_acquire) !=
                input.activeGeneration ||
            g_rejectedSerial.load(std::memory_order_acquire) >=
                input.preparedSerial)
        {
            RejectTemporalFrame(rejectionGeneration, input.preparedSerial);
            return;
        }

        // A prepared frame may advance while the stock transaction is in
        // flight. Re-read the exact lock-free token before publication so a
        // stale camera render can never be captured as the next serial.
        Halo2VrRenderSnapshot completionSnapshot{};
        if (!VR_Halo2GetRenderSnapshot(completionSnapshot) ||
            completionSnapshot.generation != input.snapshotGeneration ||
            completionSnapshot.preparedSerial != input.preparedSerial)
        {
            g_telemetry.rejectedCompletions.fetch_add(
                1, std::memory_order_relaxed);
            RejectTemporalFrame(rejectionGeneration, input.preparedSerial);
            return;
        }

        if (!VR_Halo2CompleteTemporalEye(
                input.snapshotGeneration, input.preparedSerial, input.eye,
                halfFovs.horizontal, halfFovs.vertical))
        {
            g_telemetry.rejectedCompletions.fetch_add(
                1, std::memory_order_relaxed);
            RejectTemporalFrame(rejectionGeneration, input.preparedSerial);
            return;
        }
        g_telemetry.completedFrames.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Halo2TemporalStereo_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed) noexcept
{
    const uint32_t ownedGeneration =
        g_generation.load(std::memory_order_acquire);
    uint32_t vrFailureGeneration =
        g_vrFailureGeneration.load(std::memory_order_acquire);
    if (vrFailureGeneration && generation != vrFailureGeneration && generation)
    {
        g_vrFailureGeneration.compare_exchange_strong(
            vrFailureGeneration, 0, std::memory_order_acq_rel,
            std::memory_order_acquire);
        vrFailureGeneration =
            g_vrFailureGeneration.load(std::memory_order_acquire);
    }

    const bool identityValid = moduleBase && generation &&
        moduleSize == kHalo2RetailImageSize;
    const bool vrAvailable = !vrFailureGeneration ||
        generation != vrFailureGeneration;
    const bool desired = identityValid && activeAndRange && levelRunning &&
        coldPassed && vrAvailable;
    const bool ownsDifferentModule = g_hookTarget &&
        (ownedGeneration != generation ||
         g_moduleBase.load(std::memory_order_acquire) != moduleBase);
    const bool hotEligible = desired && !ownsDifferentModule &&
        g_coreState == CoreState::Installed;

    // These are the complete worker-owned admission facts consumed by the hot
    // hook. Publish a revoked state before disable/removal starts.
    g_levelLive.store(levelRunning, std::memory_order_release);
    g_coldObservationPassed.store(coldPassed, std::memory_order_release);
    g_rightEyeFirst.store(
        g_config.right_eye_first, std::memory_order_release);
    g_stereoRequested.store(hotEligible, std::memory_order_release);
    ReportTelemetry();

    if (g_hookTarget && (!desired || ownsDifferentModule ||
                         g_coreState == CoreState::CleanupRequired))
    {
        const char* reason = ownsDifferentModule
            ? "module generation changed"
            : (!vrAvailable ? "OpenXR runtime failed"
                            : (!activeAndRange
                                   ? "Halo 2 is no longer uniquely active"
                                   : (!levelRunning
                                          ? "level liveness closed"
                                          : (!coldPassed
                                                 ? "cold proof was revoked"
                                                 : "cleanup retry"))));
        if (!RemoveCore(reason))
            return false;
    }

    if (!desired || g_rejectedGeneration == generation)
        return false;
    if (!g_hookTarget && !InstallCore(moduleBase, moduleSize, generation))
        return false;
    const bool armed = g_coreState == CoreState::Installed &&
        g_installed.load(std::memory_order_acquire) &&
        g_armed.load(std::memory_order_acquire) &&
        g_generation.load(std::memory_order_acquire) == generation;
    g_stereoRequested.store(armed, std::memory_order_release);
    return armed;
}

bool Halo2TemporalStereo_Installed() noexcept
{
    return g_installed.load(std::memory_order_acquire);
}

bool Halo2TemporalStereo_Armed() noexcept
{
    return g_armed.load(std::memory_order_acquire);
}

uint32_t Halo2TemporalStereo_Generation() noexcept
{
    return g_generation.load(std::memory_order_acquire);
}

void Halo2TemporalStereo_ShutdownForVrFailure() noexcept
{
    const uint32_t generation =
        g_generation.load(std::memory_order_acquire);
    if (generation)
        g_vrFailureGeneration.store(generation, std::memory_order_release);
    g_stereoRequested.store(false, std::memory_order_release);
    g_teardownRequested.store(true, std::memory_order_release);
    g_armed.store(false, std::memory_order_release);
    VR_ResetHalo2TemporalStereo();
}

#else

bool Halo2TemporalStereo_Poll(
    uintptr_t, size_t, uint32_t, bool, bool, bool) noexcept
{
    return false;
}

bool Halo2TemporalStereo_Installed() noexcept { return false; }
bool Halo2TemporalStereo_Armed() noexcept { return false; }
uint32_t Halo2TemporalStereo_Generation() noexcept { return 0; }
void Halo2TemporalStereo_ShutdownForVrFailure() noexcept {}

#endif
