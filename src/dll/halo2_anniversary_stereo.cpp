#include "halo2_anniversary_stereo.h"

#include <windows.h>
#include <d3d11.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/halo2_render_logic.h"
#include "../common/log.h"
#include "game.h"
#include "halo2_observer_6dof.h"
#include "halo2_saber_camera.h"
#include "vr.h"

#ifndef HALOMCCVR_HALO2_ANNIVERSARY_STEREO
#define HALOMCCVR_HALO2_ANNIVERSARY_STEREO 0
#endif

#if HALOMCCVR_HALO2_ANNIVERSARY_STEREO

namespace
{
    // Recovered from all three engine call sites, which set only rcx and r8d:
    //   lea rcx,[r15+0x10] / xor edx,edx / mov r8d,0xC8 / call memset
    //   mov r8d,ebx        / mov rcx,r15 / call 0x2DF190
    // so rdx carries nothing and the view index is the third integer register.
    using SaberSceneFn = void(__fastcall*)(void*, void*, uint32_t);
    // 0x1C7740 calls this with both pointer arguments null:
    //   xor r8d,r8d / xor edx,edx / lea rcx,[rsp+0x30] / call 0x1C6D80
    using RebuildViewMatricesFn = void(__fastcall*)(void*, void*, void*);
    using CameraCommitFn = void(__fastcall*)(void*);

    enum class CoreState : uint8_t { StockFallback = 0, CleanupRequired, Installed };

    struct HeadReference
    {
        float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
        float position[3]{};
    };

    std::atomic<bool> g_installed{false};
    std::atomic<bool> g_armed{false};
    std::atomic<bool> g_levelLive{false};
    std::atomic<bool> g_remasteredLive{false};
    std::atomic<bool> g_teardown{false};
    std::atomic<bool> g_recenterRequested{true};
    std::atomic<bool> g_referenceValid{false};
    std::atomic<uint32_t> g_generation{0};
    std::atomic<uint32_t> g_vrFailureGeneration{0};
    std::atomic<uintptr_t> g_moduleBase{0};
    std::atomic<uintptr_t> g_observerResult{0};
    std::atomic<uintptr_t> g_originalScene{0};
    std::atomic<uintptr_t> g_rebuildMatrices{0};
    std::atomic<uintptr_t> g_cameraCommit{0};
    std::atomic<uint32_t> g_activeCallbacks{0};
    std::atomic<uint64_t> g_lastCompletedSerial{0};

    std::atomic<uint64_t> g_callbacks{0};
    std::atomic<uint64_t> g_pairs{0};
    std::atomic<uint64_t> g_drops{0};
    std::atomic<uint64_t> g_stockPasses{0};
    std::atomic<uint64_t> g_exceptions{0};
    // Every way EyeLoopBody can decline to render a pair, counted by
    // reason. 790 of 791 callbacks took one of these on 2026-08-21 and the
    // log could not say which; that is never allowed to happen again.
    enum class Bail : uint8_t
    {
        NoBinding = 0,
        SnapshotNotReady,
        HeadPoseInvalid,
        GenerationMismatch,
        SerialZero,
        SerialRepeated,
        CadenceUnsupported,
        ViewRecordUnresolved,
        ObserverBasisInvalid,
        ConstantsUnreadable,
        TrackedCameraFailed,
        EyeCameraFailed,
        CameraSaveFailed,
        StockFovInvalid,
        FinalRtvNull,
        PairBeginRefused,
        SecondPassPreambleFaulted,
        EyeApplyFailed,
        EyeBeginRefused,
        SceneRenderFaulted,
        EyeCompleteRefused,
        CameraRestoreFailed,
        PairCompleteRefused,
        Count
    };
    constexpr const char* kBailNames[] = {
        "noBinding", "snapshotNotReady", "headPoseInvalid",
        "generationMismatch", "serialZero", "serialRepeated",
        "cadenceUnsupported", "viewRecordUnresolved",
        "observerBasisInvalid", "constantsUnreadable",
        "trackedCameraFailed", "eyeCameraFailed", "cameraSaveFailed",
        "stockFovInvalid", "finalRtvNull", "pairBeginRefused", "secondPassPreambleFaulted",
        "eyeApplyFailed", "eyeBeginRefused", "sceneRenderFaulted",
        "eyeCompleteRefused", "cameraRestoreFailed", "pairCompleteRefused"};
    static_assert(sizeof(kBailNames) / sizeof(kBailNames[0]) ==
        static_cast<size_t>(Bail::Count));
    std::atomic<uint64_t> g_bails[static_cast<size_t>(Bail::Count)]{};
    // E-H2-6 pose ownership outcomes and view-selection evidence.
    std::atomic<uint64_t> g_posePublished{0};
    std::atomic<uint64_t> g_poseRederived{0};
    std::atomic<uint64_t> g_poseSelfTracked{0};
    std::atomic<uint32_t> g_viewIndexMask{0};
    std::atomic<uint32_t> g_lastViewIndex{0};
    std::atomic<uint32_t> g_censusGeneration{0};
    void CountBail(Bail reason) noexcept
    {
        g_bails[static_cast<size_t>(reason)].fetch_add(
            1, std::memory_order_relaxed);
    }
    uint64_t g_lastReportMs = 0;
    uint64_t g_lastPairsReported = UINT64_MAX;
    uint64_t g_lastCallbacksReported = UINT64_MAX;

    HeadReference g_reference{};
    HMODULE g_moduleReference = nullptr;
    void* g_sceneTarget = nullptr;
    CoreState g_coreState = CoreState::StockFallback;
    uint32_t g_rejectedGeneration = 0;
    uint32_t g_armedLoggedGeneration = 0;

    // Re-entry guard. The engine itself calls the scene render once per player
    // window; our own second pass must never be mistaken for one of those.
    thread_local bool g_insideEyeLoop = false;

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

    bool CopyGuarded(void* destination, const void* source, size_t bytes) noexcept
    {
        __try
        {
            std::memcpy(destination, source, bytes);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool WriteFloatsGuarded(uintptr_t address, const float* v, size_t n) noexcept
    {
        for (size_t i = 0; i < n; ++i)
            if (!std::isfinite(v[i]))
                return false;
        __try
        {
            for (size_t i = 0; i < n; ++i)
                *reinterpret_cast<volatile float*>(address + i * sizeof(float)) = v[i];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ValidHeadPose(const float q[4], const float p[3]) noexcept
    {
        float lengthSquared = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            if (!std::isfinite(q[i]))
                return false;
            lengthSquared += q[i] * q[i];
        }
        if (!std::isfinite(lengthSquared) || std::fabs(lengthSquared - 1.0f) > 0.05f)
            return false;
        for (int a = 0; a < 3; ++a)
            if (!std::isfinite(p[a]) ||
                std::fabs(p[a]) > kHalo2MaxHeadPositionMeters)
                return false;
        return true;
    }

    // record = *(base + 0x1A250F8) + 0x150 + viewIndex * 0x758, byte-verified
    // against the scene render's own arithmetic at 0x2DF2C5.
    bool ResolveViewRecord(
        uintptr_t base, uint32_t viewIndex, uintptr_t& record) noexcept
    {
        uintptr_t collection = 0;
        if (!ReadGuarded(base + kHalo2SaberViewCollectionSlotRva, collection) ||
            !collection)
        {
            return false;
        }
        int32_t count = 0;
        if (!ReadGuarded(collection + kHalo2SaberViewCountOffset, count) ||
            count <= 0 ||
            static_cast<uint32_t>(count) > kHalo2SaberViewRecordCapacity ||
            viewIndex >= static_cast<uint32_t>(count))
        {
            return false;
        }
        record = collection + kHalo2SaberViewRecordArrayOffset +
            static_cast<uintptr_t>(viewIndex) * kHalo2SaberViewRecordStride;
        return true;
    }

    bool ReadObserverBasis(uintptr_t result, Halo2CameraBasis& basis) noexcept
    {
        __try
        {
            const auto* p = reinterpret_cast<const volatile float*>(
                result + kHalo2ObserverResultPositionOffset);
            const auto* f = reinterpret_cast<const volatile float*>(
                result + kHalo2ObserverResultForwardOffset);
            const auto* u = reinterpret_cast<const volatile float*>(
                result + kHalo2ObserverResultUpOffset);
            for (int a = 0; a < 3; ++a)
            {
                basis.position[a] = p[a];
                basis.forward[a] = f[a];
                basis.up[a] = u[a];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return Halo2ValidateCameraBasis(basis);
    }

    // One eye: write the record's embedded camera world matrix, let the engine
    // normalise it and derive its inverse, then let the engine rebuild every
    // matrix the record carries. No matrix is invented here.
    bool ApplyEyeCamera(
        uintptr_t record, const Halo2CameraBasis& eye,
        const Halo2SaberCameraConstants& constants) noexcept
    {
        Halo2SaberViewMatrix matrix{};
        if (!Halo2BuildSaberViewMatrix(eye, constants, matrix))
            return false;
        const uintptr_t camera = record + kHalo2SaberViewRecordCameraOffset;
        if (!WriteFloatsGuarded(camera, matrix.m, 16))
            return false;
        const auto commit = reinterpret_cast<CameraCommitFn>(
            g_cameraCommit.load(std::memory_order_acquire));
        const auto rebuild = reinterpret_cast<RebuildViewMatricesFn>(
            g_rebuildMatrices.load(std::memory_order_acquire));
        if (!commit || !rebuild)
            return false;
        __try
        {
            commit(reinterpret_cast<void*>(camera));
            rebuild(reinterpret_cast<void*>(record), nullptr, nullptr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool RestoreCamera(
        uintptr_t record, const uint8_t* savedCamera) noexcept
    {
        const uintptr_t camera = record + kHalo2SaberViewRecordCameraOffset;
        if (!CopyGuarded(
                reinterpret_cast<void*>(camera), savedCamera,
                kHalo2SaberViewRecordCameraBytes))
        {
            return false;
        }
        const auto rebuild = reinterpret_cast<RebuildViewMatricesFn>(
            g_rebuildMatrices.load(std::memory_order_acquire));
        if (!rebuild)
            return false;
        __try
        {
            rebuild(reinterpret_cast<void*>(record), nullptr, nullptr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void CallStock(
        SaberSceneFn original, void* ctx, void* rdx, uint32_t viewIndex) noexcept
    {
        g_stockPasses.fetch_add(1, std::memory_order_relaxed);
        __try { original(ctx, rdx, viewIndex); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_exceptions.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void EyeLoopBody(
        SaberSceneFn original, void* ctx, void* rdx, uint32_t viewIndex) noexcept
    {
        const uintptr_t base = g_moduleBase.load(std::memory_order_acquire);
        const uint32_t generation = g_generation.load(std::memory_order_acquire);
        const uintptr_t observer = g_observerResult.load(std::memory_order_acquire);
        if (!base || !generation || !observer)
        {
            CountBail(Bail::NoBinding);
            CallStock(original, ctx, rdx, viewIndex);
            return;
        }

        Halo2SynchronousVrRenderSnapshot snapshot{};
        bool ready = false;
        __try { ready = VR_Halo2GetSynchronousRenderSnapshot(snapshot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ready = false; }

        uintptr_t record = 0;
        Halo2CameraBasis stock{};
        Halo2SaberCameraConstants constants{};
        const uint64_t serial = snapshot.preparedSerial;
        // Sequential so the FIRST failing condition is the one counted.
        Bail bail = Bail::Count;
        if (!ready)
            bail = Bail::SnapshotNotReady;
        else if (!snapshot.headPoseValid ||
                 !ValidHeadPose(snapshot.headOrientation, snapshot.headPosition))
            bail = Bail::HeadPoseInvalid;
        else if (snapshot.generation != generation)
            bail = Bail::GenerationMismatch;
        else if (serial == 0)
            bail = Bail::SerialZero;
        else if (serial == g_lastCompletedSerial.load(std::memory_order_acquire))
            bail = Bail::SerialRepeated;
        else if (!Halo2PreparedCadenceSupported(
                     snapshot.predictedDisplayPeriodNs,
                     snapshot.predictedDisplayDeltaNs))
            bail = Bail::CadenceUnsupported;
        else if (!ResolveViewRecord(base, viewIndex, record))
            bail = Bail::ViewRecordUnresolved;
        else if (!ReadObserverBasis(observer, stock))
            bail = Bail::ObserverBasisInvalid;
        else if (!Halo2SaberCamera_ReadConstants(base, constants))
            bail = Bail::ConstantsUnreadable;
        if (bail != Bail::Count)
        {
            CountBail(bail);
            CallStock(original, ctx, rdx, viewIndex);
            return;
        }

        if (g_recenterRequested.exchange(false, std::memory_order_acq_rel) ||
            !g_referenceValid.load(std::memory_order_acquire))
        {
            std::memcpy(g_reference.orientation, snapshot.headOrientation,
                        sizeof(g_reference.orientation));
            std::memcpy(g_reference.position, snapshot.headPosition,
                        sizeof(g_reference.position));
            g_referenceValid.store(true, std::memory_order_release);
        }

        Halo2TrackedHeadInput head{};
        std::memcpy(head.orientation, snapshot.headOrientation, sizeof(head.orientation));
        std::memcpy(head.position, snapshot.headPosition, sizeof(head.position));
        std::memcpy(head.referenceOrientation, g_reference.orientation,
                    sizeof(head.referenceOrientation));
        std::memcpy(head.referencePosition, g_reference.position,
                    sizeof(head.referencePosition));

        head.positional = Game_IsPositionalTracking();
        head.worldScale = Game_GetWorldScale();

        // E-H2-6: `stock` was read from the observer result record. When the
        // observer core wrote this frame's head pose into it, tracking it
        // again doubles the pose exactly as it did in the classic core. The
        // Saber scene renders on its own thread, so its serial frequently
        // differs from the observer's: rebuild from the published stock
        // with THIS snapshot and the observer's own reference in that case.
        Halo2ObserverPosePublication publication{};
        bool published = false;
        __try { published = Halo2Observer6Dof_ReadPublishedPose(publication); }
        __except (EXCEPTION_EXECUTE_HANDLER) { published = false; }
        Halo2CameraBasis tracked{};
        Halo2CameraBasis eyes[kHalo2EyeCount]{};
        bool centerOk = false;
        switch (Halo2SelectPoseOwner(
            published, publication.generation, publication.serial,
            publication.tracked, stock, generation, serial))
        {
        case Halo2PoseOwnerDecision::UsePublishedTracked:
            g_posePublished.fetch_add(1, std::memory_order_relaxed);
            tracked = stock;
            centerOk = true;
            break;
        case Halo2PoseOwnerDecision::RederiveFromPublishedStock:
            g_poseRederived.fetch_add(1, std::memory_order_relaxed);
            std::memcpy(head.referenceOrientation,
                        publication.referenceOrientation,
                        sizeof(head.referenceOrientation));
            std::memcpy(head.referencePosition, publication.referencePosition,
                        sizeof(head.referencePosition));
            centerOk = Halo2BuildTrackedCenterCamera(
                publication.stock, head, tracked);
            break;
        case Halo2PoseOwnerDecision::SelfTrack:
            g_poseSelfTracked.fetch_add(1, std::memory_order_relaxed);
            centerOk = Halo2BuildTrackedCenterCamera(stock, head, tracked);
            break;
        case Halo2PoseOwnerDecision::NoPose:
        default:
            centerOk = false;
            break;
        }
        if (!centerOk)
        {
            CountBail(Bail::TrackedCameraFailed);
            CallStock(original, ctx, rdx, viewIndex);
            return;
        }
        for (int eye = 0; eye < kHalo2EyeCount; ++eye)
        {
            if (!Halo2BuildSynchronousEyeCamera(
                    tracked, snapshot.eyes[eye].position,
                    snapshot.eyes[eye].orientation, eyes[eye]))
            {
                CountBail(Bail::EyeCameraFailed);
                CallStock(original, ctx, rdx, viewIndex);
                return;
            }
        }

        // Everything the two passes disturb is saved before the first of them:
        // the record's embedded camera, and the 0xC8 context bytes the
        // engine's caller prepared for THIS call (one call site also stores a
        // resource pointer at ctx+0x98 after its memset, which the scene pass
        // consumes; zeroing it for the second eye would not be what the
        // engine handed the first).
        static thread_local uint8_t savedCamera[kHalo2SaberViewRecordCameraBytes];
        static thread_local uint8_t savedContext[kHalo2SaberSceneContextResetBytes];
        if (!CopyGuarded(
                savedCamera,
                reinterpret_cast<const void*>(
                    record + kHalo2SaberViewRecordCameraOffset),
                kHalo2SaberViewRecordCameraBytes) ||
            !CopyGuarded(
                savedContext,
                reinterpret_cast<const uint8_t*>(ctx) +
                    kHalo2SaberSceneContextResetOffset,
                kHalo2SaberSceneContextResetBytes))
        {
            CountBail(Bail::CameraSaveFailed);
            CallStock(original, ctx, rdx, viewIndex);
            return;
        }
        // The field of view this pass renders with is the engine's own: the
        // per-eye write replaces only the camera matrix. Read the stock
        // horizontal/vertical field of view in degrees from the saved copy
        // (0xBC560 keeps both consistent with the aspect) and hand the
        // half-angles to the compositor as this eye's symmetric cover.
        // VR_Halo2GetSynchronousHalfFovs cannot be used here: the pair token
        // holds the RESERVED serial until the pair completes, so it always
        // answered false and no Anniversary eye could ever complete.
        float stockVerticalDegrees = 0.0f;
        float stockHorizontalDegrees = 0.0f;
        std::memcpy(&stockVerticalDegrees,
                    savedCamera + kHalo2SaberCameraVerticalFovDegreesOffset,
                    sizeof(stockVerticalDegrees));
        std::memcpy(&stockHorizontalDegrees,
                    savedCamera + kHalo2SaberCameraHorizontalFovDegreesOffset,
                    sizeof(stockHorizontalDegrees));
        constexpr float kDegreesToRadians = 3.14159265f / 180.0f;
        const float halfFovX = stockHorizontalDegrees * 0.5f * kDegreesToRadians;
        const float halfFovY = stockVerticalDegrees * 0.5f * kDegreesToRadians;
        if (!std::isfinite(halfFovX) || !std::isfinite(halfFovY) ||
            halfFovX <= 0.01f || halfFovX >= 1.55f ||
            halfFovY <= 0.01f || halfFovY >= 1.55f)
        {
            CountBail(Bail::StockFovInvalid);
            CallStock(original, ctx, rdx, viewIndex);
            return;
        }
        int32_t savedLatch = 0;
        const bool haveLatch =
            ReadGuarded(base + kHalo2SaberSceneOnceLatchRva, savedLatch);

        uintptr_t finalRtv = 0;
        if (!ReadGuarded(base + kHalo2RetailFinalOutputRtvSlotRva, finalRtv) ||
            !finalRtv)
        {
            CountBail(Bail::FinalRtvNull);
            CallStock(original, ctx, rdx, viewIndex);
            return;
        }
        if (!VR_Halo2BeginSynchronousPair(
                generation, serial,
                reinterpret_cast<ID3D11RenderTargetView*>(finalRtv)))
        {
            CountBail(Bail::PairBeginRefused);
            CallStock(original, ctx, rdx, viewIndex);
            return;
        }

        bool ok = true;
        for (int eye = 0; eye < kHalo2EyeCount && ok; ++eye)
        {
            if (eye != 0)
            {
                // Reproduce the caller preamble the engine performs before every
                // scene render, and re-arm the once-per-frame latch the first
                // pass consumed, or the second eye silently skips work.
                __try
                {
                    std::memcpy(
                        reinterpret_cast<uint8_t*>(ctx) +
                            kHalo2SaberSceneContextResetOffset,
                        savedContext, kHalo2SaberSceneContextResetBytes);
                    if (haveLatch)
                    {
                        *reinterpret_cast<volatile int32_t*>(
                            base + kHalo2SaberSceneOnceLatchRva) = savedLatch;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    CountBail(Bail::SecondPassPreambleFaulted);
                    ok = false;
                    break;
                }
            }
            if (!ApplyEyeCamera(record, eyes[eye], constants))
            {
                CountBail(Bail::EyeApplyFailed);
                ok = false;
                break;
            }
            if (!VR_Halo2BeginSynchronousEye(generation, serial, eye))
            {
                CountBail(Bail::EyeBeginRefused);
                ok = false;
                break;
            }
            bool rendered = false;
            __try
            {
                original(ctx, rdx, viewIndex);
                rendered = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_exceptions.fetch_add(1, std::memory_order_relaxed);
            }
            // The raster-eye scope is closed whether or not the engine
            // returned normally; that is where the finished eye is copied.
            VR_EndRasterEye();
            // Once per generation, name what the engine actually holds at
            // this moment: the three classic output slots and the target
            // bound right now. Whether 0x197EE58 is the Saber frame when
            // the scene render returns is NOT proven either way; this is
            // the evidence, next to the pixel check.
            if (eye == 0 && rendered &&
                g_censusGeneration.load(std::memory_order_acquire) != generation)
            {
                g_censusGeneration.store(generation, std::memory_order_release);
                uintptr_t slots[3]{};
                (void)ReadGuarded(base + kHalo2RetailFinalOutputRtvSlotRva, slots[0]);
                (void)ReadGuarded(base + kHalo2RetailFinalOutputRtvSlotRva + 8, slots[1]);
                (void)ReadGuarded(base + kHalo2RetailFinalOutputRtvSlotRva + 0x30, slots[2]);
                VR_Halo2LogTargetCensusOnce(
                    "Anniversary eye 0 end", slots, viewIndex,
                    g_viewIndexMask.load(std::memory_order_relaxed));
            }
            if (!rendered)
            {
                CountBail(Bail::SceneRenderFaulted);
                ok = false;
                break;
            }
            if (!VR_Halo2CompleteSynchronousEye(
                    generation, serial, eye, halfFovX, halfFovY))
            {
                CountBail(Bail::EyeCompleteRefused);
                ok = false;
                break;
            }
        }

        // The engine's own state is put back before anything else, always.
        const bool restored = RestoreCamera(record, savedCamera);
        if (haveLatch)
        {
            __try
            {
                *reinterpret_cast<volatile int32_t*>(
                    base + kHalo2SaberSceneOnceLatchRva) = savedLatch;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        if (ok && !restored)
            CountBail(Bail::CameraRestoreFailed);
        if (ok && restored)
        {
            if (VR_Halo2CompleteSynchronousPair(generation, serial))
            {
                g_lastCompletedSerial.store(serial, std::memory_order_release);
                g_pairs.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            CountBail(Bail::PairCompleteRefused);
        }

        VR_Halo2InvalidateSynchronousPair(generation, serial);
        g_drops.fetch_add(1, std::memory_order_relaxed);
    }

    __declspec(noinline) void __fastcall SaberSceneDetour(
        void* ctx, void* rdx, uint32_t viewIndex)
    {
        const auto original =
            reinterpret_cast<SaberSceneFn>(
                g_originalScene.load(std::memory_order_acquire));
        if (!original)
            return;

        if (viewIndex < 32)
            g_viewIndexMask.fetch_or(1u << viewIndex, std::memory_order_relaxed);
        g_lastViewIndex.store(viewIndex, std::memory_order_relaxed);
        if (g_callbacks.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            LOG("Halo 2 Anniversary stereo: the remastered scene render "
                "detour FIRED for the first time; the binding is live");
        }

        // Our own second pass re-enters this detour; it must run stock.
        if (g_insideEyeLoop || !g_armed.load(std::memory_order_acquire) ||
            !g_levelLive.load(std::memory_order_acquire) ||
            !g_remasteredLive.load(std::memory_order_acquire) ||
            g_teardown.load(std::memory_order_acquire))
        {
            original(ctx, rdx, viewIndex);
            return;
        }

        g_activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        g_insideEyeLoop = true;
        __try
        {
            __try { EyeLoopBody(original, ctx, rdx, viewIndex); }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_exceptions.fetch_add(1, std::memory_order_relaxed);
            }
        }
        __finally
        {
            g_insideEyeLoop = false;
            g_activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        }
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

    bool RemoveCore(const char* reason) noexcept
    {
        g_armed.store(false, std::memory_order_release);
        g_teardown.store(true, std::memory_order_release);
        if (g_sceneTarget)
        {
            if (MH_DisableHook(g_sceneTarget) != MH_OK)
                return false;
            for (int i = 0; i < 200 &&
                 g_activeCallbacks.load(std::memory_order_acquire); ++i)
            {
                Sleep(10);
            }
            if (g_activeCallbacks.load(std::memory_order_acquire))
                return false;
            (void)MH_RemoveHook(g_sceneTarget);
            g_sceneTarget = nullptr;
        }
        g_originalScene.store(0, std::memory_order_release);
        g_rebuildMatrices.store(0, std::memory_order_release);
        g_cameraCommit.store(0, std::memory_order_release);
        g_moduleBase.store(0, std::memory_order_release);
        g_observerResult.store(0, std::memory_order_release);
        g_generation.store(0, std::memory_order_release);
        g_installed.store(false, std::memory_order_release);
        g_referenceValid.store(false, std::memory_order_release);
        g_recenterRequested.store(true, std::memory_order_release);
        g_lastCompletedSerial.store(0, std::memory_order_release);
        g_coreState = CoreState::StockFallback;
        if (g_moduleReference)
        {
            FreeLibrary(g_moduleReference);
            g_moduleReference = nullptr;
        }
        if (reason)
            LOG("Halo 2 Anniversary stereo removed (%s); stock rendering restored",
                reason);
        return true;
    }

    bool InstallCore(
        uintptr_t base, uint32_t generation, uintptr_t observerResult) noexcept
    {
        if (g_rejectedGeneration == generation)
            return false;

        const uintptr_t scene = base + kHalo2SaberSceneRenderRva;
        const uintptr_t rebuild = base + kHalo2SaberRebuildViewMatricesRva;
        const uintptr_t commit = base + kHalo2SaberCameraCommitRva;
        if (!IsExecutable(scene) || !IsExecutable(rebuild) || !IsExecutable(commit))
        {
            LOG("Halo 2 Anniversary stereo WITHHELD: scene=%d rebuild=%d "
                "commit=%d executable; stock rendering is untouched",
                IsExecutable(scene) ? 1 : 0, IsExecutable(rebuild) ? 1 : 0,
                IsExecutable(commit) ? 1 : 0);
            g_rejectedGeneration = generation;
            return false;
        }

        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(base), &module) || !module)
        {
            return false;
        }

        void* trampoline = nullptr;
        const MH_STATUS created = MH_CreateHook(
            reinterpret_cast<void*>(scene),
            reinterpret_cast<void*>(&SaberSceneDetour), &trampoline);
        if (created != MH_OK || !trampoline)
        {
            LOG("Halo 2 Anniversary stereo WITHHELD: scene hook create=%d",
                static_cast<int>(created));
            if (created == MH_OK)
                (void)MH_RemoveHook(reinterpret_cast<void*>(scene));
            FreeLibrary(module);
            g_rejectedGeneration = generation;
            return false;
        }

        g_moduleReference = module;
        g_sceneTarget = reinterpret_cast<void*>(scene);
        g_originalScene.store(
            reinterpret_cast<uintptr_t>(trampoline), std::memory_order_release);
        g_rebuildMatrices.store(rebuild, std::memory_order_release);
        g_cameraCommit.store(commit, std::memory_order_release);
        g_moduleBase.store(base, std::memory_order_release);
        g_observerResult.store(observerResult, std::memory_order_release);
        g_generation.store(generation, std::memory_order_release);
        g_teardown.store(false, std::memory_order_release);
        g_referenceValid.store(false, std::memory_order_release);
        g_recenterRequested.store(true, std::memory_order_release);
        g_lastCompletedSerial.store(0, std::memory_order_release);
        g_installed.store(true, std::memory_order_release);
        g_coreState = CoreState::CleanupRequired;

        if (MH_EnableHook(reinterpret_cast<void*>(scene)) != MH_OK)
        {
            LOG("Halo 2 Anniversary stereo WITHHELD: scene hook enable failed");
            (void)RemoveCore("install rollback");
            g_rejectedGeneration = generation;
            return false;
        }
        g_coreState = CoreState::Installed;
        LOG("Halo 2 Anniversary stereo installed: scene render +0x%X run twice "
            "per frame, per-eye camera written into the view record at "
            "collection+0x150+view*0x758+0x20 and rebuilt by the engine's own "
            "+0x%X; every owned byte is restored",
            static_cast<unsigned>(kHalo2SaberSceneRenderRva),
            static_cast<unsigned>(kHalo2SaberRebuildViewMatricesRva));
        return true;
    }

    void Report() noexcept
    {
        const uint64_t now = GetTickCount64();
        if (now - g_lastReportMs < 2000)
            return;
        g_lastReportMs = now;
        const uint64_t pairs = g_pairs.load(std::memory_order_relaxed);
        const uint64_t callbacks = g_callbacks.load(std::memory_order_relaxed);
        // Report on ANY movement, not just completed pairs. "Zero pairs" and
        // "the hook never fired" look identical from the player's seat, and
        // telling them apart is the whole reason four earlier Halo 2
        // candidates were expensive to diagnose.
        if (pairs == g_lastPairsReported && callbacks == g_lastCallbacksReported)
            return;
        g_lastPairsReported = pairs;
        g_lastCallbacksReported = callbacks;
        // Fixed-size, stack-only: one line per report, every nonzero reason.
        char reasons[512];
        size_t used = 0;
        reasons[0] = '\0';
        for (size_t i = 0; i < static_cast<size_t>(Bail::Count); ++i)
        {
            const uint64_t n = g_bails[i].load(std::memory_order_relaxed);
            if (!n || used >= sizeof(reasons) - 1)
                continue;
            const int written = _snprintf_s(
                reasons + used, sizeof(reasons) - used, _TRUNCATE,
                "%s%s=%llu", used ? " " : "", kBailNames[i],
                static_cast<unsigned long long>(n));
            if (written > 0)
                used += static_cast<size_t>(written);
        }
        LOG("Halo 2 Anniversary stereo: %llu scene callbacks, %llu eye pairs, "
            "%llu drops, %llu stock passes, %llu exceptions; pose published=%llu "
            "rederived=%llu self=%llu; views seen mask=0x%X last=%u; "
            "bail reasons: %s",
            static_cast<unsigned long long>(g_callbacks.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(pairs),
            static_cast<unsigned long long>(g_drops.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_stockPasses.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_exceptions.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_posePublished.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_poseRederived.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_poseSelfTracked.load(std::memory_order_relaxed)),
            g_viewIndexMask.load(std::memory_order_relaxed),
            g_lastViewIndex.load(std::memory_order_relaxed),
            used ? reasons : "none");
    }
}

bool Halo2AnniversaryStereo_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed,
    bool remasteredRendererLive, uintptr_t observerResultArray) noexcept
{
    uint32_t vrFailure = g_vrFailureGeneration.load(std::memory_order_acquire);
    if (vrFailure && generation && generation != vrFailure)
    {
        g_vrFailureGeneration.compare_exchange_strong(
            vrFailure, 0, std::memory_order_acq_rel, std::memory_order_acquire);
        vrFailure = g_vrFailureGeneration.load(std::memory_order_acquire);
    }

    const bool vrAvailable = !vrFailure || generation != vrFailure;
    const bool desired = moduleBase && generation &&
        moduleSize == kHalo2RetailImageSize && activeAndRange && levelRunning &&
        coldPassed && vrAvailable && observerResultArray != 0 &&
        remasteredRendererLive;

    g_levelLive.store(levelRunning, std::memory_order_release);
    g_remasteredLive.store(remasteredRendererLive, std::memory_order_release);

    const uint32_t owned = g_generation.load(std::memory_order_acquire);
    const bool foreignModule = g_sceneTarget &&
        (owned != generation ||
         g_moduleBase.load(std::memory_order_acquire) != moduleBase);

    if (!desired || foreignModule)
    {
        if (g_installed.load(std::memory_order_acquire))
        {
            (void)RemoveCore(foreignModule ? "module generation changed"
                                           : "level or title no longer eligible");
        }
        if (generation != g_rejectedGeneration)
            g_rejectedGeneration = 0;
        return false;
    }

    if (g_coreState != CoreState::Installed)
    {
        if (!InstallCore(moduleBase, generation, observerResultArray))
            return false;
    }

    g_armed.store(true, std::memory_order_release);
    if (g_armedLoggedGeneration != generation)
    {
        g_armedLoggedGeneration = generation;
        LOG("Halo 2 Anniversary stereo armed: the remastered scene render runs "
            "once per eye from one game frame; a frame that cannot produce a "
            "complete pair renders stock exactly once and never blacks out");
    }
    Report();
    return true;
}

bool Halo2AnniversaryStereo_Installed() noexcept
{
    return g_installed.load(std::memory_order_acquire);
}

bool Halo2AnniversaryStereo_Armed() noexcept
{
    return g_armed.load(std::memory_order_acquire);
}

uint32_t Halo2AnniversaryStereo_Generation() noexcept
{
    return g_generation.load(std::memory_order_acquire);
}

void Halo2AnniversaryStereo_RequestRecenter() noexcept
{
    g_recenterRequested.store(true, std::memory_order_release);
}

void Halo2AnniversaryStereo_ShutdownForVrFailure() noexcept
{
    const uint32_t generation = g_generation.load(std::memory_order_acquire);
    if (generation)
        g_vrFailureGeneration.store(generation, std::memory_order_release);
    g_armed.store(false, std::memory_order_release);
    g_teardown.store(true, std::memory_order_release);
}

#else

bool Halo2AnniversaryStereo_Poll(
    uintptr_t, size_t, uint32_t, bool, bool, bool, bool, uintptr_t) noexcept
{
    return false;
}
bool Halo2AnniversaryStereo_Installed() noexcept { return false; }
bool Halo2AnniversaryStereo_Armed() noexcept { return false; }
uint32_t Halo2AnniversaryStereo_Generation() noexcept { return 0; }
void Halo2AnniversaryStereo_RequestRecenter() noexcept {}
void Halo2AnniversaryStereo_ShutdownForVrFailure() noexcept {}

#endif
