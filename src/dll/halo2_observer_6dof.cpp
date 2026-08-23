#include "halo2_observer_6dof.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include "../common/halo2_render_logic.h"
#include "../common/log.h"
#include "game.h"
#include "vr.h"

#ifndef HALOMCCVR_HALO2_OBSERVER_6DOF
#define HALOMCCVR_HALO2_OBSERVER_6DOF 0
#endif

#if HALOMCCVR_HALO2_OBSERVER_6DOF

namespace
{
    // void __fastcall observer_final_transform(int user_index)
    using Halo2ObserverFinalTransformFn = void(__fastcall*)(uint32_t);

    // Unique in the complete mapped image of both pinned retail editions.
    // Prologue plus `movsxd rbx,ecx` / `imul rdi,rbx,0x368`, so the observer
    // stride is inside the signature and a moved array cannot silently change
    // the element size. The `mov rax,[rip+disp32]` at +0x1A resolves the
    // director-space matrix pointer slot at RVA 0xDFCB58, which is verified as
    // a second, independent identity check on the match.
    constexpr char kFinalTransformPattern[] =
        "48 89 5C 24 18 48 89 74 24 20 55 57 41 57 48 8D 6C 24 D0 "
        "48 81 EC 30 01 00 00 48 8B 05 ?? ?? ?? ?? 48 63 D9 "
        "48 69 FB 68 03 00 00";
    constexpr uint32_t kFinalTransformMatrixDispOffset = 0x1D;
    constexpr uint32_t kFinalTransformMatrixNextOffset = 0x21;
    constexpr uint32_t kFinalTransformMatrixSlotRva = 0x00DFCB58;

    // Only the primary player's observer is owned. Split-screen guests keep
    // their stock camera rather than inheriting one headset's pose.
    constexpr uint32_t kOwnedUser = 0;

    // E-H2-23: the frame's first-person weapon placement.
    using Halo2FirstPersonWeaponsFn = void(__fastcall*)(
        uint32_t, uint32_t, uint8_t);

    enum class CoreState : uint8_t
    {
        StockFallback = 0,
        CleanupRequired,
        Installed,
    };

    struct HeadReference
    {
        float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
        float position[3]{};
    };

    std::atomic<bool> g_installed{false};
    std::atomic<bool> g_armed{false};
    std::atomic<bool> g_levelLive{false};
    std::atomic<bool> g_teardownRequested{false};
    std::atomic<bool> g_recenterRequested{true};
    std::atomic<bool> g_referenceValid{false};
    std::atomic<uint32_t> g_generation{0};
    std::atomic<uint32_t> g_vrFailureGeneration{0};
    std::atomic<uintptr_t> g_moduleBase{0};
    std::atomic<uintptr_t> g_originalAddress{0};
    std::atomic<uintptr_t> g_observerResult{0};
    std::atomic<uint32_t> g_activeCallbacks{0};

    // E-H2-23 (C-H2-31): the weapon tick witness. first_person_weapons runs
    // at the game tick (~60/s, the C-H2-30 log: 1890 placements against ~200
    // observer updates per second) and places the weapon against the observer
    // record as it stands at that moment - the pose the mod published at the
    // last observer update before the tick. The witness records WHICH
    // publication that was (its serial), and the interpolator's first-person
    // slots are reset after every placement so the weapon the renderer draws
    // is that tick's placement and not a blend towards the previous one. The
    // Anniversary core renders the eyes from that publication and submits its
    // poses, so weapon and world are one pose and the compositor reprojects
    // both together. The observer update itself is UNTOUCHED (C-H2-30 fed it
    // the tick's sample and cut the camera to tick rate).
    void* g_weaponsTarget = nullptr;
    std::atomic<uintptr_t> g_weaponsOriginal{0};
    std::atomic<uint32_t> g_weaponsActiveCallbacks{0};
    std::atomic<uint64_t> g_weaponsCalls{0};
    std::atomic<uint64_t> g_weaponsWitnessed{0};
    std::atomic<uint64_t> g_weaponsRecordForeign{0};
    std::atomic<uint64_t> g_weaponsNoPublication{0};

    // generation << 32 is not enough for a 64-bit serial; two atomics, the
    // serial written last (release) and read first (acquire).
    std::atomic<uint32_t> g_weaponTickGeneration{0};
    std::atomic<uint64_t> g_weaponTickIndex{0};
    std::atomic<uint64_t> g_publicationIndex{0};
    std::atomic<uintptr_t> g_interpolatorResetAddress{0};
    uint64_t g_lastWitnessedReported = 0;

    // E-H2-6 publication: seqlock (even = stable), plain payload.
    std::atomic<uint32_t> g_publicationVersion{0};
    Halo2ObserverPosePublication g_publication{};
    std::atomic<uint64_t> g_publicationTornReads{0};
    // E-H2-21: the last few publications, newest first, so a renderer on
    // another thread can pick the sample the engine's OWN frame camera was
    // built from instead of whatever was published most recently.
    constexpr unsigned kPublicationRing = 8;
    std::atomic<uint32_t> g_ringVersion[kPublicationRing]{};
    Halo2ObserverPosePublication g_ring[kPublicationRing]{};
    std::atomic<unsigned> g_ringHead{0};

    void PublishPose(
        uint32_t generation, uint64_t serial, const Halo2CameraBasis& stock,
        const Halo2CameraBasis& tracked, const HeadReference& reference,
        const Halo2SynchronousVrRenderSnapshot& sample) noexcept
    {
        g_publicationVersion.fetch_add(1, std::memory_order_acq_rel);
        g_publication.generation = generation;
        g_publication.serial = serial;
        g_publication.index =
            g_publicationIndex.fetch_add(1, std::memory_order_relaxed) + 1;
        g_publication.stock = stock;
        g_publication.tracked = tracked;
        std::memcpy(g_publication.referenceOrientation, reference.orientation,
                    sizeof(g_publication.referenceOrientation));
        std::memcpy(g_publication.referencePosition, reference.position,
                    sizeof(g_publication.referencePosition));
        // E-H2-18: the sample this tracked camera was composed from, so the
        // Saber scene (another thread, another serial) renders the eyes from
        // the head pose the weapon was placed against and submits them as
        // that pose.
        Halo2ObserverPoseSnapshot& snapshot = g_publication.snapshot;
        snapshot.valid = sample.headPoseValid;
        std::memcpy(snapshot.headOrientation, sample.headOrientation,
                    sizeof(snapshot.headOrientation));
        std::memcpy(snapshot.headPosition, sample.headPosition,
                    sizeof(snapshot.headPosition));
        for (int eye = 0; eye < 2; ++eye)
        {
            std::memcpy(snapshot.eyeOffsetPosition[eye], sample.eyes[eye].position,
                        sizeof(snapshot.eyeOffsetPosition[eye]));
            std::memcpy(snapshot.eyeOffsetOrientation[eye],
                        sample.eyes[eye].orientation,
                        sizeof(snapshot.eyeOffsetOrientation[eye]));
            std::memcpy(snapshot.eyePosition[eye], sample.eyes[eye].absolutePosition,
                        sizeof(snapshot.eyePosition[eye]));
            std::memcpy(snapshot.eyeOrientation[eye],
                        sample.eyes[eye].absoluteOrientation,
                        sizeof(snapshot.eyeOrientation[eye]));
        }
        g_publicationVersion.fetch_add(1, std::memory_order_acq_rel);
        // The ring keeps one entry per DISTINCT tracked camera (the observer
        // runs many times per frame with the same sample).
        const unsigned head = g_ringHead.load(std::memory_order_acquire);
        const unsigned last = (head + kPublicationRing - 1) % kPublicationRing;
        const bool same = head != 0 &&
            std::memcmp(&g_ring[last].tracked, &tracked, sizeof(tracked)) == 0 &&
            g_ring[last].serial == serial;
        const unsigned slot = same ? last : head % kPublicationRing;
        g_ringVersion[slot].fetch_add(1, std::memory_order_acq_rel);
        g_ring[slot] = g_publication;
        g_ringVersion[slot].fetch_add(1, std::memory_order_acq_rel);
        if (!same)
            g_ringHead.store(head + 1, std::memory_order_release);
    }

    void ClearPublication() noexcept
    {
        g_publicationVersion.fetch_add(1, std::memory_order_acq_rel);
        g_publication = {};
        g_publicationVersion.fetch_add(1, std::memory_order_acq_rel);
        for (unsigned i = 0; i < kPublicationRing; ++i)
        {
            g_ringVersion[i].fetch_add(1, std::memory_order_acq_rel);
            g_ring[i] = {};
            g_ringVersion[i].fetch_add(1, std::memory_order_acq_rel);
        }
        g_ringHead.store(0, std::memory_order_release);
    }

    std::atomic<uint64_t> g_callbacks{0};
    std::atomic<uint64_t> g_appliedPoses{0};
    std::atomic<uint64_t> g_rejectedSamples{0};
    std::atomic<uint64_t> g_unreadableSamples{0};
    std::atomic<uint64_t> g_exceptions{0};
    uint64_t g_lastReportMs = 0;
    uint64_t g_lastAppliedReported = 0;
    uint64_t g_lastCallbacks = 0;

    // Written and read only from the owned detour, which the engine calls on
    // one thread inside observer_update_all.
    HeadReference g_reference{};

    HMODULE g_moduleReference = nullptr;
    void* g_target = nullptr;
    CoreState g_coreState = CoreState::StockFallback;
    uint32_t g_rejectedGeneration = 0;
    uint32_t g_armedLoggedGeneration = 0;

    bool ReadFloats(uintptr_t address, float* out, size_t count) noexcept
    {
        __try
        {
            for (size_t index = 0; index < count; ++index)
            {
                out[index] =
                    *reinterpret_cast<const volatile float*>(
                        address + index * sizeof(float));
                if (!std::isfinite(out[index]))
                    return false;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool WriteFloats(
        uintptr_t address, const float* values, size_t count) noexcept
    {
        for (size_t index = 0; index < count; ++index)
            if (!std::isfinite(values[index]))
                return false;
        __try
        {
            for (size_t index = 0; index < count; ++index)
            {
                *reinterpret_cast<volatile float*>(
                    address + index * sizeof(float)) = values[index];
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ValidHeadPose(
        const float orientation[4], const float position[3]) noexcept
    {
        float lengthSquared = 0.0f;
        for (int index = 0; index < 4; ++index)
        {
            if (!std::isfinite(orientation[index]))
                return false;
            lengthSquared += orientation[index] * orientation[index];
        }
        if (!std::isfinite(lengthSquared) ||
            std::fabs(lengthSquared - 1.0f) > 0.05f)
        {
            return false;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!std::isfinite(position[axis]) ||
                std::fabs(position[axis]) > kHalo2MaxHeadPositionMeters)
            {
                return false;
            }
        }
        return true;
    }

    bool ReadObserverPose(
        uintptr_t result, Halo2CameraBasis& basis) noexcept
    {
        return ReadFloats(
                   result + kHalo2ObserverResultPositionOffset,
                   basis.position, 3) &&
            ReadFloats(
                   result + kHalo2ObserverResultForwardOffset,
                   basis.forward, 3) &&
            ReadFloats(
                   result + kHalo2ObserverResultUpOffset, basis.up, 3) &&
            Halo2ValidateCameraBasis(basis);
    }

    // The three owned spans are written independently and each is checked
    // against the allow-list, so no future edit can widen this into a
    // whole-struct copy over engine-owned fields.
    bool WriteObserverPose(
        uintptr_t result, const Halo2CameraBasis& basis) noexcept
    {
        if (!Halo2ValidateCameraBasis(basis))
            return false;
        struct Span
        {
            uint32_t offset;
            const float* values;
        };
        const Span spans[kHalo2ObserverOwnedSpanCount] = {
            {kHalo2ObserverResultPositionOffset, basis.position},
            {kHalo2ObserverResultForwardOffset, basis.forward},
            {kHalo2ObserverResultUpOffset, basis.up},
        };
        for (const Span& span : spans)
        {
            if (!Halo2ObserverSpanWriteAllowed(
                    span.offset, kHalo2CameraVectorBytes))
            {
                return false;
            }
            if (!WriteFloats(result + span.offset, span.values, 3))
                return false;
        }
        return true;
    }

    uintptr_t OriginalAddress() noexcept
    {
        return g_originalAddress.load(std::memory_order_acquire);
    }

    void ApplyHeadPose(uintptr_t result) noexcept
    {
        Halo2SynchronousVrRenderSnapshot snapshot{};
        bool snapshotReady = false;
        __try
        {
            snapshotReady = VR_Halo2GetSynchronousRenderSnapshot(snapshot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            snapshotReady = false;
        }
        if (!snapshotReady || !snapshot.headPoseValid ||
            !ValidHeadPose(snapshot.headOrientation, snapshot.headPosition))
        {
            g_rejectedSamples.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        Halo2CameraBasis stock{};
        if (!ReadObserverPose(result, stock))
        {
            g_unreadableSamples.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (g_recenterRequested.exchange(false, std::memory_order_acq_rel) ||
            !g_referenceValid.load(std::memory_order_acquire))
        {
            std::memcpy(
                g_reference.orientation, snapshot.headOrientation,
                sizeof(g_reference.orientation));
            std::memcpy(
                g_reference.position, snapshot.headPosition,
                sizeof(g_reference.position));
            g_referenceValid.store(true, std::memory_order_release);
        }

        Halo2TrackedHeadInput head{};
        std::memcpy(
            head.orientation, snapshot.headOrientation,
            sizeof(head.orientation));
        std::memcpy(
            head.position, snapshot.headPosition, sizeof(head.position));
        std::memcpy(
            head.referenceOrientation, g_reference.orientation,
            sizeof(head.referenceOrientation));
        std::memcpy(
            head.referencePosition, g_reference.position,
            sizeof(head.referencePosition));
        head.positional = Game_IsPositionalTracking();
        head.worldScale = Game_GetWorldScale();

        Halo2CameraBasis tracked{};
        if (!Halo2BuildTrackedCenterCamera(stock, head, tracked))
        {
            g_rejectedSamples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!WriteObserverPose(result, tracked))
        {
            g_unreadableSamples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        PublishPose(
            g_generation.load(std::memory_order_acquire),
            snapshot.preparedSerial, stock, tracked, g_reference, snapshot);
        g_appliedPoses.fetch_add(1, std::memory_order_relaxed);
    }

    // E-H2-23 (C-H2-31). Runs around first_person_weapons. Before the
    // placement: record which publication the observer record holds right
    // now - that is the pose the weapon is about to be placed against. The
    // record is compared bit for bit with the newest publication's tracked
    // pose; if the engine moved it since, the tick is counted as foreign and
    // no witness is published for it. After the placement: reset the
    // interpolator's four first-person slots for the owned player (the
    // engine's own reset, pinned by entry bytes; a no-op while the
    // interpolator is unallocated), so the renderer draws this tick's
    // placement and not a blend towards the previous tick's.
    void WitnessWeaponTick() noexcept
    {
        const uint32_t generation = g_generation.load(std::memory_order_acquire);
        if (!generation)
            return;
        const uintptr_t base = g_observerResult.load(std::memory_order_acquire);
        if (!base)
            return;
        const uintptr_t result = base + Halo2ObserverRecordOffset(kOwnedUser);
        Halo2ObserverPosePublication published{};
        if (!Halo2Observer6Dof_ReadPublishedPose(published) ||
            published.generation != generation || !published.index)
        {
            // No witness for this tick: the previous one must not stand, or
            // the eyes would render from a pose older than both the weapon
            // and the frame camera until it left the ring.
            g_weaponTickIndex.store(0, std::memory_order_release);
            g_weaponsNoPublication.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Halo2CameraBasis current{};
        if (!ReadObserverPose(result, current) ||
            !Halo2CameraBasisMatchesExactly(current, published.tracked))
        {
            g_weaponTickIndex.store(0, std::memory_order_release);
            g_weaponsRecordForeign.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_weaponTickGeneration.store(generation, std::memory_order_relaxed);
        g_weaponTickIndex.store(published.index, std::memory_order_release);
        g_weaponsWitnessed.fetch_add(1, std::memory_order_relaxed);
    }

    __declspec(noinline) void __fastcall Halo2FirstPersonWeaponsDetour(
        uint32_t a, uint32_t b, uint8_t c)
    {
        // Count the callback BEFORE taking the trampoline, so RemoveCore's
        // drain cannot free it under a thread that already holds it.
        g_weaponsActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
        const auto original = reinterpret_cast<Halo2FirstPersonWeaponsFn>(
            g_weaponsOriginal.load(std::memory_order_acquire));
        if (!original)
        {
            g_weaponsActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }
        g_weaponsCalls.fetch_add(1, std::memory_order_relaxed);
        const bool owned = g_armed.load(std::memory_order_acquire) &&
            g_levelLive.load(std::memory_order_acquire) &&
            !g_teardownRequested.load(std::memory_order_acquire);
        if (owned)
        {
            __try { WitnessWeaponTick(); }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_exceptions.fetch_add(1, std::memory_order_relaxed);
            }
        }
        __try { original(a, b, c); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_exceptions.fetch_add(1, std::memory_order_relaxed);
        }
        g_weaponsActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    void DetourBody(uint32_t user) noexcept
    {
        const auto original =
            reinterpret_cast<Halo2ObserverFinalTransformFn>(OriginalAddress());
        if (!original)
            return;

        // The engine's own transform always runs first and unchanged. The mod
        // only post-processes its result, so a withheld injection is exactly
        // stock behavior rather than a missing camera.
        original(user);
        g_callbacks.fetch_add(1, std::memory_order_relaxed);

        if (user != kOwnedUser)
            return;
        if (!g_armed.load(std::memory_order_acquire) ||
            !g_levelLive.load(std::memory_order_acquire) ||
            g_teardownRequested.load(std::memory_order_acquire))
        {
            return;
        }
        const uintptr_t base = g_observerResult.load(std::memory_order_acquire);
        if (!base)
            return;
        ApplyHeadPose(base + Halo2ObserverRecordOffset(user));
    }

    __declspec(noinline) void __fastcall Halo2ObserverFinalTransformDetour(
        uint32_t user)
    {
        g_activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        __try
        {
            __try
            {
                DetourBody(user);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_exceptions.fetch_add(1, std::memory_order_relaxed);
            }
        }
        __finally
        {
            g_activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    bool IsReadableProtection(DWORD protect) noexcept
    {
        if (protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        const DWORD mask = protect & 0xFF;
        return mask == PAGE_READONLY || mask == PAGE_READWRITE ||
            mask == PAGE_WRITECOPY || mask == PAGE_EXECUTE_READ ||
            mask == PAGE_EXECUTE_READWRITE || mask == PAGE_EXECUTE_WRITECOPY;
    }

    int HexNibble(char value) noexcept
    {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    }

    struct FixedPattern
    {
        static constexpr size_t kMaxLength = 96;
        uint8_t bytes[kMaxLength]{};
        uint8_t wild[kMaxLength]{};
        size_t length = 0;
    };

    bool CompilePattern(const char* text, FixedPattern& out) noexcept
    {
        out.length = 0;
        for (const char* cursor = text; *cursor;)
        {
            if (*cursor == ' ')
            {
                ++cursor;
                continue;
            }
            if (out.length >= FixedPattern::kMaxLength)
                return false;
            if (cursor[0] == '?')
            {
                out.bytes[out.length] = 0;
                out.wild[out.length] = 1;
                ++out.length;
                ++cursor;
                if (*cursor == '?')
                    ++cursor;
                continue;
            }
            const int high = HexNibble(cursor[0]);
            const int low = cursor[1] ? HexNibble(cursor[1]) : -1;
            if (high < 0 || low < 0)
                return false;
            out.bytes[out.length] =
                static_cast<uint8_t>((high << 4) | low);
            out.wild[out.length] = 0;
            ++out.length;
            cursor += 2;
        }
        return out.length != 0;
    }

    // Region-walking and SEH-guarded: halo2.dll's mapped image contains gaps,
    // and a raw sweep across them faults.
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
                    const size_t last =
                        (regionEnd - cursor) - compiled.length;
                    for (size_t offset = 0; offset <= last; ++offset)
                    {
                        size_t index = 0;
                        for (; index < compiled.length; ++index)
                        {
                            if (!compiled.wild[index] &&
                                data[offset + index] != compiled.bytes[index])
                            {
                                break;
                            }
                        }
                        if (index != compiled.length)
                            continue;
                        if (!firstMatch)
                            firstMatch = cursor + offset;
                        if (++matchCount >= 2)
                            return true;
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

    bool DecodeRipRelative(
        uintptr_t match, uint32_t dispOffset, uint32_t nextOffset,
        uintptr_t& target) noexcept
    {
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

    bool DrainCallbacks() noexcept
    {
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            if (!g_activeCallbacks.load(std::memory_order_acquire))
                return true;
            Sleep(10);
        }
        return false;
    }

    bool RemoveCore(const char* reason) noexcept
    {
        g_armed.store(false, std::memory_order_release);
        g_teardownRequested.store(true, std::memory_order_release);
        if (g_target)
        {
            const MH_STATUS disabled = MH_DisableHook(g_target);
            if (disabled != MH_OK && disabled != MH_ERROR_NOT_CREATED &&
                disabled != MH_ERROR_DISABLED)
            {
                LOG("Halo 2 observer 6DOF: disable failed (%d); ownership "
                    "retained for cleanup", static_cast<int>(disabled));
                return false;
            }
            if (!DrainCallbacks())
            {
                LOG("Halo 2 observer 6DOF: callbacks did not drain; "
                    "ownership retained for cleanup");
                return false;
            }
            const MH_STATUS removed = MH_RemoveHook(g_target);
            if (removed != MH_OK && removed != MH_ERROR_NOT_CREATED)
            {
                LOG("Halo 2 observer 6DOF: remove failed (%d); ownership "
                    "retained for cleanup", static_cast<int>(removed));
                return false;
            }
            g_target = nullptr;
        }
        if (g_weaponsTarget)
        {
            const MH_STATUS disabled = MH_DisableHook(g_weaponsTarget);
            if (disabled != MH_OK && disabled != MH_ERROR_NOT_CREATED &&
                disabled != MH_ERROR_DISABLED)
            {
                LOG("Halo 2 observer 6DOF: first-person weapons disable "
                    "failed (%d); ownership retained for cleanup",
                    static_cast<int>(disabled));
                return false;
            }
            for (int attempt = 0; attempt < 200 &&
                 g_weaponsActiveCallbacks.load(std::memory_order_acquire);
                 ++attempt)
            {
                Sleep(10);
            }
            if (g_weaponsActiveCallbacks.load(std::memory_order_acquire))
            {
                LOG("Halo 2 observer 6DOF: first-person weapons callbacks did "
                    "not drain; ownership retained for cleanup");
                return false;
            }
            (void)MH_RemoveHook(g_weaponsTarget);
            g_weaponsTarget = nullptr;
        }
        g_weaponsOriginal.store(0, std::memory_order_release);
        g_interpolatorResetAddress.store(0, std::memory_order_release);
        g_weaponTickIndex.store(0, std::memory_order_release);
        g_weaponTickGeneration.store(0, std::memory_order_release);
        g_originalAddress.store(0, std::memory_order_release);
        g_observerResult.store(0, std::memory_order_release);
        g_moduleBase.store(0, std::memory_order_release);
        g_generation.store(0, std::memory_order_release);
        g_installed.store(false, std::memory_order_release);
        g_referenceValid.store(false, std::memory_order_release);
        g_recenterRequested.store(true, std::memory_order_release);
        g_coreState = CoreState::StockFallback;
        if (g_moduleReference)
        {
            FreeLibrary(g_moduleReference);
            g_moduleReference = nullptr;
        }
        if (reason)
            LOG("Halo 2 observer 6DOF removed (%s); stock camera restored",
                reason);
        return true;
    }

    bool InstallCore(
        uintptr_t base, size_t size, uint32_t generation,
        uintptr_t observerResultArray) noexcept
    {
        if (g_rejectedGeneration == generation)
            return false;

        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(base), &module) ||
            !module)
        {
            return false;
        }

        uintptr_t match = 0;
        uint32_t matchCount = 0;
        if (!CountPatternMatches(
                base, size, kFinalTransformPattern, match, matchCount) ||
            matchCount != 1 || match != base + kHalo2ObserverFinalTransformRva)
        {
            LOG("Halo 2 observer 6DOF WITHHELD: the observer final-transform "
                "signature matched %u times%s; the stock camera is untouched",
                matchCount,
                (matchCount == 1 && match != base +
                     kHalo2ObserverFinalTransformRva)
                    ? " but moved from its pinned RVA" : "");
            FreeLibrary(module);
            g_rejectedGeneration = generation;
            return false;
        }

        // Second, independent identity check on the same match.
        uintptr_t matrixSlot = 0;
        if (!DecodeRipRelative(
                match, kFinalTransformMatrixDispOffset,
                kFinalTransformMatrixNextOffset, matrixSlot) ||
            matrixSlot != base + kFinalTransformMatrixSlotRva)
        {
            LOG("Halo 2 observer 6DOF WITHHELD: the director-space matrix "
                "slot decoded to RVA 0x%llX instead of 0x%X; the stock camera "
                "is untouched",
                matrixSlot ? static_cast<unsigned long long>(matrixSlot - base)
                           : 0ull,
                kFinalTransformMatrixSlotRva);
            FreeLibrary(module);
            g_rejectedGeneration = generation;
            return false;
        }

        if (!observerResultArray ||
            observerResultArray != base + kHalo2ObserverResultArrayRva)
        {
            LOG("Halo 2 observer 6DOF WITHHELD: the observer result array was "
                "not signature-proven for this generation; the stock camera "
                "is untouched");
            FreeLibrary(module);
            g_rejectedGeneration = generation;
            return false;
        }

        void* const target = reinterpret_cast<void*>(match);
        void* trampoline = nullptr;
        const MH_STATUS created = MH_CreateHook(
            target,
            reinterpret_cast<void*>(&Halo2ObserverFinalTransformDetour),
            &trampoline);
        if (created != MH_OK || !trampoline)
        {
            LOG("Halo 2 observer 6DOF WITHHELD: hook create=%d",
                static_cast<int>(created));
            if (created == MH_OK)
                (void)MH_RemoveHook(target);
            FreeLibrary(module);
            g_rejectedGeneration = generation;
            return false;
        }

        g_moduleReference = module;
        g_target = target;
        g_originalAddress.store(
            reinterpret_cast<uintptr_t>(trampoline),
            std::memory_order_release);
        g_observerResult.store(
            observerResultArray, std::memory_order_release);
        g_moduleBase.store(base, std::memory_order_release);
        g_generation.store(generation, std::memory_order_release);
        g_teardownRequested.store(false, std::memory_order_release);
        g_referenceValid.store(false, std::memory_order_release);
        g_recenterRequested.store(true, std::memory_order_release);
        g_installed.store(true, std::memory_order_release);
        g_coreState = CoreState::CleanupRequired;

        const MH_STATUS enabled = MH_EnableHook(target);
        if (enabled != MH_OK)
        {
            LOG("Halo 2 observer 6DOF WITHHELD: hook enable=%d; rolling back",
                static_cast<int>(enabled));
            if (!RemoveCore("install rollback"))
                g_coreState = CoreState::CleanupRequired;
            g_rejectedGeneration = generation;
            return false;
        }

        // E-H2-23: the weapon tick witness. first_person_weapons is
        // identified by being the SINGLE caller of the interpolator's
        // first-person slot reset, so the reset's entry bytes are still
        // checked as the identity proof - C-H2-34 never calls it. Killing the
        // interpolation was what pinned the weapon to the 60 Hz game tick;
        // the weapon's own view (E-H2-26) cancels the head motion exactly, so
        // the engine's blending stays on and the weapon stays smooth at
        // whatever frame rate the player runs.
        const uintptr_t resetAddress =
            base + kHalo2FrameInterpolatorResetFirstPersonSlotRva;
        uint8_t resetBytes[sizeof(kHalo2FrameInterpolatorResetFirstPersonSlotEntryBytes)]{};
        bool resetReadable = false;
        __try
        {
            std::memcpy(
                resetBytes, reinterpret_cast<const void*>(resetAddress),
                sizeof(resetBytes));
            resetReadable = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { resetReadable = false; }
        const bool resetPinned = resetReadable &&
            std::memcmp(
                resetBytes, kHalo2FrameInterpolatorResetFirstPersonSlotEntryBytes,
                sizeof(resetBytes)) == 0;
        g_interpolatorResetAddress.store(
            resetPinned ? resetAddress : 0, std::memory_order_release);
        const uintptr_t weapons = base + kHalo2FirstPersonWeaponsRva;
        uint8_t weaponsBytes[sizeof(kHalo2FirstPersonWeaponsEntryBytes)]{};
        bool weaponsReadable = false;
        __try
        {
            std::memcpy(
                weaponsBytes, reinterpret_cast<const void*>(weapons),
                sizeof(weaponsBytes));
            weaponsReadable = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { weaponsReadable = false; }
        if (!weaponsReadable ||
            std::memcmp(
                weaponsBytes, kHalo2FirstPersonWeaponsEntryBytes,
                sizeof(weaponsBytes)) != 0)
        {
            LOG("Halo 2 weapon tick witness WITHHELD: the entry bytes at "
                "+0x%X are not first_person_weapons in this module; the "
                "Anniversary weapon stays placed against a pose the eyes "
                "cannot name",
                static_cast<unsigned>(kHalo2FirstPersonWeaponsRva));
        }
        else
        {
            void* const weaponsTarget = reinterpret_cast<void*>(weapons);
            void* weaponsTrampoline = nullptr;
            const MH_STATUS weaponsCreated = MH_CreateHook(
                weaponsTarget,
                reinterpret_cast<void*>(&Halo2FirstPersonWeaponsDetour),
                &weaponsTrampoline);
            if (weaponsCreated != MH_OK || !weaponsTrampoline)
            {
                LOG("Halo 2 weapon tick witness WITHHELD: hook create=%d",
                    static_cast<int>(weaponsCreated));
                if (weaponsCreated == MH_OK)
                    (void)MH_RemoveHook(weaponsTarget);
            }
            else
            {
                g_weaponsTarget = weaponsTarget;
                g_weaponsOriginal.store(
                    reinterpret_cast<uintptr_t>(weaponsTrampoline),
                    std::memory_order_release);
                const MH_STATUS weaponsEnabled = MH_EnableHook(weaponsTarget);
                if (weaponsEnabled != MH_OK)
                {
                    LOG("Halo 2 weapon tick witness WITHHELD: hook enable=%d",
                        static_cast<int>(weaponsEnabled));
                    (void)MH_RemoveHook(weaponsTarget);
                    g_weaponsTarget = nullptr;
                    g_weaponsOriginal.store(0, std::memory_order_release);
                }
                else
                {
                    LOG("Halo 2 weapon tick witness installed on "
                        "first_person_weapons +0x%X (the game tick's weapon "
                        "placement, the only caller of the interpolator's "
                        "first-person slot reset +0x%X, %s): every tick "
                        "records which observer publication the weapon was "
                        "placed against%s; the observer update itself is "
                        "untouched",
                        static_cast<unsigned>(kHalo2FirstPersonWeaponsRva),
                        static_cast<unsigned>(
                            kHalo2FrameInterpolatorResetFirstPersonSlotRva),
                        resetPinned ? "identity proven"
                                    : "identity NOT proven",
                        "; the engine's inter-tick blending is left ON so the "
                        "weapon stays smooth at the player's frame rate, and "
                        "the weapon's own view cancels the head motion");
                }
            }
        }

        g_coreState = CoreState::Installed;
        LOG("Halo 2 observer 6DOF installed: observer final transform "
            "+0x%X, observer results +0x%X stride 0x%X, user %u. Three "
            "12-byte spans (position +0x%X, forward +0x%X, up +0x%X) are "
            "written after the engine's own transform; field of view, "
            "aspect and every other observer field stay engine-owned",
            static_cast<unsigned>(kHalo2ObserverFinalTransformRva),
            static_cast<unsigned>(kHalo2ObserverResultArrayRva),
            static_cast<unsigned>(kHalo2ObserverStride), kOwnedUser,
            static_cast<unsigned>(kHalo2ObserverResultPositionOffset),
            static_cast<unsigned>(kHalo2ObserverResultForwardOffset),
            static_cast<unsigned>(kHalo2ObserverResultUpOffset));
        return true;
    }

    void ReportTelemetry() noexcept
    {
        const uint64_t now = GetTickCount64();
        if (now - g_lastReportMs < 5000)
            return;
        g_lastReportMs = now;
        const uint64_t applied = g_appliedPoses.load(std::memory_order_relaxed);
        // Report even when nothing is being applied. A silent zero is exactly
        // the failure this telemetry exists to expose: it distinguishes "the
        // hook never fired" from "the hook fired but every head sample was
        // rejected", which look identical from the player's seat.
        static bool reportedOnce = false;
        const uint64_t witnessed = g_weaponsWitnessed.load(std::memory_order_relaxed);
        if (reportedOnce && applied == g_lastAppliedReported &&
            g_callbacks.load(std::memory_order_relaxed) == g_lastCallbacks &&
            witnessed == g_lastWitnessedReported)
        {
            return;
        }
        g_lastWitnessedReported = witnessed;
        reportedOnce = true;
        g_lastAppliedReported = applied;
        g_lastCallbacks = g_callbacks.load(std::memory_order_relaxed);
        // The measured lean: how far the camera the engine now holds sits
        // from the camera it computed, in metres. This is the number that
        // says whether the head moves the world, not the applied count.
        Halo2ObserverPosePublication published{};
        float leanMeters = -1.0f;
        if (Halo2Observer6Dof_ReadPublishedPose(published))
        {
            float sum = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float d = published.tracked.position[axis] -
                    published.stock.position[axis];
                sum += d * d;
            }
            leanMeters = std::sqrt(sum) * kHalo2MetersPerWorldUnit;
        }
        LOG("Halo 2 observer 6DOF: %llu observer transforms seen, %llu head "
            "poses applied, %llu rejected samples, %llu unreadable, %llu "
            "exceptions; lean now %.3f m at world_scale %.3f wu/m (positional "
            "%s); weapon tick witness: %llu placements, %llu witnessed, %llu "
            "with a record the engine had moved, %llu with no publication; "
            "the engine's inter-tick blending is left ON; last witnessed "
            "publication #%llu",
            static_cast<unsigned long long>(
                g_callbacks.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(applied),
            static_cast<unsigned long long>(
                g_rejectedSamples.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_unreadableSamples.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_exceptions.load(std::memory_order_relaxed)),
            leanMeters, Game_GetWorldScale(),
            Game_IsPositionalTracking() ? "ON" : "OFF",
            static_cast<unsigned long long>(
                g_weaponsCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponsWitnessed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponsRecordForeign.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponsNoPublication.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponTickIndex.load(std::memory_order_relaxed)));
    }
}

bool Halo2Observer6Dof_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed,
    uintptr_t observerResultArray) noexcept
{
    uint32_t vrFailureGeneration =
        g_vrFailureGeneration.load(std::memory_order_acquire);
    if (vrFailureGeneration && generation && generation != vrFailureGeneration)
    {
        g_vrFailureGeneration.compare_exchange_strong(
            vrFailureGeneration, 0, std::memory_order_acq_rel,
            std::memory_order_acquire);
        vrFailureGeneration =
            g_vrFailureGeneration.load(std::memory_order_acquire);
    }

    const bool identityValid = moduleBase && generation &&
        moduleSize == kHalo2RetailImageSize;
    const bool vrAvailable =
        !vrFailureGeneration || generation != vrFailureGeneration;
    const bool desired = identityValid && activeAndRange && levelRunning &&
        coldPassed && vrAvailable && observerResultArray != 0;

    g_levelLive.store(levelRunning, std::memory_order_release);

    const uint32_t owned = g_generation.load(std::memory_order_acquire);
    const bool ownsDifferentModule = g_target &&
        (owned != generation ||
         g_moduleBase.load(std::memory_order_acquire) != moduleBase);

    if (!desired || ownsDifferentModule)
    {
        if (g_installed.load(std::memory_order_acquire) ||
            g_coreState != CoreState::StockFallback)
        {
            (void)RemoveCore(
                ownsDifferentModule ? "module generation changed"
                                    : "level or title no longer eligible");
        }
        if (generation != g_rejectedGeneration)
            g_rejectedGeneration = 0;
        return false;
    }

    if (g_coreState != CoreState::Installed)
    {
        if (!InstallCore(
                moduleBase, moduleSize, generation, observerResultArray))
        {
            return false;
        }
    }

    g_armed.store(true, std::memory_order_release);
    if (g_armedLoggedGeneration != generation)
    {
        g_armedLoggedGeneration = generation;
        LOG("Halo 2 observer 6DOF armed: the headset now owns the Halo 2 "
            "camera's position and orientation in BOTH graphics modes, "
            "because the observer is the one camera root the classic Blam "
            "renderer and the remastered Anniversary renderer both consume");
    }
    ReportTelemetry();
    return true;
}

bool Halo2Observer6Dof_Installed() noexcept
{
    return g_installed.load(std::memory_order_acquire);
}

bool Halo2Observer6Dof_Armed() noexcept
{
    return g_armed.load(std::memory_order_acquire);
}

bool Halo2Observer6Dof_ReadPublishedPose(
    Halo2ObserverPosePublication& out) noexcept
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t before =
            g_publicationVersion.load(std::memory_order_acquire);
        if (before & 1u)
            continue;
        Halo2ObserverPosePublication candidate = g_publication;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_publicationVersion.load(std::memory_order_acquire) != before)
            continue;
        if (!candidate.generation || !candidate.serial ||
            !g_armed.load(std::memory_order_acquire))
        {
            return false;
        }
        out = candidate;
        return true;
    }
    g_publicationTornReads.fetch_add(1, std::memory_order_relaxed);
    return false;
}

int Halo2Observer6Dof_ReadPublishedPoses(
    Halo2ObserverPosePublication* out, int maximum) noexcept
{
    if (!out || maximum <= 0 || !g_armed.load(std::memory_order_acquire))
        return 0;
    const unsigned head = g_ringHead.load(std::memory_order_acquire);
    int count = 0;
    for (unsigned back = 1; back <= kPublicationRing && back <= head &&
         count < maximum; ++back)
    {
        const unsigned slot = (head - back) % kPublicationRing;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const uint32_t before = g_ringVersion[slot].load(std::memory_order_acquire);
            if (before & 1u)
                continue;
            Halo2ObserverPosePublication candidate = g_ring[slot];
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_ringVersion[slot].load(std::memory_order_acquire) != before)
                continue;
            if (candidate.generation && candidate.serial)
                out[count++] = candidate;
            break;
        }
    }
    return count;
}

bool Halo2Observer6Dof_WeaponTickPublication(
    uint32_t generation, uint64_t& index) noexcept
{
    index = g_weaponTickIndex.load(std::memory_order_acquire);
    return index != 0 && generation != 0 &&
        g_weaponTickGeneration.load(std::memory_order_relaxed) == generation;
}

void Halo2Observer6Dof_RequestRecenter() noexcept
{
    g_recenterRequested.store(true, std::memory_order_release);
}

void Halo2Observer6Dof_ShutdownForVrFailure() noexcept
{
    const uint32_t generation = g_generation.load(std::memory_order_acquire);
    if (generation)
        g_vrFailureGeneration.store(generation, std::memory_order_release);
    g_armed.store(false, std::memory_order_release);
    g_teardownRequested.store(true, std::memory_order_release);
}

#else

bool Halo2Observer6Dof_Poll(
    uintptr_t, size_t, uint32_t, bool, bool, bool, uintptr_t) noexcept
{
    return false;
}

bool Halo2Observer6Dof_Installed() noexcept { return false; }
bool Halo2Observer6Dof_Armed() noexcept { return false; }
bool Halo2Observer6Dof_ReadPublishedPose(
    Halo2ObserverPosePublication&) noexcept { return false; }
int Halo2Observer6Dof_ReadPublishedPoses(
    Halo2ObserverPosePublication*, int) noexcept { return 0; }
bool Halo2Observer6Dof_WeaponTickPublication(uint32_t, uint64_t& index) noexcept
{
    index = 0;
    return false;
}
void Halo2Observer6Dof_RequestRecenter() noexcept {}
void Halo2Observer6Dof_ShutdownForVrFailure() noexcept {}

#endif
