#include "halo2_observer_6dof.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "../common/halo2_render_logic.h"
#include "../common/config.h"
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
    // E-H2-30: the tick BEFORE it, so the weapon's view can follow the
    // interpolator's blend between the two instead of snapping to the newest.
    std::atomic<uint64_t> g_weaponTickPreviousIndex{0};
    // E-H2-32: the tracked camera of the witnessed tick, seqlocked, and the
    // re-anchor hook that moves the drawn weapon from it to the frame camera.
    std::atomic<uint32_t> g_weaponTickBasisVersion{0};
    Halo2CameraBasis g_weaponTickBasis{};
    void* g_reanchorTarget = nullptr;
    std::atomic<uintptr_t> g_reanchorOriginal{0};
    std::atomic<uint32_t> g_reanchorActiveCallbacks{0};
    std::atomic<uint64_t> g_reanchorApplied{0};
    std::atomic<uint64_t> g_reanchorIdentity{0};
    std::atomic<uint64_t> g_reanchorSkipped{0};
    // C-H2-38: the C-H2-37 log reported "re-anchor applied 0, identity 0,
    // skipped 0" in both renderers - the detour's gate never opened and no
    // counter said which term closed it. Every entry is now classified and
    // the last call's arguments are kept for the report.
    std::atomic<uint64_t> g_reanchorEntered{0};
    std::atomic<uint64_t> g_reanchorUnhandled{0};
    std::atomic<uint64_t> g_reanchorOtherPlayer{0};
    std::atomic<uint64_t> g_reanchorNoNodes{0};
    std::atomic<uint64_t> g_reanchorNotLive{0};
    std::atomic<uint64_t> g_reanchorNoTick{0};
    std::atomic<int> g_reanchorLastPlayer{-1};
    std::atomic<int> g_reanchorLastId{-1};
    std::atomic<int> g_reanchorLastSlot{-1};
    std::atomic<uint32_t> g_reanchorLastHandled{0};
    std::atomic<uint32_t> g_reanchorLastCount{0};
    // E-H2-34 (C-H2-39): the pass cameras the owning core named (seqlocked),
    // the per-slot idempotency caches, and what the re-anchor found.
    std::atomic<uint32_t> g_passCamerasVersion{0};
    Halo2FirstPersonPassCameras g_passCameras{};
    std::atomic<bool> g_passCamerasSet{false};
    Halo2FirstPersonSlotCache g_slotCache[kHalo2FrameInterpolatorFirstPersonSlots]{};
    std::atomic<bool> g_slotCacheBusy{false};
    std::atomic<uint64_t> g_reanchorFromCache{0};
    std::atomic<uint64_t> g_reanchorCompensated{0};
    std::atomic<uint64_t> g_reanchorSpaceWorld{0};
    std::atomic<uint64_t> g_reanchorSpaceRelative{0};
    std::atomic<uint64_t> g_reanchorSpaceUnknown{0};
    std::atomic<uint64_t> g_reanchorPassCameras{0};
    std::atomic<uint64_t> g_reanchorFallbackFrame{0};
    std::atomic<uint64_t> g_reanchorBusy{0};
    std::atomic<uint64_t> g_reanchorBadSlot{0};
    std::atomic<uint64_t> g_publicationIndex{0};
    std::atomic<uint64_t> g_weaponsSlotResets{0};
    std::atomic<uint64_t> g_weaponsResetsBypassedForFloaty{0};
    std::atomic<uint64_t> g_floatyApplied{0};
    std::atomic<uint64_t> g_floatyFailed{0};
    // C-H2-47 (E-H2-41): the engine's own animation-graph readers, resolved by
    // unique signature. They are pure leaf reads - no allocation, no lock, no
    // logging - which is why they are safe to call from inside the palette
    // detour. Zero means the left-hand transaction stays dormant and the
    // single-carrier C-H2-46 placement runs instead.
    using Halo2GraphDefinitionGetFn = const void*(__fastcall*)(uint32_t);
    using Halo2GetSkeletonNodeFn = const void*(__fastcall*)(const void*, int);
    using Halo2FindNodeByFlagsFn = int(__fastcall*)(const void*, uint32_t);
    std::atomic<uintptr_t> g_graphDefinitionGet{0};
    std::atomic<uintptr_t> g_graphGetSkeletonNode{0};
    std::atomic<uintptr_t> g_graphFindNodeByFlags{0};
    std::atomic<uint64_t> g_leftHandApplied{0};
    std::atomic<uint64_t> g_leftHandRigidFallback{0};
    std::atomic<uint64_t> g_armBindingRebuilds{0};
    std::atomic<int> g_armBindingLastReason{-1};
    // Cached per animation graph. Rebuilt only when the graph id or the node
    // count changes, so the hot path is a compare, not a scan.
    Halo2FirstPersonArmBinding g_armBinding{};
    int g_armBindingGraphId = 0;
    bool g_armBindingGraphIdValid = false;
    std::atomic<uintptr_t> g_interpolatorResetAddress{0};
    // C-H2-43: H2EK-proven firing-only shot-direction result. Independent of
    // observer ownership and intentionally unable to move a camera or XInput.
    using Halo2WeaponAimHelperFn = void(__fastcall*)(
        uint32_t, float*, float*, uint64_t, float*, uint8_t, uint8_t, uint8_t);
    void* g_weaponAimTarget = nullptr;
    std::atomic<uintptr_t> g_weaponAimOriginal{0};
    std::atomic<uint32_t> g_weaponAimActiveCallbacks{0};
    std::atomic<uint64_t> g_weaponAimCalls{0};
    std::atomic<uint64_t> g_weaponAimApplied{0};
    std::atomic<uint64_t> g_weaponAimStock{0};
    std::atomic<uint64_t> g_weaponAimNonOwned{0};
    std::atomic<uint64_t> g_weaponAimNoOwnedUnit{0};
    std::atomic<uint64_t> g_weaponAimExceptions{0};
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
        g_weaponTickBasisVersion.fetch_add(1, std::memory_order_acq_rel);
        g_weaponTickBasis = published.tracked;
        g_weaponTickBasisVersion.fetch_add(1, std::memory_order_acq_rel);
        const uint64_t previous =
            g_weaponTickIndex.exchange(published.index, std::memory_order_acq_rel);
        if (previous && previous != published.index)
            g_weaponTickPreviousIndex.store(previous, std::memory_order_release);
        g_weaponsWitnessed.fetch_add(1, std::memory_order_relaxed);
    }

    // E-H2-32 (C-H2-37): the weapon must share the world's tick-quantised
    // state. The game's own yaw only changes at the 60 Hz tick, so the WORLD
    // steps at the tick during a stick turn - but MCC's interpolator smooths
    // the WEAPON between ticks, so the weapon trails the world it is attached
    // to. That is the drag. Correcting the weapon's view cannot fix it,
    // because the mismatch is against the world, not the head: the weapon has
    // to be at the tick the world is at. C-H2-34 removed this reset to cure a
    // 60 Hz feel that was really the CAMERA being tied to the tick (fixed
    // separately in C-H2-33, and the camera stays at the player's frame rate).
    void ResetFirstPersonInterpolation() noexcept
    {
        using ResetFn = void(__fastcall*)(int, int);
        const auto reset = reinterpret_cast<ResetFn>(
            g_interpolatorResetAddress.load(std::memory_order_acquire));
        if (!reset)
            return;
        for (int slot = 0; slot < kHalo2FrameInterpolatorFirstPersonSlots; ++slot)
            reset(static_cast<int>(kOwnedUser), slot);
        g_weaponsSlotResets.fetch_add(1, std::memory_order_relaxed);
    }

    // C-H2-47. Resolve which first-person nodes are the LEFT hand, from the
    // engine's own animation graph. E-H2-41 proves the retail layout; nothing
    // here is a guessed index. Any deviation from the exact shape every shipped
    // rig has returns false, and the caller then places the whole slot on the
    // right controller exactly as C-H2-46 does.
    //
    // Runs inside the palette detour's existing SEH + busy guard. Bounded: one
    // pass over at most 64 nodes, cached on the graph id and node count.
    bool ResolveArmBinding(
        int graphId, uint32_t count,
        Halo2FirstPersonArmBinding& out) noexcept
    {
        auto fail = [&](int reason) {
            g_armBindingLastReason.store(reason, std::memory_order_relaxed);
            g_armBinding = Halo2FirstPersonArmBinding{};
            g_armBindingGraphId = graphId;
            g_armBindingGraphIdValid = true;
            return false;
        };
        if (g_armBindingGraphIdValid && g_armBindingGraphId == graphId &&
            g_armBinding.count == count)
        {
            if (!g_armBinding.valid)
                return false;
            out = g_armBinding;
            return true;
        }
        g_armBindingRebuilds.fetch_add(1, std::memory_order_relaxed);
        g_armBindingGraphIdValid = false;

        const auto graphGet = reinterpret_cast<Halo2GraphDefinitionGetFn>(
            g_graphDefinitionGet.load(std::memory_order_acquire));
        const auto nodeGet = reinterpret_cast<Halo2GetSkeletonNodeFn>(
            g_graphGetSkeletonNode.load(std::memory_order_acquire));
        if (!graphGet || !nodeGet)
            return fail(1);
        if (!count || count > kHalo2FirstPersonPaletteCapacity)
            return fail(2);
        const void* const graph = graphGet(static_cast<uint32_t>(graphId));
        if (!graph)
            return fail(3);
        // The group tag is never checked by the engine's own resolver, so a
        // wrong id would silently hand back some other tag's bytes. This
        // equality is the gate that catches it: the palette the interpolator
        // just returned must have exactly as many nodes as the graph declares.
        const uint32_t declared = *reinterpret_cast<const volatile uint16_t*>(
            reinterpret_cast<uintptr_t>(graph) +
            kHalo2AnimationGraphNodeCountOffset);
        if (declared != count)
            return fail(4);

        uint8_t modelFlags[kHalo2FirstPersonPaletteCapacity]{};
        int16_t parents[kHalo2FirstPersonPaletteCapacity]{};
        for (uint32_t index = 0; index < count; ++index)
        {
            const void* const node = nodeGet(graph, static_cast<int>(index));
            if (!node)
                return fail(5);
            const uintptr_t address = reinterpret_cast<uintptr_t>(node);
            parents[index] = *reinterpret_cast<const volatile int16_t*>(
                address + kHalo2AnimationNodeParentOffset);
            modelFlags[index] = *reinterpret_cast<const volatile uint8_t*>(
                address + kHalo2AnimationNodeModelFlagsOffset);
        }
        Halo2FirstPersonArmBinding candidate{};
        if (!Halo2BuildFirstPersonArmBinding(
                modelFlags, parents, count, candidate))
        {
            return fail(6);
        }
        // Cross-check against the engine's OWN lookup where it is available:
        // it must name the same two wrists we derived from the same bits.
        const auto findByFlags = reinterpret_cast<Halo2FindNodeByFlagsFn>(
            g_graphFindNodeByFlags.load(std::memory_order_acquire));
        if (findByFlags)
        {
            if (findByFlags(graph, kHalo2ModelFlagRightHand) !=
                    candidate.rightWrist ||
                findByFlags(graph, kHalo2ModelFlagLeftHand) !=
                    candidate.leftWrist)
            {
                return fail(7);
            }
        }
        g_armBinding = candidate;
        g_armBindingGraphId = graphId;
        g_armBindingGraphIdValid = true;
        g_armBindingLastReason.store(0, std::memory_order_relaxed);
        out = candidate;
        return true;
    }

    // C-H2-46. THE one first-person carrier, for every consumer.
    //
    // The visible hands/gun mesh, the VR crosshair the compositor draws on
    // the hand ray, and the bullet direction must all be built from the SAME
    // controller sample with the SAME trim. C-H2-43 and C-H2-44 did not: the
    // mesh came from VR_GetAimPose with the gun_forward_m trim while the shot
    // came from the presented reticle pose with no trim, so the bullets did
    // not follow the gun the player was pointing. One builder now serves both.
    //
    // VR_GetAimPose is the shared, mount-calibrated aim pose (gun_yaw_deg /
    // gun_pitch_deg / gun_roll_deg and two-handed aim are already folded into
    // it), and it is the same pose the compositor draws the VR crosshair
    // from, so mesh, crosshair and bullet agree by construction.
    bool BuildFirstPersonCarrier(
        const Halo2CameraBasis& renderCamera,
        Halo2CameraBasis& carrier) noexcept
    {
        float headOrientation[4]{}, headPosition[3]{};
        float aimOrientation[4]{}, aimPosition[3]{};
        if (!VR_GetHeadPose(headOrientation, headPosition) ||
            !VR_GetAimPose(aimOrientation, aimPosition))
        {
            return false;
        }
        return Halo2BuildControllerCarrier(
            renderCamera, headOrientation, headPosition, aimOrientation,
            aimPosition, Game_GetWorldScale(),
            std::clamp(g_config.gun_forward_m, -0.3f, 0.5f), carrier);
    }

    // E-H2-32 (C-H2-37): the re-anchor. Normally the interpolation reset holds
    // the weapon exactly at the witnessed tick's placement; C-H2-41 keeps the
    // banks live only for floaty hands. This detour runs at the per-frame read
    // the renderer makes and moves the node geometry from
    // the tick camera to the CURRENT frame camera - so the weapon follows a
    // head turn at the frame rate and jumps with the world at every tick,
    // in both renderers, without touching any view matrix.
    using Halo2InterpolatorReadFn = uint8_t(__fastcall*)(
        int, int, int, uintptr_t*, uint32_t*);

    __declspec(noinline) uint8_t __fastcall Halo2InterpolatorReadDetour(
        int player, int id, int slot, uintptr_t* nodesOut, uint32_t* countOut)
    {
        g_reanchorActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
        const auto original = reinterpret_cast<Halo2InterpolatorReadFn>(
            g_reanchorOriginal.load(std::memory_order_acquire));
        if (!original)
        {
            g_reanchorActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
            return 0;
        }
        uint8_t handled = 0;
        __try { handled = original(player, id, slot, nodesOut, countOut); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_exceptions.fetch_add(1, std::memory_order_relaxed);
            handled = 0;
        }
        g_reanchorEntered.fetch_add(1, std::memory_order_relaxed);
        g_reanchorLastPlayer.store(player, std::memory_order_relaxed);
        g_reanchorLastId.store(id, std::memory_order_relaxed);
        g_reanchorLastSlot.store(slot, std::memory_order_relaxed);
        g_reanchorLastHandled.store(handled, std::memory_order_relaxed);
        const bool nodesPresent = nodesOut && countOut && *nodesOut;
        if (nodesPresent)
            g_reanchorLastCount.store(*countOut, std::memory_order_relaxed);
        const bool live = g_armed.load(std::memory_order_acquire) &&
            g_levelLive.load(std::memory_order_acquire) &&
            !g_teardownRequested.load(std::memory_order_acquire);
        // C-H2-38 measured the read's return value: 0 on every one of 6388
        // calls that handed back 42 valid nodes. It is not a success flag;
        // the gate is the node array itself.
        if (!nodesOut || !countOut || !*nodesOut)
            g_reanchorUnhandled.fetch_add(1, std::memory_order_relaxed);
        else if (player != static_cast<int>(kOwnedUser))
            g_reanchorOtherPlayer.fetch_add(1, std::memory_order_relaxed);
        else if (!nodesPresent)
            g_reanchorNoNodes.fetch_add(1, std::memory_order_relaxed);
        else if (!live)
            g_reanchorNotLive.fetch_add(1, std::memory_order_relaxed);
        else if (g_weaponTickIndex.load(std::memory_order_acquire) == 0)
            g_reanchorNoTick.fetch_add(1, std::memory_order_relaxed);
        else if (slot < 0 ||
                 slot >= kHalo2FrameInterpolatorFirstPersonSlots)
            g_reanchorBadSlot.fetch_add(1, std::memory_order_relaxed);
        else
        {
            Halo2CameraBasis tick{};
            bool tickValid = false;
            for (int attempt = 0; attempt < 4 && !tickValid; ++attempt)
            {
                const uint32_t before =
                    g_weaponTickBasisVersion.load(std::memory_order_acquire);
                if (before & 1u)
                    continue;
                Halo2CameraBasis candidate = g_weaponTickBasis;
                if (g_weaponTickBasisVersion.load(std::memory_order_acquire) ==
                    before)
                {
                    tick = candidate;
                    tickValid = true;
                }
            }
            // E-H2-34: the pass cameras the owning core named; the newest
            // publication only when no core has named a pass.
            Halo2FirstPersonPassCameras pass{};
            bool passValid = false;
            if (g_passCamerasSet.load(std::memory_order_acquire))
            {
                for (int attempt = 0; attempt < 4 && !passValid; ++attempt)
                {
                    const uint32_t before =
                        g_passCamerasVersion.load(std::memory_order_acquire);
                    if (before & 1u)
                        continue;
                    Halo2FirstPersonPassCameras candidate = g_passCameras;
                    if (g_passCamerasVersion.load(std::memory_order_acquire) ==
                        before)
                    {
                        pass = candidate;
                        passValid = candidate.frameValid &&
                            Halo2ValidateCameraBasis(candidate.frame);
                    }
                }
            }
            if (passValid)
            {
                g_reanchorPassCameras.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                Halo2ObserverPosePublication frame{};
                if (Halo2Observer6Dof_ReadPublishedPose(frame) &&
                    frame.generation ==
                        g_generation.load(std::memory_order_acquire) &&
                    Halo2ValidateCameraBasis(frame.tracked))
                {
                    pass = Halo2FirstPersonPassCameras{};
                    pass.frame = frame.tracked;
                    pass.frameValid = true;
                    passValid = true;
                    g_reanchorFallbackFrame.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (tickValid && passValid)
            {
                if (!pass.compensate &&
                    Halo2CameraBasesNearlyEqual(tick, pass.frame))
                {
                    g_reanchorIdentity.fetch_add(1, std::memory_order_relaxed);
                }
                bool expected = false;
                if (g_slotCacheBusy.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel))
                {
                    Halo2FirstPersonReanchorResult outcome{};
                    bool ok = false;
                    __try
                    {
                        // C-H2-46: only first-person slot 0 - the authored
                        // hands and the gun mesh they hold - is ever moved.
                        // No body, no other slot, no world geometry.
                        const bool floaty = slot == 0 &&
                            Game_Halo2ControllerAimActive();
                        if (floaty)
                        {
                            Halo2CameraBasis carrier{};
                            ok = BuildFirstPersonCarrier(pass.frame, carrier);
                            if (ok)
                            {
                                // C-H2-47: the left hand is its OWN
                                // transaction. If the binding, the left
                                // controller, or the split placement fails,
                                // the whole slot falls back to the single
                                // right-controller placement C-H2-46 shipped -
                                // the gun and right hand never depend on it.
                                Halo2FirstPersonArmBinding binding{};
                                Halo2CameraBasis leftCarrier{};
                                float leftOrientation[4]{}, leftPosition[3]{};
                                float headOrientation[4]{}, headPosition[3]{};
                                bool split = false;
                                if (ResolveArmBinding(id, *countOut, binding) &&
                                    VR_GetHeadPose(
                                        headOrientation, headPosition) &&
                                    VR_GetLeftControllerPose(
                                        leftOrientation, leftPosition) &&
                                    Halo2BuildControllerCarrier(
                                        pass.frame, headOrientation,
                                        headPosition, leftOrientation,
                                        leftPosition, Game_GetWorldScale(),
                                        std::clamp(
                                            g_config.left_hand_forward_m,
                                            -0.3f, 0.5f),
                                        leftCarrier))
                                {
                                    split =
                                        Halo2PlaceFirstPersonSlotOnTwoControllers(
                                            reinterpret_cast<float*>(*nodesOut),
                                            *countOut, pass.frame, carrier,
                                            leftCarrier, binding,
                                            std::clamp(
                                                g_config.gun_scale, 0.3f, 3.0f),
                                            std::clamp(
                                                g_config.left_hand_scale,
                                                0.3f, 3.0f),
                                            g_slotCache[slot], outcome);
                                }
                                if (split)
                                {
                                    ok = true;
                                    g_leftHandApplied.fetch_add(
                                        1, std::memory_order_relaxed);
                                }
                                else
                                {
                                    ok = Halo2PlaceFirstPersonSlotOnController(
                                        reinterpret_cast<float*>(*nodesOut),
                                        *countOut, pass.frame, carrier,
                                        std::clamp(
                                            g_config.gun_scale, 0.3f, 3.0f),
                                        g_slotCache[slot], outcome);
                                    g_leftHandRigidFallback.fetch_add(
                                        1, std::memory_order_relaxed);
                                }
                            }
                            (ok ? g_floatyApplied : g_floatyFailed)
                                .fetch_add(1, std::memory_order_relaxed);
                        }
                        else
                        {
                            ok = Halo2ReanchorFirstPersonSlot(
                                reinterpret_cast<float*>(*nodesOut), *countOut,
                                tick, pass, g_slotCache[slot], outcome);
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        g_exceptions.fetch_add(1, std::memory_order_relaxed);
                        ok = false;
                    }
                    g_slotCacheBusy.store(false, std::memory_order_release);
                    switch (outcome.space)
                    {
                    case Halo2FirstPersonNodeSpace::World:
                        g_reanchorSpaceWorld.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case Halo2FirstPersonNodeSpace::CameraRelative:
                        g_reanchorSpaceRelative.fetch_add(1, std::memory_order_relaxed);
                        break;
                    default:
                        g_reanchorSpaceUnknown.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    if (ok && outcome.applied)
                    {
                        g_reanchorApplied.fetch_add(1, std::memory_order_relaxed);
                        if (outcome.fromCache)
                            g_reanchorFromCache.fetch_add(1, std::memory_order_relaxed);
                        if (outcome.compensated)
                            g_reanchorCompensated.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        g_reanchorSkipped.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                else
                {
                    g_reanchorBusy.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                g_reanchorSkipped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_reanchorActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return handled;
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
        if (owned)
        {
            bool floatyReady = false;
            if (Game_Halo2ControllerAimActive() &&
                g_reanchorOriginal.load(std::memory_order_acquire))
            {
                float hq[4]{}, hp[3]{}, aq[4]{}, ap[3]{};
                floatyReady = VR_GetHeadPose(hq, hp) && VR_GetAimPose(aq, ap);
            }
            __try
            {
                if (floatyReady)
                {
                    // H2EK's read returns nodes only while the two frame banks
                    // remain valid. The old reset deliberately invalidates
                    // them; bypass it only while the independently installed
                    // floaty transaction has a live controller sample.
                    g_weaponsResetsBypassedForFloaty.fetch_add(
                        1, std::memory_order_relaxed);
                }
                else
                {
                    ResetFirstPersonInterpolation();
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_exceptions.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_weaponsActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    __declspec(noinline) void __fastcall Halo2WeaponAimHelperDetour(
        uint32_t objectIndex, float* origin, float* direction, uint64_t marker,
        float* offset, uint8_t projectOrigin, uint8_t useUnitAim,
        uint8_t collisionAdjust)
    {
        g_weaponAimActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
        g_weaponAimCalls.fetch_add(1, std::memory_order_relaxed);
        const auto original = reinterpret_cast<Halo2WeaponAimHelperFn>(
            g_weaponAimOriginal.load(std::memory_order_acquire));
        bool originalCompleted = false;
        if (original)
        {
            __try
            {
                original(objectIndex, origin, direction, marker, offset,
                         projectOrigin, useUnitAim, collisionAdjust);
                originalCompleted = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_weaponAimExceptions.fetch_add(1, std::memory_order_relaxed);
            }
        }

        bool applied = false;
        if (originalCompleted && useUnitAim && origin && direction &&
            Game_Halo2ControllerAimActive() &&
            Halo2Observer6Dof_DirectWeaponAimArmed())
        {
            __try
            {
                // E-H2-37: the helper is shared by every firing unit. Resolve
                // output user 0 through H2's own players_globals and datum
                // layout, then require the helper's unit handle to match it.
                // This is a read-only hot-path guard: no scan, lock, or call
                // into another engine subsystem occurs here.
                uint32_t ownedUnit = UINT32_MAX;
                const uintptr_t module =
                    reinterpret_cast<uintptr_t>(g_moduleReference);
                const uintptr_t playersGlobals = module
                    ? *reinterpret_cast<const volatile uintptr_t*>(
                          module + kHalo2PlayersGlobalsPointerRva)
                    : 0;
                const uintptr_t playersData = module
                    ? *reinterpret_cast<const volatile uintptr_t*>(
                          module + kHalo2PlayersDataArrayPointerRva)
                    : 0;
                if (playersGlobals && playersData)
                {
                    const uint32_t playerIndex =
                        *reinterpret_cast<const volatile uint32_t*>(
                            playersGlobals + kHalo2PlayerUserMappingOffset +
                            kOwnedUser * sizeof(uint32_t));
                    const uint32_t absolutePlayer = playerIndex & 0xffffu;
                    const uintptr_t storage =
                        *reinterpret_cast<const volatile uintptr_t*>(
                            playersData + kHalo2DataArrayStorageOffset);
                    if (playerIndex != UINT32_MAX &&
                        absolutePlayer < kHalo2MaximumPlayers && storage)
                    {
                        const uintptr_t player = playersData + storage +
                            static_cast<uintptr_t>(absolutePlayer) *
                                kHalo2PlayerDatumStride;
                        ownedUnit =
                            *reinterpret_cast<const volatile uint32_t*>(
                                player + kHalo2PlayerUnitIndexOffset);
                    }
                }
                if (ownedUnit == UINT32_MAX)
                {
                    g_weaponAimNoOwnedUnit.fetch_add(
                        1, std::memory_order_relaxed);
                    __leave;
                }
                if (objectIndex != ownedUnit)
                {
                    g_weaponAimNonOwned.fetch_add(
                        1, std::memory_order_relaxed);
                    __leave;
                }

                // C-H2-46: the SAME carrier the visible mesh is drawn on
                // (BuildFirstPersonCarrier), so the bullet leaves along the
                // gun the player is pointing and passes through the VR
                // crosshair drawn on that same ray.
                Halo2ObserverPosePublication publication{};
                Halo2CameraBasis carrier{};
                if (Halo2Observer6Dof_ReadPublishedPose(publication) &&
                    publication.generation ==
                        g_generation.load(std::memory_order_acquire) &&
                    BuildFirstPersonCarrier(publication.tracked, carrier))
                {
                    const float range = std::clamp(
                        g_config.crosshair_distance_m, 2.0f, 50.0f) *
                        Game_GetWorldScale();
                    float candidate[3]{};
                    if (Halo2BuildControllerShotDirection(
                            origin, carrier, range, candidate))
                    {
                        std::memcpy(direction, candidate, sizeof(candidate));
                        applied = true;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_weaponAimExceptions.fetch_add(1, std::memory_order_relaxed);
            }
        }
        (applied ? g_weaponAimApplied : g_weaponAimStock)
            .fetch_add(1, std::memory_order_relaxed);
        g_weaponAimActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
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
        if (g_weaponAimTarget)
        {
            const MH_STATUS disabled = MH_DisableHook(g_weaponAimTarget);
            if (disabled != MH_OK && disabled != MH_ERROR_NOT_CREATED &&
                disabled != MH_ERROR_DISABLED)
            {
                LOG("Halo 2 direct weapon aim: disable failed (%d); ownership "
                    "retained for cleanup", static_cast<int>(disabled));
                return false;
            }
            for (int attempt = 0; attempt < 200 &&
                 g_weaponAimActiveCallbacks.load(std::memory_order_acquire);
                 ++attempt)
            {
                Sleep(10);
            }
            if (g_weaponAimActiveCallbacks.load(std::memory_order_acquire))
            {
                LOG("Halo 2 direct weapon aim callbacks did not drain; "
                    "ownership retained for cleanup");
                return false;
            }
            (void)MH_RemoveHook(g_weaponAimTarget);
            g_weaponAimTarget = nullptr;
            g_weaponAimOriginal.store(0, std::memory_order_release);
        }
        if (g_reanchorTarget)
        {
            const MH_STATUS disabled = MH_DisableHook(g_reanchorTarget);
            if (disabled != MH_OK && disabled != MH_ERROR_NOT_CREATED &&
                disabled != MH_ERROR_DISABLED)
            {
                LOG("Halo 2 observer 6DOF: re-anchor disable failed (%d); "
                    "ownership retained for cleanup",
                    static_cast<int>(disabled));
                return false;
            }
            for (int attempt = 0; attempt < 200 &&
                 g_reanchorActiveCallbacks.load(std::memory_order_acquire);
                 ++attempt)
            {
                Sleep(10);
            }
            if (g_reanchorActiveCallbacks.load(std::memory_order_acquire))
            {
                LOG("Halo 2 observer 6DOF: re-anchor callbacks did not "
                    "drain; ownership retained for cleanup");
                return false;
            }
            (void)MH_RemoveHook(g_reanchorTarget);
            g_reanchorTarget = nullptr;
            g_reanchorOriginal.store(0, std::memory_order_release);
            g_passCamerasSet.store(false, std::memory_order_release);
            for (auto& cache : g_slotCache)
                cache.valid = false;
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
        g_weaponTickPreviousIndex.store(0, std::memory_order_release);
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
        // first-person slot reset, so the reset's entry bytes remain part of
        // the identity proof. C-H2-41 bypasses the call only while its optional
        // floaty-hand transaction is live; every stock/failed-feature path
        // retains the accepted C-H2-40 reset behavior.
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
                    const uintptr_t readSide =
                        base + kHalo2FrameInterpolatorReadRva;
                    uint8_t readBytes[sizeof(kHalo2FrameInterpolatorReadEntryBytes)]{};
                    bool readReadable = false;
                    __try
                    {
                        std::memcpy(readBytes,
                                    reinterpret_cast<const void*>(readSide),
                                    sizeof(readBytes));
                        readReadable = true;
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) { readReadable = false; }
                    if (!readReadable ||
                        std::memcmp(readBytes,
                                    kHalo2FrameInterpolatorReadEntryBytes,
                                    sizeof(readBytes)) != 0)
                    {
                        LOG("Halo 2 weapon re-anchor WITHHELD: the entry bytes "
                            "at +0x%X are not the interpolator read side; the "
                            "weapon stays at the tick camera and will lag a "
                            "head turn by up to one tick",
                            static_cast<unsigned>(kHalo2FrameInterpolatorReadRva));
                    }
                    else
                    {
                        void* const readTarget =
                            reinterpret_cast<void*>(readSide);
                        void* readTrampoline = nullptr;
                        const MH_STATUS readCreated = MH_CreateHook(
                            readTarget,
                            reinterpret_cast<void*>(&Halo2InterpolatorReadDetour),
                            &readTrampoline);
                        if (readCreated != MH_OK || !readTrampoline)
                        {
                            LOG("Halo 2 weapon re-anchor WITHHELD: hook "
                                "create=%d", static_cast<int>(readCreated));
                            if (readCreated == MH_OK)
                                (void)MH_RemoveHook(readTarget);
                        }
                        else
                        {
                            g_reanchorTarget = readTarget;
                            g_reanchorOriginal.store(
                                reinterpret_cast<uintptr_t>(readTrampoline),
                                std::memory_order_release);
                            if (MH_EnableHook(readTarget) != MH_OK)
                            {
                                LOG("Halo 2 weapon re-anchor WITHHELD: hook "
                                    "enable failed");
                                (void)MH_RemoveHook(readTarget);
                                g_reanchorTarget = nullptr;
                                g_reanchorOriginal.store(
                                    0, std::memory_order_release);
                            }
                            else
                            {
                                LOG("Halo 2 weapon re-anchor installed on the "
                                    "first-person interpolator read +0x%X "
                                    "(node stride 0x%X, axes +0x%X, position "
                                    "+0x%X): every frame the drawn weapon is "
                                    "moved rigidly from the witnessed tick's "
                                    "camera to the frame's, so it follows a "
                                    "head turn at the player's frame rate and "
                                    "jumps with the world at every tick, in "
                                    "both renderers",
                                    static_cast<unsigned>(
                                        kHalo2FrameInterpolatorReadRva),
                                    static_cast<unsigned>(
                                        kHalo2FirstPersonNodeStride),
                                    static_cast<unsigned>(
                                        kHalo2FirstPersonNodeAxesOffset),
                                    static_cast<unsigned>(
                                        kHalo2FirstPersonNodePositionOffset));
                                if (kHalo2ControllerOwnedAimEnabled)
                                {
                                    LOG("Halo 2 floating hands ARMED "
                                        "(C-H2-46): first-person slot 0 only "
                                        "- the authored hands and the gun "
                                        "mesh they hold - rides the right "
                                        "controller through one shared "
                                        "carrier (gun_forward_m %.3f m, "
                                        "gun_scale %.2f, mount trim "
                                        "%.1f/%.1f/%.1f deg). floating_hands "
                                        "in halomccvr.cfg is %d and is NOT "
                                        "consulted, as in Reach. No body, no "
                                        "other slot, no world geometry, no "
                                        "XInput, no camera field is touched",
                                        g_config.gun_forward_m,
                                        g_config.gun_scale,
                                        g_config.gun_yaw_deg,
                                        g_config.gun_pitch_deg,
                                        g_config.gun_roll_deg,
                                        g_config.floating_hands ? 1 : 0);
                                }
                            }
                        }
                    }
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

        // E-H2-41 (C-H2-47): resolve the engine's own animation-graph readers so
        // the LEFT hand can be identified from Halo 2's own data instead of a
        // hardcoded node index. These are read-only leaf calls; nothing here is
        // hooked or patched. All three must match exactly once and sit at their
        // pinned RVAs, or the left-hand transaction stays dormant and the whole
        // first-person slot rides the right controller as C-H2-46 shipped.
        if (kHalo2ControllerOwnedAimEnabled)
        {
            // The 8 leading padding bytes disambiguate the standalone function
            // from the two copies the optimizer inlined elsewhere.
            constexpr char kGraphDefinitionGetPattern[] =
                "CC CC CC CC CC CC CC CC 48 8B 05 ?? ?? ?? ?? 0F B7 C9 48 03 "
                "C9 48 63 44 C8 08 48 03 05 ?? ?? ?? ?? C3";
            constexpr uint32_t kGraphDefinitionGetSkew = 8;
            constexpr char kGetSkeletonNodePattern[] =
                "48 63 41 10 33 C9 83 F8 FF 74 26 85 C0 79 18 8B C8 48 63 C2 "
                "0F BA F1 1F 48 C1 E0 05 48 03 0D ?? ?? ?? ?? 48 03 C1 C3 48 "
                "8B C8 48 03 0D ?? ?? ?? ?? 48 63 C2 48 C1 E0 05 48 03 C1 C3";
            constexpr char kFindNodeByFlagsPattern[] =
                "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 48 63 41 0C 45 "
                "33 DB 85 C0 7E 53 4C 63 41 10 45 33 C9 48 8B 35 ?? ?? ?? ?? "
                "45 33 D2 48 8B 3D ?? ?? ?? ?? 48 8B D8 33 C0 41 83 F8 FF 74 "
                "15 45 85 C0 79 0C 41 8B C0 0F BA F0 1F 48 03 C6 EB 04 4A 8D "
                "04 07 41 0F B6 4C 02 0A 23 CA 3B CA 74 24";
            uintptr_t graphGetMatch = 0, nodeGetMatch = 0, findMatch = 0;
            uint32_t graphGetCount = 0, nodeGetCount = 0, findCount = 0;
            const bool graphGetOk = CountPatternMatches(
                    base, size, kGraphDefinitionGetPattern, graphGetMatch,
                    graphGetCount) &&
                graphGetCount == 1 &&
                graphGetMatch + kGraphDefinitionGetSkew ==
                    base + kHalo2AnimationGraphDefinitionGetRva;
            const bool nodeGetOk = CountPatternMatches(
                    base, size, kGetSkeletonNodePattern, nodeGetMatch,
                    nodeGetCount) &&
                nodeGetCount == 1 &&
                nodeGetMatch == base + kHalo2AnimationGraphGetSkeletonNodeRva;
            const bool findOk = CountPatternMatches(
                    base, size, kFindNodeByFlagsPattern, findMatch,
                    findCount) &&
                findCount == 1 &&
                findMatch == base + kHalo2AnimationGraphFindNodeByFlagsRva;
            if (graphGetOk && nodeGetOk)
            {
                g_graphDefinitionGet.store(
                    base + kHalo2AnimationGraphDefinitionGetRva,
                    std::memory_order_release);
                g_graphGetSkeletonNode.store(
                    base + kHalo2AnimationGraphGetSkeletonNodeRva,
                    std::memory_order_release);
                if (findOk)
                {
                    g_graphFindNodeByFlags.store(
                        base + kHalo2AnimationGraphFindNodeByFlagsRva,
                        std::memory_order_release);
                }
                LOG("Halo 2 left hand ARMED (C-H2-47): the left wrist and its "
                    "descendants ride the LEFT controller, identified from the "
                    "engine's own animation-graph model flags (node +0x%X, "
                    "left-hand bit 0x%02X, right-hand bit 0x%02X, stride "
                    "0x%X) - no hardcoded node index. Graph reader +0x%X, node "
                    "reader +0x%X, engine flag lookup %s. The gun stays on the "
                    "right hand; any failure drops this slot back to the "
                    "single right-controller placement",
                    static_cast<unsigned>(
                        kHalo2AnimationNodeModelFlagsOffset),
                    static_cast<unsigned>(kHalo2ModelFlagLeftHand),
                    static_cast<unsigned>(kHalo2ModelFlagRightHand),
                    static_cast<unsigned>(kHalo2AnimationNodeStride),
                    static_cast<unsigned>(
                        kHalo2AnimationGraphDefinitionGetRva),
                    static_cast<unsigned>(
                        kHalo2AnimationGraphGetSkeletonNodeRva),
                    findOk ? "cross-checking" : "NOT located (skipped)");
            }
            else
            {
                LOG("Halo 2 left hand WITHHELD: animation-graph reader "
                    "signatures matched %u / %u / %u times; the hands and gun "
                    "stay together on the right controller",
                    graphGetCount, nodeGetCount, findCount);
            }
        }

        // E-H2-37: this optional hook owns only the direction returned to the
        // stock firing function. Besides the helper, require the independent
        // H2EK-matched output-user iterator and player-update identities that
        // prove the local-player guard's two global pointers and datum layout.
        //
        // C-H2-45 stopped installing it while the experiment was reverted;
        // C-H2-46 re-arms it, because "the bullets follow the crosshair" is
        // exactly what this detour delivers. It is still the ONLY thing the
        // controller is allowed to steer besides the visible mesh: no XInput,
        // no observer field, no camera field.
        uintptr_t aimMatch = 0;
        uint32_t aimMatchCount = 0;
        uintptr_t userIteratorMatch = 0;
        uint32_t userIteratorMatchCount = 0;
        uintptr_t playerUpdateMatch = 0;
        uint32_t playerUpdateMatchCount = 0;
        constexpr char kWeaponAimPattern[] =
            "48 8B C4 48 89 58 20 55 56 41 55 48 8D 68 C1 "
            "48 81 EC C0 00 00 00";
        constexpr char kPlayerUserIteratorPattern[] =
            "45 33 C9 8D 41 01 83 F9 FF 44 0F 45 C8 49 63 C9 "
            "48 83 F9 04 7D 25 4C 8B 05 B3 EC 7D 00 49 83 C0 0C";
        constexpr char kPlayerUpdatePattern[] =
            "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 "
            "41 54 41 55 41 56 41 57 48 8D AC 24 30 FF FF FF "
            "48 81 EC D0 01 00 00 4C 8B E9 E8 D1 2A 00 00 45 "
            "33 FF 84 C0 0F 85 D9 00 00 00 48 8B 15 D7 D0 7D 00";
        if (!kHalo2ControllerOwnedAimEnabled)
        {
            LOG("Halo 2 direct weapon aim NOT installed: the controller-owned "
                "first-person feature is disarmed at its build switch. The "
                "right stick turns the character and camera, the headset owns "
                "pitch, and hand motion reaches neither the camera nor the "
                "weapon");
        }
        else if (!CountPatternMatches(
                base, size, kWeaponAimPattern, aimMatch, aimMatchCount) ||
            aimMatchCount != 1 ||
            aimMatch != base + kHalo2WeaponAimHelperRva ||
            !CountPatternMatches(
                base, size, kPlayerUserIteratorPattern, userIteratorMatch,
                userIteratorMatchCount) ||
            userIteratorMatchCount != 1 ||
            userIteratorMatch != base + kHalo2PlayerUserIteratorRva ||
            !CountPatternMatches(
                base, size, kPlayerUpdatePattern, playerUpdateMatch,
                playerUpdateMatchCount) ||
            playerUpdateMatchCount != 1 ||
            playerUpdateMatch != base + kHalo2PlayerUpdateRva)
        {
            LOG("Halo 2 direct weapon aim WITHHELD: firing helper / user "
                "iterator / player update identities were %u / %u / %u "
                "matches; right stick and camera remain stock",
                aimMatchCount,
                userIteratorMatchCount, playerUpdateMatchCount);
        }
        else
        {
            void* trampoline = nullptr;
            void* const aimTarget = reinterpret_cast<void*>(aimMatch);
            const MH_STATUS created = MH_CreateHook(
                aimTarget, reinterpret_cast<void*>(&Halo2WeaponAimHelperDetour),
                &trampoline);
            if (created != MH_OK || !trampoline)
            {
                LOG("Halo 2 direct weapon aim WITHHELD: hook create=%d; "
                    "right stick and camera remain stock",
                    static_cast<int>(created));
                if (created == MH_OK)
                    (void)MH_RemoveHook(aimTarget);
            }
            else
            {
                g_weaponAimTarget = aimTarget;
                g_weaponAimOriginal.store(
                    reinterpret_cast<uintptr_t>(trampoline),
                    std::memory_order_release);
                const MH_STATUS enabled = MH_EnableHook(aimTarget);
                if (enabled != MH_OK)
                {
                    LOG("Halo 2 direct weapon aim WITHHELD: hook enable=%d; "
                        "right stick and camera remain stock",
                        static_cast<int>(enabled));
                    (void)MH_RemoveHook(aimTarget);
                    g_weaponAimTarget = nullptr;
                    g_weaponAimOriginal.store(0, std::memory_order_release);
                }
                else
                {
                    LOG("Halo 2 direct weapon aim installed at firing helper "
                        "+0x%X with output-user-0 unit guard: controller "
                        "replaces only the local player's shot direction; "
                        "AI, XInput, observer and camera are untouched",
                        static_cast<unsigned>(kHalo2WeaponAimHelperRva));
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
            "with a record the engine had moved, %llu with no publication, "
            "%llu interpolation resets (the weapon shares the world's tick); "
            "re-anchor applied %llu, identity %llu, skipped %llu (read detour "
            "entered %llu: unhandled %llu, other player %llu, no nodes %llu, "
            "not live %llu, no witnessed tick %llu, bad slot %llu; last call "
            "player %d id %d slot %d handled %u count %u); geometry: from "
            "cache %llu, classic stale-camera compensated %llu, node space "
            "world %llu / camera-relative %llu / unknown %llu, pass cameras "
            "named %llu, newest-publication fallback %llu, busy %llu; last "
            "witnessed publication #%llu; controller placement: %llu applied, "
            "%llu failed, %llu interpolation resets bypassed; left hand: "
            "%llu on its own controller, %llu rigid fallback, %llu binding "
            "rebuilds, last binding reason %d; direct shot "
            "aim: %llu calls, %llu applied, %llu stock, %llu non-owned, "
            "%llu no-owned-unit, %llu exceptions",
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
                g_weaponsSlotResets.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorIdentity.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorSkipped.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorEntered.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorUnhandled.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorOtherPlayer.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorNoNodes.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorNotLive.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorNoTick.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorBadSlot.load(std::memory_order_relaxed)),
            g_reanchorLastPlayer.load(std::memory_order_relaxed),
            g_reanchorLastId.load(std::memory_order_relaxed),
            g_reanchorLastSlot.load(std::memory_order_relaxed),
            g_reanchorLastHandled.load(std::memory_order_relaxed),
            g_reanchorLastCount.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(
                g_reanchorFromCache.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorCompensated.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorSpaceWorld.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorSpaceRelative.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorSpaceUnknown.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorPassCameras.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorFallbackFrame.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_reanchorBusy.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponTickIndex.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_floatyApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_floatyFailed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponsResetsBypassedForFloaty.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_leftHandApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_leftHandRigidFallback.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_armBindingRebuilds.load(std::memory_order_relaxed)),
            g_armBindingLastReason.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(
                g_weaponAimCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponAimApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponAimStock.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponAimNonOwned.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponAimNoOwnedUnit.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_weaponAimExceptions.load(std::memory_order_relaxed)));
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

bool Halo2Observer6Dof_DirectWeaponAimArmed() noexcept
{
    return g_weaponAimOriginal.load(std::memory_order_acquire) != 0 &&
        g_armed.load(std::memory_order_acquire) &&
        g_levelLive.load(std::memory_order_acquire) &&
        !g_teardownRequested.load(std::memory_order_acquire);
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
    uint32_t generation, uint64_t& index, uint64_t& previousIndex) noexcept
{
    index = g_weaponTickIndex.load(std::memory_order_acquire);
    previousIndex = g_weaponTickPreviousIndex.load(std::memory_order_acquire);
    return index != 0 && generation != 0 &&
        g_weaponTickGeneration.load(std::memory_order_relaxed) == generation;
}

void Halo2Observer6Dof_SetFirstPersonPassCameras(
    const Halo2FirstPersonPassCameras* cameras) noexcept
{
    if (!cameras)
    {
        g_passCamerasSet.store(false, std::memory_order_release);
        return;
    }
    g_passCamerasVersion.fetch_add(1, std::memory_order_acq_rel);
    g_passCameras = *cameras;
    g_passCamerasVersion.fetch_add(1, std::memory_order_acq_rel);
    g_passCamerasSet.store(true, std::memory_order_release);
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
bool Halo2Observer6Dof_DirectWeaponAimArmed() noexcept { return false; }
bool Halo2Observer6Dof_ReadPublishedPose(
    Halo2ObserverPosePublication&) noexcept { return false; }
int Halo2Observer6Dof_ReadPublishedPoses(
    Halo2ObserverPosePublication*, int) noexcept { return 0; }
bool Halo2Observer6Dof_WeaponTickPublication(
    uint32_t, uint64_t& index, uint64_t& previousIndex) noexcept
{
    index = 0;
    previousIndex = 0;
    return false;
}
void Halo2Observer6Dof_SetFirstPersonPassCameras(
    const Halo2FirstPersonPassCameras*) noexcept {}
void Halo2Observer6Dof_RequestRecenter() noexcept {}
void Halo2Observer6Dof_ShutdownForVrFailure() noexcept {}

#endif
