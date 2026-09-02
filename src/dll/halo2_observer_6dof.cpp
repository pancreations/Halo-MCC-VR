#include "halo2_observer_6dof.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <intrin.h>

#include "../common/halo2_render_logic.h"
#include "../common/config.h"
#include "../common/log.h"
#include "game.h"
#include "halo2_stereo_core.h"
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

    // E-H2-75 / Stage 3AK: Halo 2 Classic's first-person muzzle effect enters
    // this particle renderer. The hook is optional and fail-open: its failure
    // never changes camera ownership. Four 32-bit arguments preserve the
    // pinned function's RCX/RDX/R8/R9 ABI; only RDX's low byte is classified.
    using Halo2ParticleRendererFn = void(__fastcall*)(
        uint32_t, uint32_t, uint32_t, uint32_t);
    constexpr uint32_t kHalo2ParticleRendererRva = 0x0076DC90;
    constexpr char kHalo2ParticleRendererPattern[] =
        "48 8B C4 88 50 10 89 48 08 55 53 56 57 41 55";
    void* g_particleTarget = nullptr;
    std::atomic<uintptr_t> g_particleOriginal{0};
    std::atomic<uint32_t> g_particleActiveCallbacks{0};
    std::atomic<uint64_t> g_particleSuppressed{0};
    std::atomic<uint64_t> g_particleReadFaults{0};
    std::atomic<bool> g_particleHitPending{false};
    uint32_t g_particleRejectedGeneration = 0;
    bool g_particleHitLogged = false;

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
    // E-H2-70 (C-H2-82): the camera the ENGINE handed the packet builder for
    // the pass whose packets the mod just owned - the frame the final
    // first-person nodes are composed against. Seqlocked; read by each
    // stereo core to compare with the camera its renderer will actually
    // draw those nodes from. Separate per renderer, because the two
    // builder calls are distinct engine calls with their own arguments.
    std::atomic<uint32_t> g_packetCameraVersion[2]{};
    Halo2CameraBasis g_packetCamera[2]{};
    std::atomic<bool> g_packetCameraValid[2]{};
    // C-H2-71: independent fail-open visibility-cover telemetry. The observer
    // pose remains owned even when this optional one-float write is refused.
    std::atomic<uint64_t> g_visibilityCoverApplied{0};
    std::atomic<uint64_t> g_visibilityCoverAlreadyWide{0};
    std::atomic<uint64_t> g_visibilityCoverRefused{0};
    std::atomic<uint32_t> g_visibilityCoverStockBits{0};
    std::atomic<uint32_t> g_visibilityCoverRequiredBits{0};
    std::atomic<uint32_t> g_visibilityCoverWrittenBits{0};
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
    using Halo2MatrixComposeFn = void(__fastcall*)(
        const float*, const float*, float*);
    void* g_finalPaletteTarget = nullptr;
    std::atomic<uintptr_t> g_finalPaletteOriginal{0};
    std::atomic<bool> g_finalPaletteReady{false};
    std::atomic<uint32_t> g_finalPaletteActiveCallbacks{0};
    std::atomic<uint64_t> g_finalPaletteCalls{0};
    std::atomic<uint64_t> g_finalPaletteMovedRight{0};
    std::atomic<uint64_t> g_finalPaletteMovedLeft{0};
    std::atomic<uint64_t> g_finalPaletteCollapsed{0};
    std::atomic<uint64_t> g_finalPaletteCoLocatedArms{0};
    std::atomic<uint64_t> g_finalPaletteRefused{0};
    using Halo2FirstPersonPacketBuilderFn = int(__fastcall*)(
        uint32_t, uint32_t, const float*, const float*, const float*, int,
        uint32_t*, uint8_t);
    void* g_packetBuilderTarget = nullptr;
    std::atomic<uintptr_t> g_packetBuilderOriginal{0};
    // E-H2-71 (C-H2-83): the interpolated first-person FRAME getter the
    // builder prefers for Classic only. While the mod's Classic build runs,
    // it reports "none", which makes the builder compose against
    // user_data+0x20C8 - the exact frame Anniversary composes against.
    void* g_interpFrameTarget = nullptr;
    std::atomic<uintptr_t> g_interpFrameOriginal{0};
    std::atomic<uint32_t> g_interpFrameActiveCallbacks{0};
    std::atomic<uint64_t> g_interpFrameCalls{0};
    std::atomic<uint64_t> g_interpFrameForcedCurrent{0};
    thread_local bool g_classicBuildInProgress = false;
    // C-H2-85: Halo 2 classic-only mount trim application counters.
    std::atomic<uint64_t> g_classicTrimApplied{0};
    std::atomic<uint64_t> g_classicTrimRefused{0};
    // E-H2-74 (C-H2-87): barrel meter, per consumer (0 classic, 1
    // anniversary), millidegrees, last sample. Measured at the packet
    // build from the numbers actually in play - no theory:
    //   carrierVsCompose  angle(carrier.forward, compose camera forward)
    //   carrierElevation  signed elevation of carrier.forward above the
    //                     compose camera's forward, about its right axis
    //   stockVsCompose    angle(stock gun root +X, compose camera forward)
    //   stockElevation    signed elevation of the stock root +X likewise
    // If the classic gun is drawn tipped, one of these says by how much.
    std::atomic<int32_t> g_meterCarrierVsCompose[2]{};
    std::atomic<int32_t> g_meterCarrierElevation[2]{};
    std::atomic<int32_t> g_meterStockVsCompose[2]{};
    std::atomic<int32_t> g_meterStockElevation[2]{};

    void PublishBarrelMeter(
        int slot, const Halo2CameraBasis& compose,
        const Halo2CameraBasis& carrier, const float* gunRootNode) noexcept
    {
        if (slot < 0 || slot > 1) return;
        auto unit = [](const float v[3], float out[3]) -> bool {
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (!std::isfinite(l) || l < 1e-5f) return false;
            out[0] = v[0] / l; out[1] = v[1] / l; out[2] = v[2] / l;
            return true;
        };
        float f[3], u[3];
        if (!unit(compose.forward, f) || !unit(compose.up, u)) return;
        const float r[3] = {
            f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2],
            f[0] * u[1] - f[1] * u[0]};
        auto publish = [&](const float dirIn[3], std::atomic<int32_t>& angle,
                           std::atomic<int32_t>& elevation) {
            float d[3];
            if (!unit(dirIn, d)) return;
            const float c = std::clamp(d[0] * f[0] + d[1] * f[1] + d[2] * f[2],
                                       -1.0f, 1.0f);
            const float total = std::acos(c) * 57.29578f;
            // elevation: angle of the direction above the compose forward
            // within the vertical plane (forward, up) - positive = higher
            const float upC = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
            const float fwdC = c;
            const float elev = std::atan2(upC, fwdC) * 57.29578f;
            (void)r;
            if (std::isfinite(total))
                angle.store(static_cast<int32_t>(total * 1000.0f),
                            std::memory_order_relaxed);
            if (std::isfinite(elev))
                elevation.store(static_cast<int32_t>(elev * 1000.0f),
                                std::memory_order_relaxed);
        };
        publish(carrier.forward, g_meterCarrierVsCompose[slot],
                g_meterCarrierElevation[slot]);
        if (gunRootNode)
        {
            // node layout: [0] scale, [1..3] column 0 (= root +X axis in
            // packet space, kHalo2FirstPersonNodeFloats stride)
            const float rootX[3] = {gunRootNode[1], gunRootNode[2], gunRootNode[3]};
            publish(rootX, g_meterStockVsCompose[slot],
                    g_meterStockElevation[slot]);
        }
    }
    std::atomic<uint32_t> g_packetBuilderActiveCallbacks{0};
    std::atomic<uint64_t> g_packetBuilderCalls{0};
    std::atomic<uint64_t> g_packetBuilderApplied{0};
    std::atomic<uint64_t> g_packetBuilderStock{0};
    std::atomic<uint64_t> g_packetBuilderGunNodes{0};
    std::atomic<uint64_t> g_packetBuilderLastAppliedMs{0};
    std::atomic<uint64_t> g_packetBuilderEligible{0};
    std::atomic<uint64_t> g_packetBuilderChiefContexts{0};
    std::atomic<uint64_t> g_packetBuilderEliteContexts{0};
    std::atomic<uint64_t> g_packetBuilderChiefApplied{0};
    std::atomic<uint64_t> g_packetBuilderEliteApplied{0};
    std::atomic<uint32_t> g_packetBuilderLastChiefFlags{0};
    std::atomic<uint32_t> g_packetBuilderLastEliteFlags{0};
    std::atomic<uint64_t> g_packetBuilderWeaponStateMiss{0};
    std::atomic<uint64_t> g_packetBuilderPublicationMiss{0};
    std::atomic<uint64_t> g_packetBuilderControllerSnapshotMiss{0};
    std::atomic<uint64_t> g_packetBuilderCarrierMiss{0};
    std::atomic<uint64_t> g_packetBuilderOwnershipMiss{0};
    std::atomic<uint32_t> g_packetBuilderRightDeltaMillimeters{0};
    std::atomic<uint32_t> g_packetBuilderLeftDeltaMillimeters{0};
    std::atomic<uint32_t> g_packetBuilderMaxRightDeltaMillimeters{0};
    std::atomic<uint32_t> g_packetBuilderMaxLeftDeltaMillimeters{0};
    using Halo2VisibleFirstPersonConsumerFn = void(__fastcall*)(
        uint32_t, uint32_t, uint32_t, int32_t, float*);
    void* g_visibleConsumerTarget = nullptr;
    std::atomic<uintptr_t> g_visibleConsumerOriginal{0};
    std::atomic<uint32_t> g_visibleConsumerActiveCallbacks{0};
    std::atomic<uint64_t> g_visibleConsumerCalls{0};
    std::atomic<uint64_t> g_visibleConsumerHandsApplied{0};
    std::atomic<uint64_t> g_visibleConsumerGunsApplied{0};

    struct Halo2VisibleConsumerContext
    {
        bool valid = false;
        uint32_t user = UINT32_MAX;
        uint32_t unitObject = UINT32_MAX;
        uint32_t weaponObject = UINT32_MAX;
        uint32_t handsCount = 0;
        uint32_t gunCount = 0;
        const int32_t* handsRemap = nullptr;
        Halo2FirstPersonArmBinding binding{};
        Halo2CameraBasis renderCamera{};
        Halo2CameraBasis rightCarrier{};
        Halo2CameraBasis leftCarrier{};
        bool twoHandAimActive = false;
        float rightScale = 1.0f;
        float leftScale = 1.0f;
        float worldScale = 1.0f;
        bool handsApplied = false;
        bool gunApplied = false;
        Halo2FirstPersonTransform rightDelta{};
        bool handsDeferred = false;
        uint32_t handsModelObject = UINT32_MAX;
        uint32_t handsOwnerObject = UINT32_MAX;
        float* handsMatrices = nullptr;
    };
    thread_local Halo2VisibleConsumerContext g_visibleConsumerContext{};

    struct Halo2ClassicPacketContext
    {
        bool valid = false;
        bool eyeActive = false;
        float* hands = nullptr;
        float* gun = nullptr;
        uint32_t handsCount = 0;
        uint32_t gunCount = 0;
        uint64_t appliedAtMs = 0;
        float handsBackup[
            kHalo2FirstPersonPaletteCapacity *
            kHalo2FirstPersonNodeFloats]{};
        float gunBackup[
            kHalo2FirstPersonPaletteCapacity *
            kHalo2FirstPersonNodeFloats]{};
    };
    thread_local Halo2ClassicPacketContext g_classicPacketContext{};
    std::atomic<uint64_t> g_classicPacketCalls{0};
    std::atomic<uint64_t> g_classicPacketApplied{0};
    std::atomic<uint64_t> g_classicEyeCalls{0};
    std::atomic<uint64_t> g_classicEyeCompensated{0};
    std::atomic<uint64_t> g_classicEyeNoPacket{0};
    std::atomic<uint64_t> g_classicEyeNoPass{0};
    std::atomic<uint64_t> g_classicEyeRefused{0};

    using Halo2NativeAimUpdateFn = uint64_t(__fastcall*)(uint32_t);
    using Halo2ObjectDatumAccessorFn = void*(__fastcall*)(const void*);
    void* g_nativeAimTarget = nullptr;
    std::atomic<uintptr_t> g_nativeAimOriginal{0};
    std::atomic<uintptr_t> g_objectDatumAccessor{0};
    std::atomic<uint32_t> g_nativeAimActiveCallbacks{0};
    std::atomic<uint64_t> g_nativeAimCalls{0};
    std::atomic<uint64_t> g_nativeAimApplied{0};
    std::atomic<uint64_t> g_nativeAimNonOwned{0};
    std::atomic<uint64_t> g_nativeAimRefused{0};

    struct Halo2FinalPaletteContext
    {
        bool valid = false;
        const float* source = nullptr;
        uint32_t count = 0;
        Halo2FirstPersonArmBinding binding{};
        Halo2CameraBasis rightCarrier{};
        Halo2CameraBasis leftCarrier{};
        bool rightDeltaValid = false;
        bool leftDeltaValid = false;
        float rightRotation[9]{};
        float leftRotation[9]{};
        float rightStockPosition[3]{};
        float leftStockPosition[3]{};
        float rightDesiredPosition[3]{};
        float leftDesiredPosition[3]{};
    };
    thread_local Halo2FinalPaletteContext g_finalPaletteContext{};
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
    std::atomic<uint64_t> g_weaponAimChanged{0};
    std::atomic<uint32_t> g_weaponAimMaxDeflectionMilliDegrees{0};
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
        snapshot.rightAimValid = sample.rightAimValid;
        std::memcpy(snapshot.rightAimOrientation,
                    sample.rightAimOrientation,
                    sizeof(snapshot.rightAimOrientation));
        std::memcpy(snapshot.rightAimPosition, sample.rightAimPosition,
                    sizeof(snapshot.rightAimPosition));
        snapshot.twoHandAimActive = sample.twoHandAimActive;
        snapshot.leftControllerValid = sample.leftControllerValid;
        std::memcpy(snapshot.leftControllerOrientation,
                    sample.leftControllerOrientation,
                    sizeof(snapshot.leftControllerOrientation));
        std::memcpy(snapshot.leftControllerPosition,
                    sample.leftControllerPosition,
                    sizeof(snapshot.leftControllerPosition));
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

    void ApplyUpstreamVisibilityCover(
        uintptr_t result,
        const Halo2SynchronousVrRenderSnapshot& snapshot) noexcept
    {
        if (!kHalo2C71UpstreamVisibilityCoverEnabled)
            return;
        if (!snapshot.eyes[0].fovValid || !snapshot.eyes[1].fovValid ||
            !Halo2ObserverVisibilityFovWriteAllowed(
                kHalo2ObserverResultVerticalFovOffset, sizeof(float)))
        {
            g_visibilityCoverRefused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        float stock = 0.0f;
        float cover = 0.0f;
        if (!ReadFloats(
                result + kHalo2ObserverResultVerticalFovOffset, &stock, 1) ||
            !Halo2DeriveObserverVisibilityVerticalFov(
                snapshot.eyes[0].fov, snapshot.eyes[1].fov, stock, cover))
        {
            g_visibilityCoverRefused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Halo2SaberEyeCover required{};
        if (!Halo2DeriveSaberEyeCover(
                snapshot.eyes[0].fov, snapshot.eyes[1].fov, required))
        {
            g_visibilityCoverRefused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const float requiredRadians = required.halfVerticalRadians * 2.0f;
        uint32_t bits = 0;
        std::memcpy(&bits, &stock, sizeof(bits));
        g_visibilityCoverStockBits.store(bits, std::memory_order_relaxed);
        std::memcpy(&bits, &requiredRadians, sizeof(bits));
        g_visibilityCoverRequiredBits.store(bits, std::memory_order_relaxed);
        std::memcpy(&bits, &cover, sizeof(bits));
        g_visibilityCoverWrittenBits.store(bits, std::memory_order_relaxed);
        if (cover <= stock + 1.0e-6f)
        {
            g_visibilityCoverAlreadyWide.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!WriteFloats(
                result + kHalo2ObserverResultVerticalFovOffset, &cover, 1))
        {
            g_visibilityCoverRefused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_visibilityCoverApplied.fetch_add(1, std::memory_order_relaxed);
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
        // Optional and fail-open: the pose publication never depends on the
        // visibility-cover write succeeding.
        ApplyUpstreamVisibilityCover(result, snapshot);
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

    // Shared controller-to-Halo-2 transform for both visible placement and the
    // shot ray. The mesh receives gun_forward_m as a presentation trim. The
    // shot deliberately receives zero trim: the compositor places the visible
    // crosshair at `presented aim position + (-Z * distance)`, so adding the
    // mesh-only trim to that origin would create a second, parallel ray.
    //
    // VR_GetAimPose is the shared, mount-calibrated aim pose (gun_yaw_deg /
    // gun_pitch_deg / gun_roll_deg and two-handed aim are already folded into
    // it), and the firing caller supplies the exact stabilized pose the
    // compositor actually presented.
    bool BuildFirstPersonCarrierFromAimPose(
        const Halo2CameraBasis& renderCamera, const float aimOrientation[4],
        const float aimPosition[3], float forwardTrimMeters,
        float rightTrimMeters, float upTrimMeters,
        Halo2CameraBasis& carrier) noexcept
    {
        float headOrientation[4]{}, headPosition[3]{};
        if (!VR_GetHeadPose(headOrientation, headPosition))
        {
            return false;
        }
        return Halo2BuildControllerCarrier(
            renderCamera, headOrientation, headPosition, aimOrientation,
            aimPosition, Game_GetWorldScale(), forwardTrimMeters,
            rightTrimMeters, upTrimMeters, carrier);
    }

    bool BuildFirstPersonCarrier(
        const Halo2CameraBasis& renderCamera,
        Halo2CameraBasis& carrier) noexcept
    {
        float aimOrientation[4]{}, aimPosition[3]{};
        return VR_GetAimPose(aimOrientation, aimPosition) &&
            BuildFirstPersonCarrierFromAimPose(
                renderCamera, aimOrientation, aimPosition,
                std::clamp(g_config.gun_forward_m, -0.3f, 0.5f),
                std::clamp(g_config.gun_right_m, -0.3f, 0.3f),
                std::clamp(g_config.gun_up_m, -0.3f, 0.3f), carrier);
    }

    bool BuildStableFirstPersonCarriers(
        const Halo2ObserverPosePublication& publication,
        uint32_t generation,
        Halo2CameraBasis& rightCarrier,
        Halo2CameraBasis& leftCarrier) noexcept
    {
        if (!Halo2ObserverControllerSnapshotUsable(
                publication, generation))
            return false;
        const Halo2ObserverPoseSnapshot& tracking = publication.snapshot;
        float leftOrientation[4]{};
        if (!Halo2BuildMirroredLeftAimOrientation(
                tracking.leftControllerOrientation,
                g_config.gun_yaw_deg, g_config.gun_pitch_deg,
                g_config.gun_roll_deg, leftOrientation))
            return false;
        const float worldScale = Game_GetWorldScale();
        return Halo2BuildStableControllerCarrier(
                   publication.stock, publication.referenceOrientation,
                   publication.referencePosition,
                   tracking.rightAimOrientation,
                   tracking.rightAimPosition, worldScale,
                   std::clamp(g_config.gun_forward_m, -0.3f, 0.5f),
                   std::clamp(g_config.gun_right_m, -0.3f, 0.3f),
                   std::clamp(g_config.gun_up_m, -0.3f, 0.3f),
                   rightCarrier) &&
            Halo2BuildStableControllerCarrier(
                   publication.stock, publication.referenceOrientation,
                   publication.referencePosition, leftOrientation,
                   tracking.leftControllerPosition, worldScale,
                   std::clamp(
                       g_config.left_hand_forward_m, -0.15f, 0.30f),
                   leftCarrier);
    }

    bool PrepareFinalPaletteContext(
        const Halo2CameraBasis& renderCamera, int graphId,
        const float* source, uint32_t count) noexcept
    {
        g_finalPaletteContext = Halo2FinalPaletteContext{};
        if (!kHalo2FinalPaletteControllerOwnershipEnabled || !source ||
            !count || count > kHalo2FirstPersonPaletteCapacity ||
            !Game_Halo2ControllerAimActive() ||
            !g_finalPaletteReady.load(std::memory_order_acquire))
        {
            return false;
        }
        Halo2FinalPaletteContext candidate{};
        float leftOrientation[4]{}, leftPosition[3]{};
        float headOrientation[4]{}, headPosition[3]{};
        if (!ResolveArmBinding(graphId, count, candidate.binding) ||
            !BuildFirstPersonCarrier(renderCamera, candidate.rightCarrier) ||
            !VR_GetHeadPose(headOrientation, headPosition) ||
            !VR_GetLeftControllerPose(leftOrientation, leftPosition) ||
            !Halo2BuildControllerCarrier(
                renderCamera, headOrientation, headPosition, leftOrientation,
                leftPosition, Game_GetWorldScale(),
                std::clamp(g_config.left_hand_forward_m, -0.3f, 0.5f),
                candidate.leftCarrier))
        {
            return false;
        }
        candidate.source = source;
        candidate.count = count;
        candidate.valid = true;
        g_finalPaletteContext = candidate;
        return true;
    }

    __declspec(noinline) void __fastcall Halo2FinalPaletteComposeDetour(
        const float* root, const float* source, float* destination)
    {
        g_finalPaletteActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
        const auto original = reinterpret_cast<Halo2MatrixComposeFn>(
            g_finalPaletteOriginal.load(std::memory_order_acquire));
        if (original)
            original(root, source, destination);

        bool changed = false;
        __try
        {
            const uintptr_t base = g_moduleBase.load(std::memory_order_acquire);
            const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
            Halo2FinalPaletteContext& context = g_finalPaletteContext;
            const bool admittedCaller = base &&
                (caller == base + kHalo2FirstPersonPrimaryComposeReturnRva ||
                 caller == base + kHalo2FirstPersonSecondaryComposeReturnRva);
            if (original && admittedCaller && destination && source &&
                context.valid && context.source && context.count &&
                context.count <= kHalo2FirstPersonPaletteCapacity)
            {
                const uintptr_t first =
                    reinterpret_cast<uintptr_t>(context.source);
                const uintptr_t address = reinterpret_cast<uintptr_t>(source);
                const uintptr_t bytes = static_cast<uintptr_t>(context.count) *
                    kHalo2FirstPersonNodeStride;
                if (first <= UINTPTR_MAX - bytes && address >= first &&
                    address < first + bytes &&
                    (address - first) % kHalo2FirstPersonNodeStride == 0)
                {
                    const uint32_t index = static_cast<uint32_t>(
                        (address - first) / kHalo2FirstPersonNodeStride);
                    const uint64_t bit = uint64_t{1} << index;
                    const bool right = (context.binding.rightSubtree & bit) != 0;
                    const bool left = (context.binding.leftSubtree & bit) != 0;
                    if (right && index == static_cast<uint32_t>(
                            context.binding.rightWrist))
                    {
                        context.rightDeltaValid =
                            Halo2BuildFinalPaletteWristDelta(
                                destination, context.rightCarrier,
                                context.rightRotation,
                                context.rightStockPosition,
                                context.rightDesiredPosition);
                    }
                    if (left && index == static_cast<uint32_t>(
                            context.binding.leftWrist))
                    {
                        context.leftDeltaValid =
                            Halo2BuildFinalPaletteWristDelta(
                                destination, context.leftCarrier,
                                context.leftRotation,
                                context.leftStockPosition,
                                context.leftDesiredPosition);
                    }
                    if (right && context.rightDeltaValid)
                    {
                        Halo2ReanchorFirstPersonNode(
                            destination, context.rightRotation,
                            context.rightStockPosition,
                            context.rightDesiredPosition);
                        g_finalPaletteMovedRight.fetch_add(
                            1, std::memory_order_relaxed);
                        changed = true;
                    }
                    else if (left && context.leftDeltaValid)
                    {
                        Halo2ReanchorFirstPersonNode(
                            destination, context.leftRotation,
                            context.leftStockPosition,
                            context.leftDesiredPosition);
                        g_finalPaletteMovedLeft.fetch_add(
                            1, std::memory_order_relaxed);
                        changed = true;
                    }
                    else if (!right && !left && std::isfinite(destination[0]))
                    {
                        // This is the final independent palette: no hierarchy
                        // is evaluated after it. Collapse every non-hand node,
                        // matching Halo 3/Reach/Halo 4, so no arm can remain on
                        // either controller while the held gun stays in the
                        // right-wrist subtree.
                        destination[0] *= 0.0001f;
                        g_finalPaletteCollapsed.fetch_add(
                            1, std::memory_order_relaxed);
                        changed = true;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_finalPaletteRefused.fetch_add(1, std::memory_order_relaxed);
        }
        if (changed)
            g_finalPaletteCalls.fetch_add(1, std::memory_order_relaxed);
        g_finalPaletteActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    __declspec(noinline) void __fastcall Halo2VisibleFirstPersonConsumerDetour(
        uint32_t user, uint32_t modelObject, uint32_t ownerObject,
        int32_t weaponSlot, float* matrices)
    {
        g_visibleConsumerActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
        g_visibleConsumerCalls.fetch_add(1, std::memory_order_relaxed);
        const auto original =
            reinterpret_cast<Halo2VisibleFirstPersonConsumerFn>(
                g_visibleConsumerOriginal.load(std::memory_order_acquire));
        auto& context = g_visibleConsumerContext;
        bool callCurrentOriginal = true;
        if (original && matrices && context.valid && user == context.user &&
            g_armed.load(std::memory_order_acquire) &&
            g_levelLive.load(std::memory_order_acquire) &&
            !g_teardownRequested.load(std::memory_order_acquire))
        {
            __try
            {
                if (!context.handsDeferred && weaponSlot == -1 &&
                    ownerObject == context.unitObject)
                {
                    context.handsDeferred = true;
                    context.handsModelObject = modelObject;
                    context.handsOwnerObject = ownerObject;
                    context.handsMatrices = matrices;
                    callCurrentOriginal = false;
                }
                else if (!context.gunApplied && context.handsDeferred &&
                          weaponSlot == 0 &&
                          ownerObject == context.weaponObject)
                {
                    Halo2FinalPacketOwnershipResult result{};
                    PublishBarrelMeter(
                        1, context.renderCamera, context.rightCarrier,
                        matrices);
                    if (Halo2OwnFinalFirstPersonPackets(
                            context.handsMatrices, context.handsCount,
                            context.handsRemap, context.binding, matrices,
                            context.gunCount, context.renderCamera,
                            context.rightCarrier, context.leftCarrier,
                            context.twoHandAimActive, context.rightScale,
                            context.leftScale, context.worldScale, result))
                    {
                        context.handsApplied = true;
                        context.gunApplied = true;
                        g_visibleConsumerHandsApplied.fetch_add(
                            1, std::memory_order_relaxed);
                        g_visibleConsumerGunsApplied.fetch_add(
                            1, std::memory_order_relaxed);
                        g_finalPaletteCalls.fetch_add(1, std::memory_order_relaxed);
                        g_finalPaletteMovedRight.fetch_add(
                            result.rightNodes, std::memory_order_relaxed);
                        g_finalPaletteMovedLeft.fetch_add(
                            result.leftNodes, std::memory_order_relaxed);
                        g_finalPaletteCollapsed.fetch_add(
                            result.collapsedNodes, std::memory_order_relaxed);
                        g_finalPaletteCoLocatedArms.fetch_add(
                            result.coLocatedArmNodes, std::memory_order_relaxed);
                        g_packetBuilderGunNodes.fetch_add(
                            result.gunNodes, std::memory_order_relaxed);
                        const auto millimeters = [&context](float world) {
                            return static_cast<uint32_t>(std::min(
                                world / context.worldScale * 1000.0f,
                                100000.0f));
                        };
                        const uint32_t rightMm =
                            millimeters(result.rightWristDeltaWorld);
                        const uint32_t leftMm =
                            millimeters(result.leftWristDeltaWorld);
                        g_packetBuilderRightDeltaMillimeters.store(
                            rightMm, std::memory_order_relaxed);
                        g_packetBuilderLeftDeltaMillimeters.store(
                            leftMm, std::memory_order_relaxed);
                        auto publishMaximum = [](
                            std::atomic<uint32_t>& destination,
                            uint32_t value) {
                            uint32_t previous =
                                destination.load(std::memory_order_relaxed);
                            while (previous < value &&
                                   !destination.compare_exchange_weak(
                                       previous, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed))
                            {
                            }
                        };
                        publishMaximum(
                            g_packetBuilderMaxRightDeltaMillimeters, rightMm);
                        publishMaximum(
                            g_packetBuilderMaxLeftDeltaMillimeters, leftMm);
                        g_packetBuilderLastAppliedMs.store(
                            GetTickCount64(), std::memory_order_release);
                    }
                    original(user, context.handsModelObject,
                        context.handsOwnerObject, -1, context.handsMatrices);
                    context.handsDeferred = false;
                    context.handsMatrices = nullptr;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_finalPaletteRefused.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (original && callCurrentOriginal)
        {
            __try
            {
                original(user, modelObject, ownerObject, weaponSlot, matrices);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_finalPaletteRefused.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_visibleConsumerActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    // E-H2-71 (C-H2-83). Signature: bool(user_index, position_index, out).
    using Halo2InterpolatedFrameFn = uint8_t(__fastcall*)(int, int, void*);

    __declspec(noinline) uint8_t __fastcall Halo2InterpolatedFrameDetour(
        int userIndex, int positionIndex, void* out)
    {
        g_interpFrameActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
        g_interpFrameCalls.fetch_add(1, std::memory_order_relaxed);
        const auto original = reinterpret_cast<Halo2InterpolatedFrameFn>(
            g_interpFrameOriginal.load(std::memory_order_acquire));
        uint8_t result = 0;
        // Only the mod's own Classic first-person packet build is changed.
        // Every other caller - the world, the other renderer, any other
        // user - keeps the engine's interpolated frame exactly.
        const bool forceCurrent = g_classicBuildInProgress &&
            userIndex == static_cast<int>(kOwnedUser) &&
            g_armed.load(std::memory_order_acquire) &&
            g_levelLive.load(std::memory_order_acquire) &&
            !g_teardownRequested.load(std::memory_order_acquire);
        if (forceCurrent)
        {
            g_interpFrameForcedCurrent.fetch_add(1, std::memory_order_relaxed);
        }
        else if (original)
        {
            __try { result = original(userIndex, positionIndex, out); }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_exceptions.fetch_add(1, std::memory_order_relaxed);
                result = 0;
            }
        }
        g_interpFrameActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return result;
    }

    __declspec(noinline) int __fastcall Halo2FirstPersonPacketBuilderDetour(
        uint32_t user, uint32_t unitObject, const float* position,
        const float* forward, const float* up, int packetCapacity,
        uint32_t* packets, uint8_t publishToRenderer)
    {
        g_packetBuilderActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
        g_packetBuilderCalls.fetch_add(1, std::memory_order_relaxed);
        const auto original = reinterpret_cast<Halo2FirstPersonPacketBuilderFn>(
            g_packetBuilderOriginal.load(std::memory_order_acquire));
        g_visibleConsumerContext = Halo2VisibleConsumerContext{};
        const bool anniversaryConsumer = publishToRenderer != 0;
        const bool classicConsumer = !anniversaryConsumer &&
            Halo2Stereo_Armed() && !g_classicPacketContext.eyeActive;
        Halo2VisibleConsumerContext candidate{};
        if (classicConsumer)
        {
            g_classicPacketCalls.fetch_add(1, std::memory_order_relaxed);
            g_classicPacketContext.valid = false;
        }
        if (original && user == kOwnedUser &&
            (anniversaryConsumer || classicConsumer) && packets &&
            packetCapacity > 0 && g_armed.load(std::memory_order_acquire) &&
            g_levelLive.load(std::memory_order_acquire) &&
            !g_teardownRequested.load(std::memory_order_acquire) &&
            g_finalPaletteReady.load(std::memory_order_acquire))
        {
            __try
            {
                const uintptr_t module =
                    g_moduleBase.load(std::memory_order_acquire);
                const uintptr_t userArray = module
                    ? *reinterpret_cast<const volatile uintptr_t*>(
                          module + kHalo2FirstPersonUserDataPointerRva)
                    : 0;
                const uintptr_t weaponData = userArray
                    ? userArray + static_cast<uintptr_t>(user) *
                          kHalo2FirstPersonUserStride +
                          kHalo2FirstPersonWeaponDataOffset
                    : 0;
                const uint8_t flags = weaponData
                    ? *reinterpret_cast<const volatile uint8_t*>(weaponData)
                    : 0;
                const uint32_t weaponObject = weaponData
                    ? *reinterpret_cast<const volatile uint32_t*>(
                          weaponData + kHalo2FirstPersonWeaponObjectOffset)
                    : UINT32_MAX;
                const int graphId = weaponData
                    ? *reinterpret_cast<const volatile int32_t*>(
                          weaponData + kHalo2FirstPersonWeaponGraphOffset)
                    : -1;
                const uint32_t gunCount = weaponData
                    ? *reinterpret_cast<const volatile uint32_t*>(
                          weaponData + kHalo2FirstPersonWeaponNodeCountOffset)
                    : 0;
                const uint32_t handsCount = weaponData
                    ? *reinterpret_cast<const volatile uint32_t*>(
                          weaponData + kHalo2FirstPersonHandsNodeCountOffset)
                    : 0;
                const uint32_t animationCount = weaponData
                    ? *reinterpret_cast<const volatile uint32_t*>(
                          weaponData +
                          kHalo2FirstPersonAnimationNodeCountOffset)
                    : 0;
                const int32_t* const handsRemap = weaponData
                    ? reinterpret_cast<const int32_t*>(
                          weaponData + kHalo2FirstPersonHandsRemapOffset)
                    : nullptr;
                Halo2CameraBasis renderCamera{};
                if (position && forward && up)
                {
                    std::memcpy(renderCamera.position, position, 12);
                    std::memcpy(renderCamera.forward, forward, 12);
                    std::memcpy(renderCamera.up, up, 12);
                }
                Halo2FirstPersonArmBinding binding{};
                Halo2CameraBasis rightCarrier{}, leftCarrier{};
                Halo2ObserverPosePublication publication{};
                const uint32_t generation =
                    g_generation.load(std::memory_order_acquire);
                g_packetBuilderEligible.fetch_add(1, std::memory_order_relaxed);
                // The packet builder call plus its exact unit/weapon packet
                // identities are the ownership proof. The old 0x07 byte gate
                // described Chief's weapon_data state, but H2EK does not make
                // that byte a character-independent packet contract; applying
                // it here silently excluded Arbiter before his authored graph
                // and packets could be validated.
                if (weaponObject == UINT32_MAX || !gunCount || !handsCount ||
                    !handsRemap ||
                    !ResolveArmBinding(graphId, animationCount, binding) ||
                    !Halo2ValidateCameraBasis(renderCamera))
                {
                    g_packetBuilderWeaponStateMiss.fetch_add(
                        1, std::memory_order_relaxed);
                }
                else if (!Halo2Observer6Dof_ReadPublishedPose(publication))
                {
                    g_packetBuilderPublicationMiss.fetch_add(
                        1, std::memory_order_relaxed);
                }
                else if (!Halo2ObserverControllerSnapshotUsable(
                             publication, generation))
                {
                    g_packetBuilderControllerSnapshotMiss.fetch_add(
                        1, std::memory_order_relaxed);
                }
                else if (!BuildStableFirstPersonCarriers(
                             publication, generation, rightCarrier, leftCarrier))
                {
                    g_packetBuilderCarrierMiss.fetch_add(
                        1, std::memory_order_relaxed);
                }
                else
                {
                    // Mesh-only barrel trim from the ACTIVE title profile
                    // (Anniversary and Classic each carry their own): rotates
                    // the carriers, so the gun and the hands holding it turn
                    // together about the controller while the crosshair and
                    // the shot ray - which never read the carriers - stay
                    // put. The legacy halo2_classic_* keys add on top for
                    // Classic only. An invalid trim leaves the carriers as
                    // built.
                    {
                        const float extraPitch = classicConsumer
                            ? g_config.halo2_classic_gun_pitch_deg : 0.0f;
                        const float extraYaw = classicConsumer
                            ? g_config.halo2_classic_gun_yaw_deg : 0.0f;
                        const float extraRoll = classicConsumer
                            ? g_config.halo2_classic_gun_roll_deg : 0.0f;
                        const float extraForward = classicConsumer
                            ? g_config.halo2_classic_gun_forward_m : 0.0f;
                        const float extraRight = classicConsumer
                            ? g_config.halo2_classic_gun_right_m : 0.0f;
                        const float extraUp = classicConsumer
                            ? g_config.halo2_classic_gun_up_m : 0.0f;
                        const float pitch = g_config.barrel_pitch_deg + extraPitch;
                        const float yaw = g_config.barrel_yaw_deg + extraYaw;
                        const float roll = g_config.barrel_roll_deg + extraRoll;
                        const float worldScale = Game_GetWorldScale();
                        Halo2CameraBasis trimmedRight{}, trimmedLeft{};
                        if (Halo2ApplyCarrierTrim(
                                rightCarrier, pitch, yaw, roll, extraForward,
                                extraRight, extraUp, worldScale, trimmedRight) &&
                            Halo2ApplyCarrierTrim(
                                leftCarrier, pitch, yaw, roll, extraForward,
                                extraRight, extraUp, worldScale, trimmedLeft))
                        {
                            rightCarrier = trimmedRight;
                            leftCarrier = trimmedLeft;
                            g_classicTrimApplied.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        else
                        {
                            g_classicTrimRefused.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                    auto& context = candidate;
                    context.valid = true;
                    context.user = user;
                    context.unitObject = unitObject;
                    context.weaponObject = weaponObject;
                    context.handsCount = handsCount;
                    context.gunCount = gunCount;
                    context.handsRemap = handsRemap;
                    context.binding = binding;
                    context.renderCamera = renderCamera;
                    // E-H2-70: publish this renderer's packet-build camera.
                    {
                        const int slot = anniversaryConsumer ? 1 : 0;
                        g_packetCameraVersion[slot].fetch_add(
                            1, std::memory_order_acq_rel);
                        g_packetCamera[slot] = renderCamera;
                        g_packetCameraVersion[slot].fetch_add(
                            1, std::memory_order_acq_rel);
                        g_packetCameraValid[slot].store(
                            true, std::memory_order_release);
                    }
                    context.rightCarrier = rightCarrier;
                    context.leftCarrier = leftCarrier;
                    context.twoHandAimActive =
                        publication.snapshot.twoHandAimActive;
                    context.rightScale =
                        std::clamp(g_config.gun_scale, 0.3f, 3.0f);
                    context.leftScale =
                        std::clamp(g_config.left_hand_scale, 0.3f, 3.0f);
                    context.worldScale = Game_GetWorldScale();
                    if (binding.rigKind ==
                        Halo2FirstPersonRigKind::MasterChief)
                    {
                        g_packetBuilderChiefContexts.fetch_add(
                            1, std::memory_order_relaxed);
                        g_packetBuilderLastChiefFlags.store(
                            flags, std::memory_order_relaxed);
                    }
                    else if (binding.rigKind ==
                             Halo2FirstPersonRigKind::Elite)
                    {
                        g_packetBuilderEliteContexts.fetch_add(
                            1, std::memory_order_relaxed);
                        g_packetBuilderLastEliteFlags.store(
                            flags, std::memory_order_relaxed);
                    }
                    if (anniversaryConsumer)
                        g_visibleConsumerContext = context;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_finalPaletteRefused.fetch_add(1, std::memory_order_relaxed);
            }
        }

        int packetCount = 0;
        if (original)
        {
            // E-H2-71 (C-H2-83): for the CLASSIC build only, the engine
            // prefers the frame-INTERPOLATED first-person frame while
            // Anniversary always composes against the current tick frame
            // (user_data+0x20C8). Identical packets expressed in two
            // different frames read as a rotation - the muzzle rise. Mark
            // the classic build so the interpolated-frame getter reports
            // none, which is the engine's own documented fallback to that
            // same current frame.
            const bool markClassicBuild = classicConsumer;
            if (markClassicBuild)
                g_classicBuildInProgress = true;
            __try
            {
                packetCount = original(
                    user, unitObject, position, forward, up, packetCapacity,
                    packets, publishToRenderer);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_finalPaletteRefused.fetch_add(1, std::memory_order_relaxed);
            }
            if (markClassicBuild)
                g_classicBuildInProgress = false;
        }
        // The Anniversary callback normally publishes hands then gun. If a
        // malformed/empty packet set omits the gun, fail this optional alignment
        // transaction open by publishing the untouched deferred hands now.
        if (anniversaryConsumer && g_visibleConsumerContext.handsDeferred)
        {
            const auto consumer =
                reinterpret_cast<Halo2VisibleFirstPersonConsumerFn>(
                    g_visibleConsumerOriginal.load(std::memory_order_acquire));
            if (consumer)
            {
                __try
                {
                    consumer(user, g_visibleConsumerContext.handsModelObject,
                        g_visibleConsumerContext.handsOwnerObject, -1,
                        g_visibleConsumerContext.handsMatrices);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    g_finalPaletteRefused.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            g_visibleConsumerContext.handsDeferred = false;
            g_visibleConsumerContext.handsMatrices = nullptr;
        }
        bool applied = anniversaryConsumer &&
            g_visibleConsumerContext.handsApplied &&
            g_visibleConsumerContext.gunApplied;
        if (classicConsumer && candidate.valid && packetCount > 0 &&
            packetCount <= packetCapacity &&
            packetCount <= kHalo2FrameInterpolatorFirstPersonSlots + 1)
        {
            __try
            {
                float* handsMatrices = nullptr;
                float* gunMatrices = nullptr;
                for (int packetIndex = 0; packetIndex < packetCount;
                     ++packetIndex)
                {
                    uint8_t* const packet =
                        reinterpret_cast<uint8_t*>(packets) +
                        static_cast<uintptr_t>(packetIndex) *
                            kHalo2FirstPersonRenderPacketStride;
                    const uint32_t packetObject =
                        *reinterpret_cast<const uint32_t*>(packet + 4);
                    if (!handsMatrices && packetObject == candidate.unitObject)
                    {
                        handsMatrices = reinterpret_cast<float*>(
                            packet + kHalo2FirstPersonRenderPacketHeaderBytes);
                    }
                    else if (!gunMatrices &&
                             packetObject == candidate.weaponObject)
                    {
                        gunMatrices = reinterpret_cast<float*>(
                            packet + kHalo2FirstPersonRenderPacketHeaderBytes);
                    }
                }
                Halo2FinalPacketOwnershipResult packetResult{};
                float stockHands[
                    kHalo2FirstPersonPaletteCapacity *
                    kHalo2FirstPersonNodeFloats]{};
                float stockGun[
                    kHalo2FirstPersonPaletteCapacity *
                    kHalo2FirstPersonNodeFloats]{};
                const size_t handsBytes =
                    static_cast<size_t>(candidate.handsCount) *
                    kHalo2FirstPersonNodeStride;
                const size_t gunBytes =
                    static_cast<size_t>(candidate.gunCount) *
                    kHalo2FirstPersonNodeStride;
                const bool packetBoundsValid = handsMatrices && gunMatrices &&
                    candidate.handsCount > 0 &&
                    candidate.handsCount <= kHalo2FirstPersonPaletteCapacity &&
                    candidate.gunCount > 0 &&
                    candidate.gunCount <= kHalo2FirstPersonPaletteCapacity;
                if (packetBoundsValid)
                {
                    std::memcpy(stockHands, handsMatrices, handsBytes);
                    std::memcpy(stockGun, gunMatrices, gunBytes);
                    PublishBarrelMeter(
                        0, candidate.renderCamera, candidate.rightCarrier,
                        stockGun);
                }
                if (packetBoundsValid && Halo2OwnFinalFirstPersonPackets(
                        handsMatrices, candidate.handsCount,
                        candidate.handsRemap, candidate.binding, gunMatrices,
                        candidate.gunCount, candidate.renderCamera,
                        candidate.rightCarrier, candidate.leftCarrier,
                        candidate.twoHandAimActive, candidate.rightScale,
                        candidate.leftScale, candidate.worldScale, packetResult))
                {
                    auto& classic = g_classicPacketContext;
                    classic.hands = handsMatrices;
                    classic.gun = gunMatrices;
                    classic.handsCount = candidate.handsCount;
                    classic.gunCount = candidate.gunCount;
                    classic.appliedAtMs = GetTickCount64();
                    classic.eyeActive = false;
                    classic.valid = true;
                    applied = true;
                    g_classicPacketApplied.fetch_add(
                        1, std::memory_order_relaxed);
                    g_finalPaletteCalls.fetch_add(1, std::memory_order_relaxed);
                    g_finalPaletteMovedRight.fetch_add(
                        packetResult.rightNodes, std::memory_order_relaxed);
                    g_finalPaletteMovedLeft.fetch_add(
                        packetResult.leftNodes, std::memory_order_relaxed);
                    g_finalPaletteCollapsed.fetch_add(
                        packetResult.collapsedNodes, std::memory_order_relaxed);
                    g_finalPaletteCoLocatedArms.fetch_add(
                        packetResult.coLocatedArmNodes,
                        std::memory_order_relaxed);
                    g_packetBuilderGunNodes.fetch_add(
                        packetResult.gunNodes, std::memory_order_relaxed);
                    g_packetBuilderLastAppliedMs.store(
                        classic.appliedAtMs, std::memory_order_release);
                }
                else if (packetBoundsValid)
                {
                    std::memcpy(handsMatrices, stockHands, handsBytes);
                    std::memcpy(gunMatrices, stockGun, gunBytes);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_finalPaletteRefused.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (((anniversaryConsumer && g_visibleConsumerContext.valid) ||
             (classicConsumer && candidate.valid)) && !applied)
            g_packetBuilderOwnershipMiss.fetch_add(1, std::memory_order_relaxed);
        if (applied && candidate.valid)
        {
            if (candidate.binding.rigKind ==
                Halo2FirstPersonRigKind::MasterChief)
                g_packetBuilderChiefApplied.fetch_add(
                    1, std::memory_order_relaxed);
            else if (candidate.binding.rigKind ==
                     Halo2FirstPersonRigKind::Elite)
                g_packetBuilderEliteApplied.fetch_add(
                    1, std::memory_order_relaxed);
        }
        if (anniversaryConsumer)
            g_visibleConsumerContext = Halo2VisibleConsumerContext{};
        (applied ? g_packetBuilderApplied : g_packetBuilderStock)
            .fetch_add(1, std::memory_order_relaxed);
        g_packetBuilderActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return packetCount;
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
        if (slot == 0)
            g_finalPaletteContext = Halo2FinalPaletteContext{};
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
                if (slot == 0)
                {
                    (void)PrepareFinalPaletteContext(
                        pass.frame, id,
                        reinterpret_cast<const float*>(*nodesOut), *countOut);
                }
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
                            kHalo2RejectedInterpolatorControllerOwnershipEnabled &&
                            Game_Halo2ControllerAimActive();
                        if (floaty)
                        {
                            Halo2CameraBasis carrier{};
                            ok = BuildFirstPersonCarrier(pass.frame, carrier);
                            if (ok)
                            {
                                // The exact two-subtree binding is required for
                                // this presentation feature. A failure leaves
                                // the complete palette stock for this frame;
                                // falling back to C-H2-46's whole-slot carrier
                                // is what put both arms on the right hand.
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
                                    ok = false;
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
            if (kHalo2RejectedInterpolatorControllerOwnershipEnabled &&
                Game_Halo2ControllerAimActive() &&
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

    __declspec(noinline) uint64_t __fastcall Halo2NativeAimUpdateDetour(
        uint32_t objectIndex)
    {
        g_nativeAimActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
        g_nativeAimCalls.fetch_add(1, std::memory_order_relaxed);
        const auto original = reinterpret_cast<Halo2NativeAimUpdateFn>(
            g_nativeAimOriginal.load(std::memory_order_acquire));
        uint64_t result = 0;
        // C-H2-72 diagnostic: when VR owns Halo 2's local-player aim, do not
        // let stock unit_update_aiming run first. That stock transaction owns
        // more than the two direction vectors and can retain Halo 2's
        // controller aim-assist / target-acquisition state even though the mod
        // overwrites +0x168/+0x174 afterwards. Reports from the accepted 0.3.5
        // line describe aim assist remaining active and melee/lunge behaving
        // incorrectly. In the VR-owned case the controller publication is
        // already the authoritative sight line, so write the two H2EK-proven
        // native aim vectors directly. Outside VR-owned gameplay the original
        // updater remains completely stock.
        const bool directVrAim =
            original && Game_Halo2ControllerAimActive() &&
            Halo2Observer6Dof_DirectWeaponAimArmed();
        if (original && !directVrAim)
        {
            __try { result = original(objectIndex); }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_nativeAimRefused.fetch_add(1, std::memory_order_relaxed);
            }
        }

        bool applied = false;
        if (directVrAim)
        {
            __try
            {
                const uintptr_t module =
                    g_moduleBase.load(std::memory_order_acquire);
                uint32_t ownedUnit = UINT32_MAX;
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
                if (ownedUnit == UINT32_MAX || objectIndex != ownedUnit)
                {
                    g_nativeAimNonOwned.fetch_add(1, std::memory_order_relaxed);
                    __leave;
                }

                Halo2ObserverPosePublication publication{};
                Halo2CameraBasis rightCarrier{}, leftCarrier{};
                const uint32_t generation =
                    g_generation.load(std::memory_order_acquire);
                if (!Halo2Observer6Dof_ReadPublishedPose(publication) ||
                    !Halo2ObserverControllerSnapshotUsable(
                        publication, generation) ||
                    !BuildStableFirstPersonCarriers(
                        publication, generation, rightCarrier, leftCarrier))
                    __leave;
                const float range = std::clamp(
                    g_config.crosshair_distance_m, 2.0f, 50.0f) *
                    Game_GetWorldScale();
                float direction[3]{};
                if (!Halo2BuildControllerShotDirection(
                        publication.stock.position, rightCarrier, range,
                        direction))
                    __leave;

                const uintptr_t objectsData = module
                    ? *reinterpret_cast<const volatile uintptr_t*>(
                          module + kHalo2ObjectsDataArrayPointerRva)
                    : 0;
                const uintptr_t objectStorage = objectsData
                    ? *reinterpret_cast<const volatile uintptr_t*>(
                          objectsData + kHalo2DataArrayStorageOffset)
                    : 0;
                const auto objectAccessor =
                    reinterpret_cast<Halo2ObjectDatumAccessorFn>(
                        g_objectDatumAccessor.load(std::memory_order_acquire));
                if (!objectsData || !objectStorage || !objectAccessor)
                    __leave;
                const uintptr_t entry = objectsData + objectStorage +
                    static_cast<uintptr_t>(objectIndex & 0xffffu) *
                        kHalo2ObjectDataEntryStride;
                auto* const unit = static_cast<uint8_t*>(
                    objectAccessor(reinterpret_cast<const void*>(entry)));
                if (!unit)
                    __leave;
                std::memcpy(
                    unit + kHalo2UnitDesiredAimingVectorOffset,
                    direction, sizeof(direction));
                std::memcpy(
                    unit + kHalo2UnitAimingVectorOffset,
                    direction, sizeof(direction));
                applied = true;
                result |= 1u;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_nativeAimRefused.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (applied)
            g_nativeAimApplied.fetch_add(1, std::memory_order_relaxed);
        g_nativeAimActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return result;
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
        if (Halo2ShouldAttemptDirectShotOwnership(
                originalCompleted, origin && direction,
                Game_Halo2ControllerAimActive(),
                Halo2Observer6Dof_DirectWeaponAimArmed(), useUnitAim))
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

                // The bullet must meet the crosshair the player can actually
                // see. The compositor may stabilize that displayed pose, so
                // using a newer raw VR_GetAimPose here creates two different
                // rays whenever aim_stabilization is non-zero. Rebuild the
                // carrier from the most recently PRESENTED reticle pose and
                // converge the engine-owned muzzle origin on that exact ray.
                // This helper's completed direction is the final firing
                // boundary for both values of `useUnitAim`. Restricting
                // ownership to the true case left 19 C-H2-55 helper calls as
                // unclassified stock before the local-unit guard could even
                // run. The guard below now decides ownership for both values.
                Halo2ObserverPosePublication publication{};
                Halo2CameraBasis carrier{};
                float presentedOrientation[4]{};
                float presentedPosition[3]{};
                uint64_t presentedAtMs = 0;
                const uint64_t nowMs = GetTickCount64();
                if (Halo2Observer6Dof_ReadPublishedPose(publication) &&
                    publication.generation ==
                        g_generation.load(std::memory_order_acquire) &&
                    VR_GetPresentedReticleAimPose(
                        presentedOrientation, presentedPosition,
                        presentedAtMs) &&
                    presentedAtMs <= nowMs && nowMs - presentedAtMs <= 250 &&
                    Halo2BuildStableControllerCarrier(
                        publication.stock, publication.referenceOrientation,
                        publication.referencePosition, presentedOrientation,
                        presentedPosition, Game_GetWorldScale(), 0.0f,
                        carrier))
                {
                    const float range = std::clamp(
                        g_config.crosshair_distance_m, 2.0f, 50.0f) *
                        Game_GetWorldScale();
                    float candidate[3]{};
                    if (Halo2BuildControllerShotDirection(
                            origin, carrier, range, candidate))
                    {
                        float stockLengthSquared = 0.0f;
                        float candidateLengthSquared = 0.0f;
                        float dot = 0.0f;
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            stockLengthSquared += direction[axis] * direction[axis];
                            candidateLengthSquared +=
                                candidate[axis] * candidate[axis];
                            dot += direction[axis] * candidate[axis];
                        }
                        const float denominator = std::sqrt(
                            stockLengthSquared * candidateLengthSquared);
                        if (std::isfinite(denominator) && denominator > 1.0e-6f)
                        {
                            dot = std::clamp(dot / denominator, -1.0f, 1.0f);
                            const float degrees =
                                std::acos(dot) * 57.29577951308232f;
                            if (std::isfinite(degrees))
                            {
                                const uint32_t milliDegrees =
                                    static_cast<uint32_t>(std::min(
                                        degrees * 1000.0f, 180000.0f));
                                if (milliDegrees >= 250)
                                    g_weaponAimChanged.fetch_add(
                                        1, std::memory_order_relaxed);
                                uint32_t previous =
                                    g_weaponAimMaxDeflectionMilliDegrees.load(
                                        std::memory_order_relaxed);
                                while (previous < milliDegrees &&
                                       !g_weaponAimMaxDeflectionMilliDegrees.
                                           compare_exchange_weak(
                                               previous, milliDegrees,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed))
                                {
                                }
                            }
                        }
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

    __declspec(noinline) void __fastcall Halo2ParticleRendererDetour(
        uint32_t arg0, uint32_t currentUserFirstPerson, uint32_t arg2,
        uint32_t arg3)
    {
        g_particleActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
        __try
        {
            const auto original = reinterpret_cast<Halo2ParticleRendererFn>(
                g_particleOriginal.load(std::memory_order_acquire));
            bool suppress = false;
            if (original && g_armed.load(std::memory_order_acquire) &&
                g_levelLive.load(std::memory_order_acquire) &&
                !g_teardownRequested.load(std::memory_order_acquire))
            {
                const uintptr_t base =
                    g_moduleBase.load(std::memory_order_acquire);
                if (base)
                {
                    uint8_t classicDisabled = 1;
                    bool readable = false;
                    __try
                    {
                        classicDisabled = *reinterpret_cast<
                            const volatile uint8_t*>(
                                base + kHalo2ClassicRenderDisabledByteRva);
                        readable = true;
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        g_particleReadFaults.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    suppress = readable &&
                        Halo2ShouldSuppressClassicFirstPersonParticle(
                            classicDisabled,
                            static_cast<uint8_t>(currentUserFirstPerson));
                }
            }

            if (suppress)
            {
                g_particleSuppressed.fetch_add(1, std::memory_order_relaxed);
                g_particleHitPending.store(true, std::memory_order_release);
            }
            else if (original)
            {
                original(arg0, currentUserFirstPerson, arg2, arg3);
            }
        }
        __finally
        {
            g_particleActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel);
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

    bool InstallParticleGate(
        uintptr_t base, size_t size, uint32_t generation) noexcept
    {
        if (g_particleTarget)
            return true;
        if (g_particleRejectedGeneration == generation)
            return false;

        uintptr_t match = 0;
        uint32_t matchCount = 0;
        if (!CountPatternMatches(
                base, size, kHalo2ParticleRendererPattern, match,
                matchCount) ||
            matchCount != 1 || match != base + kHalo2ParticleRendererRva)
        {
            LOG("Halo 2 Classic muzzle suppression StockFallback: particle "
                "renderer signature matched %u times%s; camera and every "
                "other feature remain active",
                matchCount,
                (matchCount == 1 &&
                 match != base + kHalo2ParticleRendererRva)
                    ? " but moved from its pinned RVA" : "");
            g_particleRejectedGeneration = generation;
            return false;
        }

        bool gateReadable = false;
        uint8_t gate = 0xFF;
        __try
        {
            gate = *reinterpret_cast<const volatile uint8_t*>(
                base + kHalo2ClassicRenderDisabledByteRva);
            gateReadable = gate <= 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            gateReadable = false;
        }
        if (!gateReadable)
        {
            LOG("Halo 2 Classic muzzle suppression StockFallback: live "
                "renderer gate +0x%X was unreadable or invalid; camera and "
                "every other feature remain active",
                static_cast<unsigned>(kHalo2ClassicRenderDisabledByteRva));
            g_particleRejectedGeneration = generation;
            return false;
        }

        void* trampoline = nullptr;
        void* const target = reinterpret_cast<void*>(match);
        const MH_STATUS created = MH_CreateHook(
            target, reinterpret_cast<void*>(&Halo2ParticleRendererDetour),
            &trampoline);
        if (created != MH_OK || !trampoline)
        {
            LOG("Halo 2 Classic muzzle suppression StockFallback: hook "
                "create=%d; camera and every other feature remain active",
                static_cast<int>(created));
            if (created == MH_OK)
                (void)MH_RemoveHook(target);
            g_particleRejectedGeneration = generation;
            return false;
        }

        g_particleOriginal.store(
            reinterpret_cast<uintptr_t>(trampoline),
            std::memory_order_release);
        const MH_STATUS enabled = MH_EnableHook(target);
        if (enabled != MH_OK)
        {
            LOG("Halo 2 Classic muzzle suppression StockFallback: hook "
                "enable=%d; camera and every other feature remain active",
                static_cast<int>(enabled));
            (void)MH_RemoveHook(target);
            g_particleOriginal.store(0, std::memory_order_release);
            g_particleRejectedGeneration = generation;
            return false;
        }

        g_particleTarget = target;
        g_particleSuppressed.store(0, std::memory_order_relaxed);
        g_particleReadFaults.store(0, std::memory_order_relaxed);
        g_particleHitPending.store(false, std::memory_order_release);
        g_particleHitLogged = false;
        LOG("Halo 2 Classic muzzle suppression Installed (Stage 3AK): "
            "particle renderer +0x%X is skipped only for nonzero current-user "
            "first-person calls while live renderer gate +0x%X is 0; "
            "Anniversary and all stock/world callers remain stock",
            static_cast<unsigned>(kHalo2ParticleRendererRva),
            static_cast<unsigned>(kHalo2ClassicRenderDisabledByteRva));
        return true;
    }

    bool RemoveParticleGate() noexcept
    {
        if (!g_particleTarget)
        {
            g_particleOriginal.store(0, std::memory_order_release);
            return true;
        }
        const MH_STATUS disabled = MH_DisableHook(g_particleTarget);
        if (disabled != MH_OK && disabled != MH_ERROR_NOT_CREATED &&
            disabled != MH_ERROR_DISABLED)
        {
            LOG("Halo 2 Classic muzzle suppression: disable failed (%d); "
                "ownership retained for cleanup", static_cast<int>(disabled));
            return false;
        }
        for (int attempt = 0; attempt < 200 &&
             g_particleActiveCallbacks.load(std::memory_order_acquire);
             ++attempt)
        {
            Sleep(10);
        }
        if (g_particleActiveCallbacks.load(std::memory_order_acquire))
        {
            LOG("Halo 2 Classic muzzle suppression: callbacks did not drain; "
                "ownership retained for cleanup");
            return false;
        }
        const MH_STATUS removed = MH_RemoveHook(g_particleTarget);
        if (removed != MH_OK && removed != MH_ERROR_NOT_CREATED)
        {
            LOG("Halo 2 Classic muzzle suppression: remove failed (%d); "
                "ownership retained for cleanup", static_cast<int>(removed));
            return false;
        }
        g_particleTarget = nullptr;
        g_particleOriginal.store(0, std::memory_order_release);
        LOG("Halo 2 Classic muzzle suppression removed: %llu first-person "
            "Classic particle calls suppressed; stock renderer restored",
            static_cast<unsigned long long>(
                g_particleSuppressed.load(std::memory_order_relaxed)));
        return true;
    }

    bool RemoveCore(const char* reason) noexcept
    {
        g_armed.store(false, std::memory_order_release);
        g_teardownRequested.store(true, std::memory_order_release);
        // Optional and failure-isolated: restore the stock engine boolean even
        // if a later hook cleanup needs to remain pending.
        if constexpr (kHalo2DebugGlobalAimAssistOverrideEnabled)
            Game_Halo2RestoreAimAssist();
        if (!RemoveParticleGate())
            return false;
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
        g_finalPaletteReady.store(false, std::memory_order_release);
        if (g_interpFrameTarget)
        {
            (void)MH_DisableHook(g_interpFrameTarget);
            while (g_interpFrameActiveCallbacks.load(std::memory_order_acquire))
                Sleep(1);
            (void)MH_RemoveHook(g_interpFrameTarget);
            g_interpFrameTarget = nullptr;
            g_interpFrameOriginal.store(0, std::memory_order_release);
        }
        if (g_packetBuilderTarget)
        {
            const MH_STATUS disabled = MH_DisableHook(g_packetBuilderTarget);
            if (disabled != MH_OK && disabled != MH_ERROR_NOT_CREATED &&
                disabled != MH_ERROR_DISABLED)
            {
                LOG("Halo 2 final-packet hands: disable failed (%d); ownership "
                    "retained for cleanup", static_cast<int>(disabled));
                return false;
            }
            for (int attempt = 0; attempt < 200 &&
                 g_packetBuilderActiveCallbacks.load(std::memory_order_acquire);
                 ++attempt)
            {
                Sleep(10);
            }
            if (g_packetBuilderActiveCallbacks.load(std::memory_order_acquire))
            {
                LOG("Halo 2 final-packet callbacks did not drain; ownership "
                    "retained for cleanup");
                return false;
            }
            (void)MH_RemoveHook(g_packetBuilderTarget);
            g_packetBuilderTarget = nullptr;
            g_packetBuilderOriginal.store(0, std::memory_order_release);
            g_packetBuilderLastAppliedMs.store(0, std::memory_order_release);
        }
        if (g_visibleConsumerTarget)
        {
            const MH_STATUS disabled = MH_DisableHook(g_visibleConsumerTarget);
            if (disabled != MH_OK && disabled != MH_ERROR_NOT_CREATED &&
                disabled != MH_ERROR_DISABLED)
            {
                LOG("Halo 2 visible consumer: disable failed (%d); ownership "
                    "retained for cleanup", static_cast<int>(disabled));
                return false;
            }
            for (int attempt = 0; attempt < 200 &&
                 g_visibleConsumerActiveCallbacks.load(
                     std::memory_order_acquire); ++attempt)
                Sleep(10);
            if (g_visibleConsumerActiveCallbacks.load(std::memory_order_acquire))
            {
                LOG("Halo 2 visible-consumer callbacks did not drain; "
                    "ownership retained for cleanup");
                return false;
            }
            (void)MH_RemoveHook(g_visibleConsumerTarget);
            g_visibleConsumerTarget = nullptr;
            g_visibleConsumerOriginal.store(0, std::memory_order_release);
            g_visibleConsumerContext = Halo2VisibleConsumerContext{};
        }
        if (g_finalPaletteTarget)
        {
            const MH_STATUS disabled = MH_DisableHook(g_finalPaletteTarget);
            if (disabled != MH_OK && disabled != MH_ERROR_NOT_CREATED &&
                disabled != MH_ERROR_DISABLED)
            {
                LOG("Halo 2 final-palette hands: disable failed (%d); "
                    "ownership retained for cleanup", static_cast<int>(disabled));
                return false;
            }
            for (int attempt = 0; attempt < 200 &&
                 g_finalPaletteActiveCallbacks.load(std::memory_order_acquire);
                 ++attempt)
            {
                Sleep(10);
            }
            if (g_finalPaletteActiveCallbacks.load(std::memory_order_acquire))
            {
                LOG("Halo 2 final-palette callbacks did not drain; ownership "
                    "retained for cleanup");
                return false;
            }
            (void)MH_RemoveHook(g_finalPaletteTarget);
            g_finalPaletteTarget = nullptr;
            g_finalPaletteOriginal.store(0, std::memory_order_release);
            g_finalPaletteContext = Halo2FinalPaletteContext{};
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
        if (g_nativeAimTarget)
        {
            const MH_STATUS disabled = MH_DisableHook(g_nativeAimTarget);
            if (disabled != MH_OK && disabled != MH_ERROR_NOT_CREATED &&
                disabled != MH_ERROR_DISABLED)
            {
                LOG("Halo 2 native controller aim: disable failed (%d); "
                    "ownership retained for cleanup", static_cast<int>(disabled));
                return false;
            }
            for (int attempt = 0; attempt < 200 &&
                 g_nativeAimActiveCallbacks.load(std::memory_order_acquire);
                 ++attempt)
                Sleep(10);
            if (g_nativeAimActiveCallbacks.load(std::memory_order_acquire))
            {
                LOG("Halo 2 native controller-aim callbacks did not drain; "
                    "ownership retained for cleanup");
                return false;
            }
            (void)MH_RemoveHook(g_nativeAimTarget);
            g_nativeAimTarget = nullptr;
            g_nativeAimOriginal.store(0, std::memory_order_release);
            g_objectDatumAccessor.store(0, std::memory_order_release);
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
        // Non-owning identity only. Holding a loader reference here prevents
        // MCC from unloading halo2.dll between levels and makes the eventual
        // release race the next load.
        g_moduleReference = nullptr;
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
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
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
            g_rejectedGeneration = generation;
            return false;
        }

        if (!observerResultArray ||
            observerResultArray != base + kHalo2ObserverResultArrayRva)
        {
            LOG("Halo 2 observer 6DOF WITHHELD: the observer result array was "
                "not signature-proven for this generation; the stock camera "
                "is untouched");
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
                                if (kHalo2RejectedInterpolatorControllerOwnershipEnabled)
                                {
                                    LOG("Halo 2 floating hands ARMED "
                                        "(C-H2-48): first-person slot 0 only "
                                        "- exact left/right wrist subtrees "
                                        "ride their controllers; upper-arm and "
                                        "forearm nodes collapse, and the gun remains "
                                        "inside the right-hand subtree "
                                        "carrier (gun_forward_m %.3f m, "
                                        "gun_scale %.2f, mount trim "
                                        "%.1f/%.1f/%.1f deg). floating_hands "
                                        "in halomccvr.cfg is %d and is NOT "
                                        "consulted, as in Reach. No visible "
                                        "arms/body, no "
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

        // E-H2-41 (C-H2-47/48): resolve the engine's own animation-graph readers so
        // the LEFT hand can be identified from Halo 2's own data instead of a
        // hardcoded node index. These are read-only leaf calls; nothing here is
        // hooked or patched. All three must match exactly once and sit at their
        // pinned RVAs, or the complete optional floating-hand presentation stays
        // stock for that frame.
        if (kHalo2VisibleConsumerControllerOwnershipEnabled)
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
                LOG("Halo 2 hand binding readers ARMED (C-H2-60): the left and right "
                    "wrist descendants are identified from the "
                    "engine's own animation-graph model flags (node +0x%X, "
                    "left-hand bit 0x%02X, right-hand bit 0x%02X, stride "
                    "0x%X) - no hardcoded node index. Graph reader +0x%X, node "
                    "reader +0x%X, engine flag lookup %s. The final packet "
                    "transaction maps those source flags through H2's authored "
                    "hands remap; any failure leaves the complete optional "
                    "presentation stock",
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
                LOG("Halo 2 final-packet hand binding WITHHELD: animation-graph "
                    "reader signatures matched %u / %u / %u times; hands, gun "
                    "and firing stay stock",
                    graphGetCount, nodeGetCount, findCount);
            }
        }

        // E-H2-53: +0x8181F0 calls the registered +0x6BB40 consumer before it
        // returns. The outer detour only establishes exact same-thread identity
        // and controller context; the consumer detour owns the matrices before
        // the renderer copies them. There is no post-return ownership path.
        g_finalPaletteReady.store(false, std::memory_order_release);
        uintptr_t packetMatch = 0;
        uint32_t packetMatchCount = 0;
        uintptr_t consumerMatch = 0;
        uint32_t consumerMatchCount = 0;
        constexpr char kPacketBuilderPattern[] =
            "48 89 5C 24 18 4C 89 4C 24 20 89 54 24 10 89 4C 24 08 "
            "55 56 57 41 54 41 55 41 56 41 57";
        constexpr char kVisibleConsumerPattern[] =
            "48 8B C4 89 48 08 55 41 54 41 55 41 56 41 57 48 8D A8 "
            "48 FE FF FF 48 81 EC 90 02 00 00";
        if (!kHalo2VisibleConsumerControllerOwnershipEnabled)
        {
            LOG("Halo 2 visible-consumer hands NOT installed: build switch off");
        }
        else if (!CountPatternMatches(
                     base, size, kPacketBuilderPattern, packetMatch,
                     packetMatchCount) ||
                 packetMatchCount != 1 ||
                 packetMatch != base + kHalo2FirstPersonPacketBuilderRva ||
                 !CountPatternMatches(
                     base, size, kVisibleConsumerPattern, consumerMatch,
                     consumerMatchCount) ||
                 consumerMatchCount != 1 ||
                 consumerMatch !=
                     base + kHalo2FirstPersonVisibleConsumerRva)
        {
            LOG("Halo 2 visible-consumer hands WITHHELD: builder / consumer "
                "identities matched %u / %u times",
                packetMatchCount, consumerMatchCount);
        }
        else
        {
            void* packetTrampoline = nullptr;
            void* consumerTrampoline = nullptr;
            void* const packetTarget = reinterpret_cast<void*>(packetMatch);
            void* const consumerTarget =
                reinterpret_cast<void*>(consumerMatch);
            const MH_STATUS consumerCreated = MH_CreateHook(
                consumerTarget,
                reinterpret_cast<void*>(
                    &Halo2VisibleFirstPersonConsumerDetour),
                &consumerTrampoline);
            const MH_STATUS packetCreated = MH_CreateHook(
                packetTarget,
                reinterpret_cast<void*>(&Halo2FirstPersonPacketBuilderDetour),
                &packetTrampoline);
            if (consumerCreated == MH_OK && consumerTrampoline)
                g_visibleConsumerOriginal.store(
                    reinterpret_cast<uintptr_t>(consumerTrampoline),
                    std::memory_order_release);
            if (packetCreated == MH_OK && packetTrampoline)
                g_packetBuilderOriginal.store(
                    reinterpret_cast<uintptr_t>(packetTrampoline),
                    std::memory_order_release);
            const MH_STATUS consumerEnabled =
                consumerCreated == MH_OK && consumerTrampoline
                ? MH_EnableHook(consumerTarget)
                : MH_ERROR_NOT_CREATED;
            const MH_STATUS packetEnabled =
                packetCreated == MH_OK && packetTrampoline
                ? MH_EnableHook(packetTarget)
                : MH_ERROR_NOT_CREATED;
            if (consumerCreated == MH_OK && consumerTrampoline &&
                packetCreated == MH_OK && packetTrampoline &&
                consumerEnabled == MH_OK && packetEnabled == MH_OK)
            {
                g_visibleConsumerTarget = consumerTarget;
                g_packetBuilderTarget = packetTarget;
                g_finalPaletteReady.store(true, std::memory_order_release);
                // E-H2-71 (C-H2-83): the Classic-only interpolated frame.
                // Optional and fail-open: without it the hands/gun keep
                // C-H2-81 behaviour; nothing else is gated on it.
                {
                    uintptr_t frameMatch = 0;
                    uint32_t frameMatchCount = 0;
                    // 28 bytes: the 23-byte prologue is shared with the
                    // sibling getter at +0x7227A0 (that is what made
                    // C-H2-83 match twice and refuse to install).
                    constexpr char kInterpFramePattern[] =
                        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 "
                        "48 83 EC 20 48 83 3D D4 8B F2 00 00";
                    if (!CountPatternMatches(
                            base, size, kInterpFramePattern, frameMatch,
                            frameMatchCount) ||
                        frameMatchCount != 1 ||
                        frameMatch !=
                            base + kHalo2FrameInterpolatorFirstPersonFrameRva)
                    {
                        LOG("Halo 2 classic composition frame NOT owned: the "
                            "interpolated first-person frame getter matched "
                            "%u times (expected 1 at +0x%X); Classic keeps "
                            "the engine's interpolated frame",
                            frameMatchCount,
                            static_cast<unsigned>(
                                kHalo2FrameInterpolatorFirstPersonFrameRva));
                    }
                    else
                    {
                        void* frameTrampoline = nullptr;
                        void* const frameTarget =
                            reinterpret_cast<void*>(frameMatch);
                        const MH_STATUS frameCreated = MH_CreateHook(
                            frameTarget,
                            reinterpret_cast<void*>(
                                &Halo2InterpolatedFrameDetour),
                            &frameTrampoline);
                        if (frameCreated == MH_OK && frameTrampoline)
                        {
                            g_interpFrameOriginal.store(
                                reinterpret_cast<uintptr_t>(frameTrampoline),
                                std::memory_order_release);
                            if (MH_EnableHook(frameTarget) == MH_OK)
                            {
                                g_interpFrameTarget = frameTarget;
                                LOG("Halo 2 classic composition frame OWNED "
                                    "(C-H2-83, E-H2-71): +0x%X reports no "
                                    "interpolated first-person frame during "
                                    "the mod's Classic packet build, so the "
                                    "builder composes the weapon against "
                                    "user_data+0x%X - the exact frame "
                                    "Anniversary composes against. Hands and "
                                    "gun therefore carry the same orientation "
                                    "in both renderers",
                                    static_cast<unsigned>(
                                        kHalo2FrameInterpolatorFirstPersonFrameRva),
                                    static_cast<unsigned>(
                                        kHalo2FirstPersonCurrentFrameOffset));
                            }
                            else
                            {
                                (void)MH_RemoveHook(frameTarget);
                                g_interpFrameOriginal.store(
                                    0, std::memory_order_release);
                                LOG("Halo 2 classic composition frame NOT "
                                    "owned: enable failed");
                            }
                        }
                        else
                        {
                            LOG("Halo 2 classic composition frame NOT owned: "
                                "create failed %d", static_cast<int>(frameCreated));
                        }
                    }
                }
                LOG("Halo 2 renderer-selected hands ARMED (C-H2-65): builder "
                     "+0x%X establishes exact packet context; Anniversary "
                     "consumer +0x%X owns matrices before copy, while Classic "
                     "owns its returned persistent packets at each eye draw; "
                     "rejected generic C-H2-64 alignment is disabled and the "
                     "accepted C-H2-63 presentation is restored",
                    static_cast<unsigned>(kHalo2FirstPersonPacketBuilderRva),
                    static_cast<unsigned>(
                        kHalo2FirstPersonVisibleConsumerRva));
            }
            else
            {
                LOG("Halo 2 visible-consumer hands WITHHELD: callback create/"
                    "enable %d/%d, builder create/enable %d/%d",
                    static_cast<int>(consumerCreated),
                    static_cast<int>(consumerEnabled),
                    static_cast<int>(packetCreated),
                    static_cast<int>(packetEnabled));
                if (packetCreated == MH_OK)
                {
                    (void)MH_DisableHook(packetTarget);
                    (void)MH_RemoveHook(packetTarget);
                }
                if (consumerCreated == MH_OK)
                {
                    (void)MH_DisableHook(consumerTarget);
                    (void)MH_RemoveHook(consumerTarget);
                }
                g_packetBuilderOriginal.store(0, std::memory_order_release);
                g_visibleConsumerOriginal.store(0, std::memory_order_release);
            }
        }

        // E-H2-54: own Halo 2's native desired/current aiming vectors at their
        // updater, rather than rewriting one later firing-helper result. This
        // single engine aim reaches crosshair, weapon simulation and projectile
        // construction together, using the same publication as the mesh.
        uintptr_t aimMatch = 0;
        uint32_t aimMatchCount = 0;
        uintptr_t accessorMatch = 0;
        uint32_t accessorMatchCount = 0;
        constexpr char kNativeAimPattern[] =
            "40 55 53 56 57 41 54 48 8D 6C 24 D0 48 81 EC 30 01 00 00 "
            "48 8B 05 ?? ?? ?? ?? 33 FF 44 8B E1";
        constexpr char kObjectAccessorPattern[] =
            "8B 51 08 48 8B 0D ?? ?? ?? ?? E9 ?? ?? ?? ??";
        if (!kHalo2VisibleConsumerControllerOwnershipEnabled ||
            !g_finalPaletteReady.load(std::memory_order_acquire))
        {
            LOG("Halo 2 native controller aim NOT installed: visible consumer "
                "ownership is unavailable");
        }
        else if (!CountPatternMatches(
                base, size, kNativeAimPattern, aimMatch, aimMatchCount) ||
            aimMatchCount != 1 ||
            aimMatch != base + kHalo2NativeAimUpdateRva ||
            !CountPatternMatches(
                base, size, kObjectAccessorPattern, accessorMatch,
                accessorMatchCount) ||
            accessorMatchCount != 1 ||
            accessorMatch != base + kHalo2ObjectDatumAccessorRva)
        {
            LOG("Halo 2 native controller aim WITHHELD: updater / object "
                "accessor identities matched %u / %u times",
                aimMatchCount, accessorMatchCount);
        }
        else
        {
            void* trampoline = nullptr;
            void* const aimTarget = reinterpret_cast<void*>(aimMatch);
            const MH_STATUS created = MH_CreateHook(
                aimTarget, reinterpret_cast<void*>(&Halo2NativeAimUpdateDetour),
                &trampoline);
            if (created != MH_OK || !trampoline)
            {
                LOG("Halo 2 native controller aim WITHHELD: hook create=%d",
                    static_cast<int>(created));
                if (created == MH_OK)
                    (void)MH_RemoveHook(aimTarget);
            }
            else
            {
                g_nativeAimTarget = aimTarget;
                g_nativeAimOriginal.store(
                    reinterpret_cast<uintptr_t>(trampoline),
                    std::memory_order_release);
                g_objectDatumAccessor.store(
                    accessorMatch, std::memory_order_release);
                const MH_STATUS enabled = MH_EnableHook(aimTarget);
                if (enabled != MH_OK)
                {
                    LOG("Halo 2 native controller aim WITHHELD: hook enable=%d",
                        static_cast<int>(enabled));
                    (void)MH_RemoveHook(aimTarget);
                    g_nativeAimTarget = nullptr;
                    g_nativeAimOriginal.store(0, std::memory_order_release);
                    g_objectDatumAccessor.store(0, std::memory_order_release);
                }
                else
                {
                    LOG("Halo 2 native controller aim installed (C-H2-62): "
                        "unit aim updater +0x%X writes local user 0 desired/"
                        "current aim +0x%X/+0x%X from the same stable "
                        "controller sight line as the visible mesh; the old "
                        "firing-helper substitution is not installed",
                        static_cast<unsigned>(kHalo2NativeAimUpdateRva),
                        static_cast<unsigned>(
                            kHalo2UnitDesiredAimingVectorOffset),
                        static_cast<unsigned>(kHalo2UnitAimingVectorOffset));
                }
            }
        }

        // Optional feature transaction: refusal here leaves the proven camera,
        // stereo, input and hand paths installed and loudly retains stock
        // particles for this generation.
        (void)InstallParticleGate(base, size, generation);

        // C-H2-89 is deliberately a separate optional transaction. A missing
        // or invalid debug-global slot leaves stock aim assist active without
        // disarming any of the proven Halo 2 VR paths.
        if constexpr (kHalo2DebugGlobalAimAssistOverrideEnabled)
            (void)Game_Halo2TryDisableAimAssist(base, size);

        g_coreState = CoreState::Installed;
        LOG("Halo 2 observer 6DOF installed: observer final transform "
            "+0x%X, observer results +0x%X stride 0x%X, user %u. Three "
            "12-byte spans (position +0x%X, forward +0x%X, up +0x%X) are "
            "written after the engine's own transform; C-H2-71 may additionally "
            "expand only vertical FOV +0x%X to the headset visibility cover "
            "(never narrows it); aspect and every other observer field stay "
            "engine-owned",
            static_cast<unsigned>(kHalo2ObserverFinalTransformRva),
            static_cast<unsigned>(kHalo2ObserverResultArrayRva),
            static_cast<unsigned>(kHalo2ObserverStride), kOwnedUser,
            static_cast<unsigned>(kHalo2ObserverResultPositionOffset),
            static_cast<unsigned>(kHalo2ObserverResultForwardOffset),
            static_cast<unsigned>(kHalo2ObserverResultUpOffset),
            static_cast<unsigned>(kHalo2ObserverResultVerticalFovOffset));
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
            "%llu on its own controller, %llu stock fallback, %llu binding "
            "rebuilds, last binding reason %d; final packet: %llu builder calls, "
            "%llu owned, %llu stock, %llu gun nodes; packet gates: %llu eligible, "
            "%llu weapon-state miss, %llu publication miss, %llu controller "
            "snapshot miss, %llu carrier miss, %llu ownership miss; rig packets: "
            "Chief %llu contexts/%llu owned flags 0x%02X, Arbiter %llu contexts/"
            "%llu owned flags 0x%02X; wrist motion "
            "right %u/%u mm last/max, left %u/%u mm last/max; palette result: "
            "%llu changed, %llu right, %llu left, %llu collapsed (%llu Elite "
            "arm records co-located), %llu refused; "
            "visible consumer: %llu calls, %llu hands owned, %llu guns owned; "
            "barrel meter [classic] carrier vs compose %.2f deg (elev %+.2f), "
            "stock root+X vs compose %.2f deg (elev %+.2f); [anniv] carrier "
            "vs compose %.2f deg (elev %+.2f), stock root+X vs compose %.2f "
            "deg (elev %+.2f); "
            "classic trim %llu applied / %llu refused; "
            "classic composition frame forced to current %llu of %llu "
            "getter calls; classic packet: %llu calls, %llu owned; classic eyes: %llu calls, "
            "%llu compensated, %llu no-packet, %llu no-pass, %llu refused; "
            "native aim: %llu calls, %llu applied, %llu non-owned, %llu refused",
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
                g_packetBuilderCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderStock.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderGunNodes.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderEligible.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderWeaponStateMiss.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderPublicationMiss.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderControllerSnapshotMiss.load(
                    std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderCarrierMiss.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderOwnershipMiss.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderChiefContexts.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderChiefApplied.load(std::memory_order_relaxed)),
            g_packetBuilderLastChiefFlags.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(
                g_packetBuilderEliteContexts.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_packetBuilderEliteApplied.load(std::memory_order_relaxed)),
            g_packetBuilderLastEliteFlags.load(std::memory_order_relaxed),
            g_packetBuilderRightDeltaMillimeters.load(
                std::memory_order_relaxed),
            g_packetBuilderMaxRightDeltaMillimeters.load(
                std::memory_order_relaxed),
            g_packetBuilderLeftDeltaMillimeters.load(
                std::memory_order_relaxed),
            g_packetBuilderMaxLeftDeltaMillimeters.load(
                std::memory_order_relaxed),
            static_cast<unsigned long long>(
                g_finalPaletteCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_finalPaletteMovedRight.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_finalPaletteMovedLeft.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_finalPaletteCollapsed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_finalPaletteCoLocatedArms.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_finalPaletteRefused.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_visibleConsumerCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_visibleConsumerHandsApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_visibleConsumerGunsApplied.load(std::memory_order_relaxed)),
            g_meterCarrierVsCompose[0].load(std::memory_order_relaxed) / 1000.0,
            g_meterCarrierElevation[0].load(std::memory_order_relaxed) / 1000.0,
            g_meterStockVsCompose[0].load(std::memory_order_relaxed) / 1000.0,
            g_meterStockElevation[0].load(std::memory_order_relaxed) / 1000.0,
            g_meterCarrierVsCompose[1].load(std::memory_order_relaxed) / 1000.0,
            g_meterCarrierElevation[1].load(std::memory_order_relaxed) / 1000.0,
            g_meterStockVsCompose[1].load(std::memory_order_relaxed) / 1000.0,
            g_meterStockElevation[1].load(std::memory_order_relaxed) / 1000.0,
            static_cast<unsigned long long>(
                g_classicTrimApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_classicTrimRefused.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_interpFrameForcedCurrent.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_interpFrameCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_classicPacketCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_classicPacketApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_classicEyeCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_classicEyeCompensated.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_classicEyeNoPacket.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_classicEyeNoPass.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_classicEyeRefused.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_nativeAimCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_nativeAimApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_nativeAimNonOwned.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_nativeAimRefused.load(std::memory_order_relaxed)));
        uint32_t coverBits[3] = {
            g_visibilityCoverStockBits.load(std::memory_order_relaxed),
            g_visibilityCoverRequiredBits.load(std::memory_order_relaxed),
            g_visibilityCoverWrittenBits.load(std::memory_order_relaxed)};
        float coverRadians[3]{};
        std::memcpy(coverRadians, coverBits, sizeof(coverRadians));
        constexpr float kDegrees = 57.29577951308232f;
        LOG("Halo 2 C-H2-71 upstream visibility cover: %llu expanded, %llu "
            "already wide, %llu refused; last stock %.1f deg, headset %.1f "
            "deg, selected %.1f deg at observer_result+0x%X (pose/stereo stay "
            "owned if this optional feature refuses)",
            static_cast<unsigned long long>(
                g_visibilityCoverApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_visibilityCoverAlreadyWide.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_visibilityCoverRefused.load(std::memory_order_relaxed)),
            coverRadians[0] * kDegrees, coverRadians[1] * kDegrees,
            coverRadians[2] * kDegrees,
            static_cast<unsigned>(kHalo2ObserverResultVerticalFovOffset));
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
    // Map/script initialization can rewrite debug globals after core install.
    // This poll runs on the title worker, not an eye/render hook.
    if constexpr (kHalo2DebugGlobalAimAssistOverrideEnabled)
        Game_Halo2MaintainDisabledAimAssist();
    if (g_armedLoggedGeneration != generation)
    {
        g_armedLoggedGeneration = generation;
        LOG("Halo 2 observer 6DOF armed: the headset now owns the Halo 2 "
            "camera's position and orientation in BOTH graphics modes, "
            "because the observer is the one camera root the classic Blam "
            "renderer and the remastered Anniversary renderer both consume");
    }
    if (!g_particleHitLogged &&
        g_particleHitPending.exchange(false, std::memory_order_acq_rel))
    {
        g_particleHitLogged = true;
        LOG("Halo 2 Classic muzzle suppression HIT: first-person Classic "
            "particle renderer calls are being skipped; Anniversary remains "
            "stock");
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
    return g_nativeAimOriginal.load(std::memory_order_acquire) != 0 &&
        g_objectDatumAccessor.load(std::memory_order_acquire) != 0 &&
        g_armed.load(std::memory_order_acquire) &&
        g_levelLive.load(std::memory_order_acquire) &&
        !g_teardownRequested.load(std::memory_order_acquire);
}

bool Halo2Observer6Dof_FinalPaletteArmed() noexcept
{
    const uint64_t appliedAt =
        g_packetBuilderLastAppliedMs.load(std::memory_order_acquire);
    const uint64_t now = GetTickCount64();
    return g_finalPaletteReady.load(std::memory_order_acquire) &&
        g_packetBuilderOriginal.load(std::memory_order_acquire) != 0 &&
        g_visibleConsumerOriginal.load(std::memory_order_acquire) != 0 &&
        appliedAt && appliedAt <= now && now - appliedAt <= 250 &&
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

// E-H2-67 (C-H2-79): the camera the classic weapon pass was MEASURED to
// hold at draw time, replacing whichever camera the core predicted. Only
// the viewing term changes; the frame/correct cameras the owning core
// named stand.
bool Halo2Observer6Dof_ReadPacketBuildCamera(
    bool anniversary, Halo2CameraBasis& out) noexcept
{
    const int slot = anniversary ? 1 : 0;
    if (!g_packetCameraValid[slot].load(std::memory_order_acquire))
        return false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t before =
            g_packetCameraVersion[slot].load(std::memory_order_acquire);
        if (before & 1u) continue;
        const Halo2CameraBasis candidate = g_packetCamera[slot];
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_packetCameraVersion[slot].load(std::memory_order_acquire) ==
            before)
        {
            if (!Halo2ValidateCameraBasis(candidate))
                return false;
            out = candidate;
            return true;
        }
    }
    return false;
}

void Halo2Observer6Dof_SetMeasuredFirstPersonViewingCamera(
    const Halo2CameraBasis& viewing, bool admitCompensation) noexcept
{
    if (!g_passCamerasSet.load(std::memory_order_acquire) ||
        !Halo2ValidateCameraBasis(viewing))
        return;
    // The owning core decides admission (E-H2-69). A measurement can only
    // name the camera; it can never turn on a transform the core refused.
    g_passCamerasVersion.fetch_add(1, std::memory_order_acq_rel);
    g_passCameras.viewing = viewing;
    g_passCameras.compensate = admitCompensation && g_passCameras.frameValid;
    g_passCamerasVersion.fetch_add(1, std::memory_order_acq_rel);
}

bool Halo2Observer6Dof_BeginClassicFirstPersonEye() noexcept
{
    g_classicEyeCalls.fetch_add(1, std::memory_order_relaxed);
    auto& classic = g_classicPacketContext;
    const uint64_t now = GetTickCount64();
    if (!g_armed.load(std::memory_order_acquire) ||
        !g_levelLive.load(std::memory_order_acquire) ||
        g_teardownRequested.load(std::memory_order_acquire) ||
        !classic.valid || classic.eyeActive || !classic.hands || !classic.gun ||
        classic.handsCount == 0 ||
        classic.handsCount > kHalo2FirstPersonPaletteCapacity ||
        classic.gunCount == 0 ||
        classic.gunCount > kHalo2FirstPersonPaletteCapacity ||
        !classic.appliedAtMs || classic.appliedAtMs > now ||
        now - classic.appliedAtMs > 250)
    {
        g_classicEyeNoPacket.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    Halo2FirstPersonPassCameras pass{};
    bool passValid = false;
    if (g_passCamerasSet.load(std::memory_order_acquire))
    {
        for (int attempt = 0; attempt < 4 && !passValid; ++attempt)
        {
            const uint32_t before =
                g_passCamerasVersion.load(std::memory_order_acquire);
            if (before & 1u) continue;
            const Halo2FirstPersonPassCameras candidate = g_passCameras;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_passCamerasVersion.load(std::memory_order_acquire) == before)
            {
                pass = candidate;
                passValid = candidate.frameValid && candidate.compensate &&
                    Halo2ValidateCameraBasis(candidate.correct) &&
                    Halo2ValidateCameraBasis(candidate.viewing);
            }
        }
    }
    if (!passValid)
    {
        g_classicEyeNoPass.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool compensated = false;
    bool backupsCaptured = false;
    __try
    {
        const size_t handsBytes =
            static_cast<size_t>(classic.handsCount) *
            kHalo2FirstPersonNodeStride;
        const size_t gunBytes =
            static_cast<size_t>(classic.gunCount) *
            kHalo2FirstPersonNodeStride;
        std::memcpy(classic.handsBackup, classic.hands, handsBytes);
        std::memcpy(classic.gunBackup, classic.gun, gunBytes);
        backupsCaptured = true;
        compensated = Halo2CompensateClassicFirstPersonEye(
                classic.hands, classic.handsCount, pass) &&
            Halo2CompensateClassicFirstPersonEye(
                classic.gun, classic.gunCount, pass);
        if (!compensated)
        {
            std::memcpy(classic.hands, classic.handsBackup, handsBytes);
            std::memcpy(classic.gun, classic.gunBackup, gunBytes);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        compensated = false;
        if (backupsCaptured)
        {
            __try
            {
                std::memcpy(
                    classic.hands, classic.handsBackup,
                    static_cast<size_t>(classic.handsCount) *
                        kHalo2FirstPersonNodeStride);
                std::memcpy(
                    classic.gun, classic.gunBackup,
                    static_cast<size_t>(classic.gunCount) *
                        kHalo2FirstPersonNodeStride);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        classic.valid = false;
    }
    if (!compensated)
    {
        g_classicEyeRefused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    classic.eyeActive = true;
    g_classicEyeCompensated.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Halo2Observer6Dof_EndClassicFirstPersonEye() noexcept
{
    auto& classic = g_classicPacketContext;
    if (!classic.eyeActive) return;
    __try
    {
        std::memcpy(
            classic.hands, classic.handsBackup,
            static_cast<size_t>(classic.handsCount) *
                kHalo2FirstPersonNodeStride);
        std::memcpy(
            classic.gun, classic.gunBackup,
            static_cast<size_t>(classic.gunCount) *
                kHalo2FirstPersonNodeStride);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        classic.valid = false;
        g_classicEyeRefused.fetch_add(1, std::memory_order_relaxed);
    }
    classic.eyeActive = false;
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
bool Halo2Observer6Dof_FinalPaletteArmed() noexcept { return false; }
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
void Halo2Observer6Dof_SetMeasuredFirstPersonViewingCamera(
    const Halo2CameraBasis&, bool) noexcept {}
bool Halo2Observer6Dof_ReadPacketBuildCamera(
    bool, Halo2CameraBasis&) noexcept { return false; }
bool Halo2Observer6Dof_BeginClassicFirstPersonEye() noexcept { return false; }
void Halo2Observer6Dof_EndClassicFirstPersonEye() noexcept {}
void Halo2Observer6Dof_RequestRecenter() noexcept {}
void Halo2Observer6Dof_ShutdownForVrFailure() noexcept {}

#endif
