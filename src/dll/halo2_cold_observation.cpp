#include "halo2_cold_observation.h"

#include <windows.h>

#include <cstring>

#include "../common/halo2_render_logic.h"
#include "../common/log.h"
#include "game.h"
#include "halo2_render_mode_guard.h"
#include "vr.h"

#ifndef HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION
#define HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION 0
#endif

namespace
{
    uintptr_t g_gateBase = 0;
    size_t g_gateSize = 0;
    uint32_t g_gateGeneration = 0;
    uintptr_t g_gameTimeSlot = 0;
    uintptr_t g_gameTimeObject = 0;
    bool g_gateAnchorsChecked = false;
    bool g_gateAnchorsProven = false;
    bool g_gateOpenLogged = false;
    uint64_t g_lastGateLogMs = 0;
    Halo2GameTimeGateLogic g_gateLogic;

    uintptr_t g_completedBase = 0;
    uint32_t g_completedGeneration = 0;
    bool g_completed = false;
    uint32_t g_passedGeneration = 0;
    bool g_passed = false;
    uint32_t g_pinFailLoggedGeneration = 0;

    // E-H2-3 / E-H2-4 read-only graphics-mode state, reset with the gate.
    Halo2GraphicsMode g_graphicsMode = Halo2GraphicsMode::Unknown;
    uint32_t g_graphicsModeGeneration = 0;
    bool g_graphicsModeValid = false;
    uint8_t g_classicRenderDisabledByte = 0;
    int32_t g_appliedRenderMode = 0;
    bool g_appliedRenderModeValid = false;
    uintptr_t g_observerResultArray = 0;
    uintptr_t g_classicRenderDisabledByteAddress = 0;

    int HexNibble(char c) noexcept
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }

    constexpr size_t kMaxHalo2PatternBytes = 96;

    struct FixedPattern
    {
        uint8_t bytes[kMaxHalo2PatternBytes]{};
        uint8_t wild[kMaxHalo2PatternBytes]{};
        size_t length = 0;
        size_t anchor = 0;
    };

    bool CompilePattern(const char* pattern, FixedPattern& compiled) noexcept
    {
        compiled = {};
        if (!pattern)
            return false;
        for (const char* p = pattern; *p;)
        {
            if (*p == ' ')
            {
                ++p;
                continue;
            }
            if (compiled.length >= kMaxHalo2PatternBytes)
                return false;
            if (*p == '?')
            {
                compiled.wild[compiled.length++] = 1;
                ++p;
                if (*p == '?')
                    ++p;
                continue;
            }
            if (!p[1])
                return false;
            const int hi = HexNibble(p[0]);
            const int lo = HexNibble(p[1]);
            if (hi < 0 || lo < 0)
                return false;
            compiled.bytes[compiled.length++] =
                static_cast<uint8_t>((hi << 4) | lo);
            p += 2;
        }
        if (!compiled.length)
            return false;
        while (compiled.anchor < compiled.length &&
               compiled.wild[compiled.anchor])
        {
            ++compiled.anchor;
        }
        return true;
    }

    // Exact-at-RVA comparison used before level liveness. It reads at most 73
    // pinned code bytes and never allocates or walks the image. Full uniqueness
    // is deliberately deferred until the game clock proves loading is over.
    bool PatternMatchesAt(
        uintptr_t address, size_t available, const char* pattern) noexcept
    {
        FixedPattern compiled{};
        if (!CompilePattern(pattern, compiled) || !address ||
            compiled.length > available)
            return false;
        __try
        {
            for (size_t index = 0; index < compiled.length; ++index)
            {
                if (!compiled.wild[index] &&
                    *reinterpret_cast<const uint8_t*>(address + index) !=
                        compiled.bytes[index])
                {
                    return false;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool IsReadableProtection(DWORD protect) noexcept
    {
        if ((protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        switch (protect & 0xFFu)
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

    // Fixed-storage, guarded scanner for this optional observation. Unlike the
    // shared signature helper it cannot allocate, it walks VM regions without
    // dereferencing inaccessible image gaps, and any query/read fault degrades
    // the optional result to failure. Only two matches are needed to prove
    // ambiguity.
    bool CountPatternMatches(
        uintptr_t base, size_t size, const char* pattern,
        uintptr_t& firstMatch, uint32_t& matchCount) noexcept
    {
        firstMatch = 0;
        matchCount = 0;
        FixedPattern compiled{};
        if (!base || !size || base > UINTPTR_MAX - size ||
            !CompilePattern(pattern, compiled) || size < compiled.length)
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
                if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info,
                                  sizeof(info)) || !info.RegionSize)
                {
                    return false;
                }
                const uintptr_t regionBase =
                    reinterpret_cast<uintptr_t>(info.BaseAddress);
                if (regionBase > UINTPTR_MAX - info.RegionSize)
                    return false;
                uintptr_t regionEnd = regionBase + info.RegionSize;
                if (regionEnd > imageEnd)
                    regionEnd = imageEnd;
                if (regionEnd <= cursor)
                    return false;

                if (info.State == MEM_COMMIT &&
                    IsReadableProtection(info.Protect) &&
                    compiled.length <= regionEnd - cursor)
                {
                    const auto* data =
                        reinterpret_cast<const uint8_t*>(cursor);
                    const size_t span = regionEnd - cursor;
                    const size_t last = span - compiled.length;
                    size_t offset = 0;
                    while (offset <= last)
                    {
                        if (compiled.anchor == compiled.length)
                        {
                            if (!firstMatch)
                                firstMatch = cursor + offset;
                            if (++matchCount >= 2)
                                return true;
                            ++offset;
                            continue;
                        }
                        const auto* anchorHit = static_cast<const uint8_t*>(
                            std::memchr(
                                data + offset + compiled.anchor,
                                compiled.bytes[compiled.anchor],
                                last - offset + 1));
                        if (!anchorHit)
                            break;
                        const size_t candidate =
                            static_cast<size_t>(anchorHit - data) -
                            compiled.anchor;
                        size_t index = 0;
                        for (; index < compiled.length; ++index)
                        {
                            if (!compiled.wild[index] &&
                                data[candidate + index] != compiled.bytes[index])
                            {
                                break;
                            }
                        }
                        if (index == compiled.length)
                        {
                            if (!firstMatch)
                                firstMatch = cursor + candidate;
                            if (++matchCount >= 2)
                                return true;
                        }
                        offset = candidate + 1;
                    }
                }
                cursor = regionEnd;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            firstMatch = 0;
            matchCount = 0;
            return false;
        }
        return true;
    }

    bool DecodeRelativeTarget(
        uintptr_t match, uint8_t dispOffset, uintptr_t& target) noexcept
    {
        if (!match || !dispOffset)
            return false;
        __try
        {
            const int32_t displacement =
                *reinterpret_cast<const int32_t*>(match + dispOffset);
            target = match + dispOffset + 4 + displacement;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool VerifyLoadedPeIdentity(uintptr_t base, size_t size) noexcept
    {
        __try
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE ||
                dos->e_lfanew < 0)
            {
                return false;
            }
            const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
            if (ntOffset > size || sizeof(IMAGE_NT_HEADERS64) > size - ntOffset)
                return false;
            const auto* nt =
                reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + ntOffset);
            return nt->Signature == IMAGE_NT_SIGNATURE &&
                nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                nt->FileHeader.TimeDateStamp == kHalo2RetailPeTimestamp &&
                nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                nt->OptionalHeader.SizeOfImage == kHalo2RetailImageSize;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool IsWritableImageRange(
        uintptr_t base, size_t size, uintptr_t address, size_t span) noexcept
    {
        if (!base || !size || !address || !span || address < base ||
            address - base >= size || span > size - (address - base))
        {
            return false;
        }
        __try
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                base + static_cast<size_t>(dos->e_lfanew));
            const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
            const uint32_t rva = static_cast<uint32_t>(address - base);
            for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections;
                 ++i, ++section)
            {
                const uint32_t sectionSpan =
                    section->Misc.VirtualSize > section->SizeOfRawData
                    ? section->Misc.VirtualSize : section->SizeOfRawData;
                if (rva >= section->VirtualAddress &&
                    rva - section->VirtualAddress <= sectionSpan &&
                    span <= sectionSpan - (rva - section->VirtualAddress))
                {
                    return (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return false;
    }

    bool IsReadableRange(uintptr_t address, size_t span) noexcept
    {
        if (!address || !span || address > UINTPTR_MAX - span)
            return false;
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info,
                          sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(info.BaseAddress);
        return address >= regionBase && span <= info.RegionSize - (address - regionBase);
    }

    void ResetGate(uintptr_t base, size_t size, uint32_t generation) noexcept
    {
        g_gateBase = base;
        g_gateSize = size;
        g_gateGeneration = generation;
        g_gameTimeSlot = 0;
        g_gameTimeObject = 0;
        g_gateAnchorsChecked = false;
        g_gateAnchorsProven = false;
        g_gateOpenLogged = false;
        g_lastGateLogMs = 0;
        g_gateLogic.Reset();
        g_graphicsMode = Halo2GraphicsMode::Unknown;
        g_graphicsModeGeneration = 0;
        g_graphicsModeValid = false;
        g_classicRenderDisabledByte = 0;
        g_appliedRenderMode = 0;
        g_appliedRenderModeValid = false;
        g_observerResultArray = 0;
        g_classicRenderDisabledByteAddress = 0;
    }

    bool PrepareGate() noexcept
    {
        if (g_gateAnchorsChecked)
            return g_gateAnchorsProven;
        g_gateAnchorsChecked = true;

        if (g_gateSize != kHalo2RetailImageSize ||
            !VerifyLoadedPeIdentity(g_gateBase, g_gateSize))
        {
            LOG("Halo 2 cold observation WITHHELD: loaded PE identity does not "
                "match the pinned retail halo2.dll; no image scan, hook, or "
                "engine write was attempted");
            return false;
        }

        uintptr_t decodedSlot = 0;
        const size_t gateAnchors[] = {
            kHalo2AnchorGameTimeIncrement,
            kHalo2AnchorGameTimeInit,
        };
        for (size_t index : gateAnchors)
        {
            const Halo2RetailAnchor& anchor = kHalo2RetailAnchors[index];
            if (anchor.rva >= g_gateSize)
                return false;
            const uintptr_t expected = g_gateBase + anchor.rva;
            const size_t available = g_gateSize - anchor.rva;
            uintptr_t target = 0;
            if (!PatternMatchesAt(expected, available, anchor.pattern) ||
                !DecodeRelativeTarget(
                    expected, anchor.relativeDispOffset, target) ||
                target != g_gateBase + anchor.relativeTargetRva ||
                (target & (alignof(uintptr_t) - 1)) != 0 ||
                !IsWritableImageRange(
                    g_gateBase, g_gateSize, target, sizeof(uintptr_t)) ||
                (decodedSlot && decodedSlot != target))
            {
                LOG("Halo 2 cold observation WITHHELD: pinned liveness anchor "
                    "%s did not match/decode at RVA 0x%X; only its bounded "
                    "expected bytes were read, the module was not scanned, "
                    "and no hook or engine write was attempted",
                    anchor.name, anchor.rva);
                return false;
            }
            decodedSlot = target;
        }

        g_gameTimeSlot = decodedSlot;
        g_gateAnchorsProven = g_gameTimeSlot != 0;
        if (g_gateAnchorsProven)
        {
            LOG("Halo 2 level-load gate armed: two independent pinned code "
                "anchors agree on game_time_globals pointer slot RVA 0x%X; "
                "waiting for a coherent post-initialization game tick",
                kHalo2GameTimeSlotRva);
        }
        return g_gateAnchorsProven;
    }

    bool ReadCoherentGameTimeSample(
        uintptr_t& object, bool& initialized, uint32_t& tick) noexcept
    {
        uintptr_t firstObject = 0;
        uintptr_t secondObject = 0;
        uint8_t firstInitialized = 0;
        uint8_t secondInitialized = 0;
        __try
        {
            firstObject =
                *reinterpret_cast<const uintptr_t*>(g_gameTimeSlot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        const size_t sampleSpan = kHalo2GameTimeCurrentTickOffset +
            sizeof(uint32_t);
        if (!firstObject || (firstObject & (alignof(uint32_t) - 1)) != 0 ||
            !IsReadableRange(firstObject, sampleSpan))
        {
            return false;
        }
        __try
        {
            firstInitialized = *reinterpret_cast<const uint8_t*>(
                firstObject + kHalo2GameTimeInitializedOffset);
            tick = *reinterpret_cast<const uint32_t*>(
                firstObject + kHalo2GameTimeCurrentTickOffset);
            secondInitialized = *reinterpret_cast<const uint8_t*>(
                firstObject + kHalo2GameTimeInitializedOffset);
            secondObject =
                *reinterpret_cast<const uintptr_t*>(g_gameTimeSlot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (firstObject != secondObject ||
            firstInitialized != secondInitialized || firstInitialized > 1)
        {
            return false;
        }
        object = firstObject;
        initialized = firstInitialized == 1;
        return true;
    }

    class Halo2ModulePin
    {
    public:
        ~Halo2ModulePin()
        {
            if (m_module)
                FreeLibrary(m_module);
        }

        bool Acquire(uintptr_t expectedBase) noexcept
        {
            if (!expectedBase || !GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(expectedBase), &m_module) ||
                reinterpret_cast<uintptr_t>(m_module) != expectedBase)
            {
                if (m_module)
                {
                    FreeLibrary(m_module);
                    m_module = nullptr;
                }
                return false;
            }
            return true;
        }

        bool IsCurrent(uintptr_t expectedBase) const noexcept
        {
            return m_module &&
                reinterpret_cast<uintptr_t>(m_module) == expectedBase &&
                GetModuleHandleW(L"halo2.dll") == m_module;
        }

    private:
        HMODULE m_module = nullptr;
    };

    // -----------------------------------------------------------------
    // E-H2-3 / E-H2-4 read-only graphics-mode observation.
    //
    // halo2.dll ships two renderers. The classic Blam tree is skipped whole
    // when the byte the classic driver tests is non-zero, which is why every
    // classic-path render hook can install correctly and still receive zero
    // callbacks. Reporting the live mode costs one guarded read and turns an
    // otherwise unattributable "nothing hooked" result into a named one.
    //
    // This stays inside C-H2-1's accepted contract: bounded reads of already
    // pinned code plus two module globals. No hook, no engine write.
    // -----------------------------------------------------------------

    // DecodeRelativeTarget assumes the displacement is the final field of the
    // instruction. The classic gate is `cmp byte [rip+disp32], imm8`, whose
    // trailing immediate makes the next instruction one byte further on, so
    // the caller states both offsets explicitly rather than relying on that.
    bool DecodeRipRelative(
        uintptr_t match, uint32_t dispOffset, uint32_t nextOffset,
        uintptr_t& target) noexcept
    {
        if (!match || !dispOffset || nextOffset <= dispOffset)
            return false;
        __try
        {
            const int32_t displacement =
                *reinterpret_cast<const int32_t*>(match + dispOffset);
            target = match + nextOffset + displacement;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadGuardedByte(uintptr_t address, uint8_t& value) noexcept
    {
        if (!address || !IsReadableRange(address, sizeof(uint8_t)))
            return false;
        __try
        {
            value = *reinterpret_cast<const volatile uint8_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadGuardedInt32(uintptr_t address, int32_t& value) noexcept
    {
        if (!address || !IsReadableRange(address, sizeof(int32_t)))
            return false;
        __try
        {
            value = *reinterpret_cast<const volatile int32_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Resolves a unique signature and requires it to sit at its pinned RVA.
    // Zero or multiple matches fail closed and are logged by name.
    bool ResolveUniqueAnchor(
        uintptr_t base, size_t size, const char* pattern, uint32_t pinnedRva,
        const char* name, uintptr_t& match) noexcept
    {
        match = 0;
        uint32_t matchCount = 0;
        if (!CountPatternMatches(base, size, pattern, match, matchCount))
        {
            LOG("Halo 2 graphics mode: guarded image scan failed for %s; the "
                "live renderer is not reported this generation", name);
            return false;
        }
        if (matchCount != 1)
        {
            LOG("Halo 2 graphics mode: %s matched %u times, not once; the "
                "live renderer is not reported this generation",
                name, matchCount);
            match = 0;
            return false;
        }
        if (match != base + pinnedRva)
        {
            LOG("Halo 2 graphics mode: %s moved from pinned RVA 0x%X to "
                "0x%llX; the live renderer is not reported this generation",
                name, pinnedRva,
                static_cast<unsigned long long>(match - base));
            match = 0;
            return false;
        }
        return true;
    }

    void ObserveGraphicsMode(uintptr_t base, size_t size) noexcept
    {
        g_graphicsMode = Halo2GraphicsMode::Unknown;
        g_graphicsModeValid = false;
        g_appliedRenderModeValid = false;
        g_observerResultArray = 0;
        g_classicRenderDisabledByteAddress = 0;

        uintptr_t gateMatch = 0;
        if (!ResolveUniqueAnchor(
                base, size, kHalo2ClassicRenderGatePattern,
                kHalo2ClassicRenderDriverRva, "classic render gate",
                gateMatch))
        {
            return;
        }

        uintptr_t gateByteAddress = 0;
        if (!DecodeRipRelative(
                gateMatch, kHalo2ClassicRenderGateDispOffset,
                kHalo2ClassicRenderGateNextOffset, gateByteAddress) ||
            gateByteAddress != base + kHalo2ClassicRenderDisabledByteRva)
        {
            LOG("Halo 2 graphics mode: the classic render gate decoded to "
                "RVA 0x%llX instead of the pinned 0x%X; the live renderer is "
                "not reported this generation",
                gateByteAddress
                    ? static_cast<unsigned long long>(gateByteAddress - base)
                    : 0ull,
                kHalo2ClassicRenderDisabledByteRva);
            return;
        }

        uint8_t classicDisabled = 0;
        if (!ReadGuardedByte(gateByteAddress, classicDisabled))
        {
            LOG("Halo 2 graphics mode: the classic-disabled byte at RVA 0x%X "
                "was not readable; the live renderer is not reported",
                kHalo2ClassicRenderDisabledByteRva);
            return;
        }

        // The mode dword is corroboration only. The byte above is what the
        // classic driver actually tests, so it decides, and a disagreement is
        // reported rather than silently resolved.
        int32_t appliedMode = 0;
        const bool appliedModeRead = ReadGuardedInt32(
            base + kHalo2AppliedRenderModeRva, appliedMode);

        // The observer accessor carries its own 0x368 stride inside the
        // signature, so a moved array cannot silently change the element size.
        uintptr_t observerMatch = 0;
        uintptr_t observerResultArray = 0;
        if (ResolveUniqueAnchor(
                base, size, kHalo2ObserverResultAccessorPattern,
                kHalo2ObserverResultAccessorRva, "observer result accessor",
                observerMatch))
        {
            if (DecodeRipRelative(
                    observerMatch, kHalo2ObserverResultAccessorDispOffset,
                    kHalo2ObserverResultAccessorNextOffset,
                    observerResultArray) &&
                observerResultArray == base + kHalo2ObserverResultArrayRva)
            {
                g_observerResultArray = observerResultArray;
            }
            else
            {
                LOG("Halo 2 graphics mode: the observer result accessor "
                    "decoded to RVA 0x%llX instead of the pinned 0x%X",
                    observerResultArray
                        ? static_cast<unsigned long long>(
                              observerResultArray - base)
                        : 0ull,
                    kHalo2ObserverResultArrayRva);
            }
        }

        g_classicRenderDisabledByteAddress = gateByteAddress;
        g_classicRenderDisabledByte = classicDisabled;
        g_appliedRenderMode = appliedMode;
        g_appliedRenderModeValid = appliedModeRead;
        g_graphicsMode = Halo2ClassicRenderTreeRuns(classicDisabled)
            ? Halo2GraphicsMode::Classic
            : Halo2GraphicsMode::Remastered;
        g_graphicsModeValid = true;
        g_graphicsModeGeneration = g_gateGeneration;

        const bool coherent = appliedModeRead &&
            Halo2GraphicsModeIsCoherent(appliedMode, classicDisabled);
        if (g_graphicsMode == Halo2GraphicsMode::Classic)
        {
            LOG("Halo 2 live renderer: CLASSIC (legacy Blam). The classic "
                "render tree runs, so its proven hooks can fire. Gate byte "
                "RVA 0x%X = 0, mode dword RVA 0x%X = %d (%s), observer "
                "results at RVA 0x%X stride 0x%X",
                kHalo2ClassicRenderDisabledByteRva,
                kHalo2AppliedRenderModeRva,
                appliedModeRead ? appliedMode : 0,
                appliedModeRead
                    ? (coherent ? "coherent" : "DISAGREES with the gate byte")
                    : "unreadable",
                kHalo2ObserverResultArrayRva, kHalo2ObserverStride);
        }
        else
        {
            LOG("Halo 2 live renderer: REMASTERED (Anniversary / Saber). The "
                "classic render tree is skipped whole at the driver's second "
                "instruction, so a classic-path render hook installs cleanly "
                "and receives ZERO callbacks by design - that is not a broken "
                "signature. Gate byte RVA 0x%X = %u, mode dword RVA 0x%X = %d "
                "(%s), observer results at RVA 0x%X stride 0x%X",
                kHalo2ClassicRenderDisabledByteRva,
                static_cast<unsigned>(classicDisabled),
                kHalo2AppliedRenderModeRva,
                appliedModeRead ? appliedMode : 0,
                appliedModeRead
                    ? (coherent ? "coherent" : "DISAGREES with the gate byte")
                    : "unreadable",
                kHalo2ObserverResultArrayRva, kHalo2ObserverStride);
        }
    }

    void RunColdObservation() noexcept
    {
        Halo2ModulePin pin;
        if (!pin.Acquire(g_gateBase))
        {
            if (g_pinFailLoggedGeneration != g_gateGeneration)
            {
                g_pinFailLoggedGeneration = g_gateGeneration;
                LOG("Halo 2 cold observation: halo2.dll could not be pinned "
                    "after the game-time gate opened; attempt not consumed, "
                    "will retry");
            }
            return;
        }

        Halo2ColdObservationResult result{};
        result.moduleRangeValid = g_gateSize == kHalo2RetailImageSize;
        result.peIdentity = VerifyLoadedPeIdentity(g_gateBase, g_gateSize);
        result.postInitializationTickObserved = g_gateLogic.IsOpen();

        if (result.moduleRangeValid && result.peIdentity)
        {
            for (const Halo2RetailAnchor& anchor : kHalo2RetailAnchors)
            {
                uintptr_t hit = 0;
                uint32_t matchCount = 0;
                if (!CountPatternMatches(
                        g_gateBase, g_gateSize, anchor.pattern,
                        hit, matchCount))
                {
                    LOG("Halo 2 cold observation: guarded image scan failed "
                        "for anchor %s; optional observation remains stock",
                        anchor.name);
                    continue;
                }
                if (matchCount == 0)
                {
                    LOG("Halo 2 cold observation: anchor %s matched ZERO "
                        "times (pinned RVA 0x%X)", anchor.name, anchor.rva);
                    continue;
                }
                if (matchCount != 1)
                {
                    LOG("Halo 2 cold observation: anchor %s is NOT unique "
                        "(first RVA 0x%zX, pinned 0x%X)", anchor.name,
                        hit - g_gateBase, anchor.rva);
                    continue;
                }
                ++result.anchorsMatchedOnce;
                if (hit - g_gateBase == anchor.rva)
                    ++result.anchorsAtPinnedRva;
                else
                    LOG("Halo 2 cold observation: anchor %s moved (RVA "
                        "0x%zX, pinned 0x%X)", anchor.name,
                        hit - g_gateBase, anchor.rva);

                if (anchor.relativeDispOffset)
                {
                    uintptr_t target = 0;
                    if (DecodeRelativeTarget(
                            hit, anchor.relativeDispOffset, target) &&
                        target == g_gateBase + anchor.relativeTargetRva)
                    {
                        ++result.relativeTargetsAtPinnedRva;
                    }
                    else
                    {
                        LOG("Halo 2 cold observation: anchor %s relative "
                            "decode missed pinned target RVA 0x%X",
                            anchor.name, anchor.relativeTargetRva);
                    }
                }
            }
        }

        result.mappingStable = pin.IsCurrent(g_gateBase);
        g_completed = true;
        g_completedBase = g_gateBase;
        g_completedGeneration = g_gateGeneration;
        g_passed = Halo2ColdObservationPass(result);
        g_passedGeneration = g_passed ? g_gateGeneration : 0;

        if (g_passed)
        {
            LOG("Halo 2 cold observation PASS (C-H2-1): coherent active-level "
                "tick observed; PE timestamp 0x%08X and SizeOfImage 0x%08X "
                "match; all %zu anchors are unique at pinned RVAs and both "
                "game-time decodes agree on slot RVA 0x%X. This authorizes "
                "no hook or engine write; stereo and 6DOF remain disabled",
                kHalo2RetailPeTimestamp,
                static_cast<uint32_t>(kHalo2RetailImageSize),
                kHalo2RetailAnchorCount, kHalo2GameTimeSlotRva);
            ObserveGraphicsMode(g_gateBase, g_gateSize);
        }
        else
        {
            LOG("Halo 2 cold observation FAIL (C-H2-1): range=%d "
                "peIdentity=%d anchorsOnce=%u/%zu anchorsPinned=%u/%zu "
                "relativeDecodes=%u/%u activeTick=%d mappingStable=%d; "
                "hooks and engine writes remained disabled",
                result.moduleRangeValid ? 1 : 0,
                result.peIdentity ? 1 : 0,
                result.anchorsMatchedOnce, kHalo2RetailAnchorCount,
                result.anchorsAtPinnedRva, kHalo2RetailAnchorCount,
                result.relativeTargetsAtPinnedRva,
                kHalo2RetailAnchorRelativeTargets,
                result.postInitializationTickObserved ? 1 : 0,
                result.mappingStable ? 1 : 0);
        }
    }
}

bool Halo2ColdObservation_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation) noexcept
{
#if !HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION
    (void)moduleBase;
    (void)moduleSize;
    (void)generation;
    return false;
#else
    if (!moduleBase || !moduleSize || !generation)
        return false;
    const bool completedForModuleInstance = g_completed &&
        g_completedBase == moduleBase &&
        g_completedGeneration == generation;
    if (g_gateBase != moduleBase || g_gateGeneration != generation)
        ResetGate(moduleBase, moduleSize, generation);
    if (!PrepareGate())
        return false;

    uintptr_t object = 0;
    bool initialized = false;
    uint32_t tick = 0;
    if (!ReadCoherentGameTimeSample(object, initialized, tick))
    {
        const bool invalidatedOpen = g_gateLogic.IsOpen();
        g_gameTimeObject = 0;
        g_gateLogic.InvalidateSample();
        if (invalidatedOpen)
        {
            g_gateOpenLogged = false;
            LOG("Halo 2 level-load gate: an unreadable or incoherent game-time "
                "sample closed level liveness; fresh proof is required");
        }
        const uint64_t now = GetTickCount64();
        if (now - g_lastGateLogMs >= 2000)
        {
            g_lastGateLogMs = now;
            LOG("Halo 2 level-load gate: coherent game-time read failed; "
                "holding cold observation without manufacturing a still "
                "sample");
        }
        return false;
    }
    if (object != g_gameTimeObject)
    {
        const bool invalidatedOpen = g_gateLogic.IsOpen();
        g_gameTimeObject = object;
        g_gateLogic.InvalidateSample();
        if (invalidatedOpen)
        {
            g_gateOpenLogged = false;
            LOG("Halo 2 level-load gate: game_time_globals object changed; "
                "level liveness closed and a fresh baseline is required");
        }
    }

    const bool wasOpen = g_gateLogic.IsOpen();
    const Halo2GameTimeGateLogic::Decision decision =
        g_gateLogic.Observe(initialized, tick);
    if (decision == Halo2GameTimeGateLogic::Decision::Hold)
    {
        if (wasOpen)
        {
            g_gateOpenLogged = false;
            LOG("Halo 2 level-load gate: game_time_globals became explicitly "
                "uninitialized/disposed; level liveness closed while the "
                "one-shot image result remains cached");
        }
        const uint64_t now = GetTickCount64();
        if (now - g_lastGateLogMs >= 2000)
        {
            g_lastGateLogMs = now;
            LOG("Halo 2 level-load gate: holding cold observation "
                "(generation %u, initialized=%d, uninitializedSeen=%d, "
                "still=%u, "
                "changeRun=%u)", g_gateGeneration, initialized ? 1 : 0,
                g_gateLogic.SawUninitialized() ? 1 : 0,
                g_gateLogic.StillRun(), g_gateLogic.ChangeRun());
        }
        return false;
    }

    if (!g_gateOpenLogged && decision ==
        Halo2GameTimeGateLogic::Decision::OpenAfterBoundaryThenTick)
    {
        g_gateOpenLogged = true;
        LOG("Halo 2 level-load gate: game_time_globals was explicitly "
            "uninitialized/disposed "
            "and now completed an active update; level running");
    }
    else if (!g_gateOpenLogged)
    {
        g_gateOpenLogged = true;
        LOG("Halo 2 level-load gate: game_time_globals changed on %u "
            "consecutive samples; title was already running when observed",
            g_gateLogic.ChangeRun());
    }
    if (Halo2ColdObservationNeedsImageScan(completedForModuleInstance))
        RunColdObservation();
    return g_gateLogic.IsOpen();
#endif
}

void Halo2ColdObservation_Rearm() noexcept
{
    if (g_gateBase || g_gateGeneration)
        ResetGate(0, 0, 0);
}

bool Halo2ColdObservation_Pending(uint32_t generation) noexcept
{
    return !(generation && g_completed &&
        g_completedGeneration == generation);
}

bool Halo2ColdObservation_Passed(uint32_t generation) noexcept
{
    return generation && g_passed && g_passedGeneration == generation;
}

Halo2GraphicsMode Halo2ColdObservation_GraphicsMode(
    uint32_t generation) noexcept
{
#if !HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION
    (void)generation;
    return Halo2GraphicsMode::Unknown;
#else
    if (!generation || !g_graphicsModeValid ||
        g_graphicsModeGeneration != generation ||
        !g_classicRenderDisabledByteAddress)
    {
        return Halo2GraphicsMode::Unknown;
    }
    // The player can toggle Halo 2's graphics mode at any moment, and the
    // engine flips this byte the instant they do. Reading it once at
    // cold-observation time would freeze the answer for the whole module
    // generation and leave the classic stereo core withheld in a mode where
    // its hooks demonstrably fire; the D-H2-1 census caught exactly that.
    // The ADDRESS is proven once; the VALUE is re-read every call.
    uint8_t live = 0;
    if (!ReadGuardedByte(g_classicRenderDisabledByteAddress, live))
        return Halo2GraphicsMode::Unknown;
    if (live != g_classicRenderDisabledByte)
    {
        g_classicRenderDisabledByte = live;
        g_graphicsMode = Halo2ClassicRenderTreeRuns(live)
            ? Halo2GraphicsMode::Classic
            : Halo2GraphicsMode::Remastered;
        // E-H2-13: the engine switches on a rising edge of input action 0x21
        // (frame driver 0x515E0) and the only pad it sees is the mod's; name
        // every button mask fed in the last 600 ms so the trigger is evidence.
        char recentButtons[256];
        Input_DescribeRecentButtons(recentButtons, sizeof(recentButtons), 600);
        char guard[256];
        Halo2RenderModeGuard_DescribeLastSwitch(guard, sizeof(guard));
        LOG("Halo 2 live renderer CHANGED to %s (gate byte RVA 0x%X = %u); "
            "the classic render tree %s execute now; virtual pad buttons fed "
            "in the last 600 ms: %s; switch guard: %s",
            g_graphicsMode == Halo2GraphicsMode::Classic
                ? "CLASSIC (legacy Blam)"
                : "REMASTERED (Anniversary / Saber)",
            kHalo2ClassicRenderDisabledByteRva,
            static_cast<unsigned>(live),
            Halo2ClassicRenderTreeRuns(live) ? "DOES" : "does NOT",
            recentButtons, guard);
        // E-H2-34: a picture of the renderer we just switched TO, 3 s in,
        // so a short visit still leaves eye pictures next to the log.
        VR_Halo2RequestEyeDump(3000);
    }
    return g_graphicsMode;
#endif
}

bool Halo2ColdObservation_ClassicRenderTreeRuns(
    uint32_t generation) noexcept
{
    return Halo2ColdObservation_GraphicsMode(generation) ==
        Halo2GraphicsMode::Classic;
}

uintptr_t Halo2ColdObservation_ObserverResultArray(
    uint32_t generation) noexcept
{
#if !HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION
    (void)generation;
    return 0;
#else
    if (!generation || !g_graphicsModeValid ||
        g_graphicsModeGeneration != generation)
    {
        return 0;
    }
    return g_observerResultArray;
#endif
}
