#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <windows.h>

#include "pose_inertia_filter.h"
#include "config.h"
#include "frame_pacing_logic.h"
#include "hud_layout_logic.h"
#include "input_logic.h"
#include "odst_bringup_logic.h"
#include "reach_adapter.h"
#include "reach_chud_logic.h"
#include "reach_observer_logic.h"
#include "reach_render_candidate.h"
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
#include "reach_render_preflight.h"
#endif
#include "reach_render_logic.h"
#include "scope_logic.h"
#include "title_registry.h"
#include "title_runtime_state.h"

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++g_failures;
        }
    }

    ReachRenderCandidateProof CompleteReachRenderCandidateProof()
    {
        ReachRenderCandidateProof proof{};
        proof.retailIdentity = true;
        proof.mainRenderViewMatchCount = 1;
        proof.mainRenderViewAtExpectedRva = true;
        proof.mainRenderViewBodyHash = true;
        proof.playerViewRenderMatchCount = 1;
        proof.playerViewRenderAtExpectedRva = true;
        proof.playerViewRenderBodyHash = true;
        proof.frustumHelperMatchCount = 1;
        proof.frustumHelperAtExpectedRva = true;
        proof.frustumHelperExecutableRange = true;
        proof.fpCameraRebuildMatchCount = 1;
        proof.fpCameraRebuildAtExpectedRva = true;
        proof.fpCameraRebuildBodyHash = true;
        proof.fpCameraUploadMatchCount = 1;
        proof.fpCameraUploadAtExpectedRva = true;
        proof.fpCameraUploadBodyHash = true;
        proof.fpCameraWrapperBodyHashes = true;
        proof.exactFpCameraFlowEdges = true;
        proof.exactOuterCallerEdges = true;
        proof.exactInnerCallerEdge = true;
        proof.fixedDataRanges = true;
        return proof;
    }

    ReachDisplaySurfaceProof CompleteReachDisplaySurfaceProof(
        const ReachModuleEpoch& epoch,
        const ReachPreflightToken& preflight,
        uint64_t resourceRevision = 1)
    {
        ReachDisplaySurfaceProof proof{};
        proof.continuity.epoch = epoch;
        proof.continuity.resourceRevision = resourceRevision;
        proof.continuity.lifecycleSerial = 7;
        proof.continuity.swapchainIdentity = 0x20000000;
        proof.continuity.buffer0Identity = 0x20001000;
        proof.continuity.surfaceArrayIdentity = 0x20002000;
        proof.continuity.record0RtvIdentity = 0x20003000;
        proof.continuity.record0SrvIdentity = 0x20004000;
        proof.continuity.selectedRtvIdentity = 0x20003000;
        proof.continuity.deviceIdentity = 0x20008000;
        proof.continuity.immediateContextIdentity = 0x20005000;
        proof.continuity.eyeResourceIdentities[0] = 0x20006000;
        proof.continuity.eyeResourceIdentities[1] = 0x20007000;
        proof.continuity.specializationCount =
            kReachDisplaySurfaceCount;
        proof.continuity.selectedSpecialization = 0;
        proof.preflight = preflight;
        proof.immediateContextIdentity = 0x20005000;
        proof.eyeResourceIdentities[0] = 0x20006000;
        proof.eyeResourceIdentities[1] = 0x20007000;
        proof.source = {1920, 1080, 1, 1,
                        kReachDisplayFormatR8G8B8A8Unorm, 1, 0};
        proof.eyes[0] = proof.source;
        proof.eyes[1] = proof.source;
        proof.readyEyeMask = 0x3u;
        proof.engineSwapchainMatchesPresent = true;
        proof.selectedRtvMatchesRecord0 = true;
        proof.swapchainContract = true;
        proof.sameDevice = true;
        proof.immediateContext = true;
        return proof;
    }

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }

    size_t CountText(std::string_view text, std::string_view needle)
    {
        size_t count = 0;
        for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string_view::npos;
             pos += needle.size())
            ++count;
        return count;
    }
}

int main()
{
    {
    // Standalone controller-pose inertia math. No OpenXR or game process is
    // loaded here; each hand owns an independent filter instance at runtime.
    {
        const auto identityPose = [] {
            PoseInertiaPose pose{};
            pose.orientation[3] = 1.0f;
            return pose;
        };
        const auto yawPose = [&](float degrees) {
            PoseInertiaPose pose = identityPose();
            const float half = degrees * 0.00872664626f;
            pose.orientation[1] = std::sin(half);
            pose.orientation[3] = std::cos(half);
            return pose;
        };
        const auto quaternionErrorDegrees = [](const PoseInertiaPose& a,
                                               const PoseInertiaPose& b) {
            float dot = 0.0f;
            for (int component = 0; component < 4; ++component)
                dot += a.orientation[component] * b.orientation[component];
            dot = std::clamp(std::fabs(dot), 0.0f, 1.0f);
            return 2.0f * std::acos(dot) * 57.2957795f;
        };
        PoseInertiaSettings settings{};
        PoseInertiaPose output{};

        PoseInertiaFilter first;
        PoseInertiaPose firstTarget = yawPose(35.0f);
        firstTarget.position[0] = 0.4f;
        Check(first.Update(true, true, firstTarget, 1.0f / 90.0f,
                           settings, output) &&
                  output.position[0] == firstTarget.position[0] &&
                  quaternionErrorDegrees(output, firstTarget) < 0.001f,
            "Weapon inertia first valid pose initializes without a jump");

        PoseInertiaFilter disabled;
        PoseInertiaPose raw = yawPose(20.0f);
        raw.position[0] = 0.123f;
        raw.position[1] = -0.456f;
        Check(disabled.Update(false, true, raw, 1.0f / 72.0f,
                              settings, output) &&
                  std::memcmp(&raw, &output, sizeof(raw)) == 0,
            "Disabled weapon inertia returns the exact normalized raw pose");

        PoseInertiaSettings convergence = settings;
        PoseInertiaFilter positionFilter;
        PoseInertiaPose origin = identityPose();
        PoseInertiaPose positionTarget = origin;
        positionTarget.position[0] = 1.0f;
        positionFilter.Update(true, true, origin, 1.0f / 90.0f,
                              convergence, output);
        for (int frame = 0; frame < 180; ++frame)
            positionFilter.Update(true, true, positionTarget, 1.0f / 90.0f,
                                  convergence, output);
        Check(std::fabs(output.position[0] - 1.0f) < 0.001f,
            "Weapon inertia position converges on a stationary target");

        PoseInertiaFilter rotationFilter;
        const PoseInertiaPose rotationTarget = yawPose(90.0f);
        rotationFilter.Update(true, true, origin, 1.0f / 90.0f,
                              convergence, output);
        for (int frame = 0; frame < 180; ++frame)
            rotationFilter.Update(true, true, rotationTarget, 1.0f / 90.0f,
                                  convergence, output);
        Check(quaternionErrorDegrees(output, rotationTarget) < 0.05f,
            "Weapon inertia rotation converges on a stationary target");

        PoseInertiaFilter compositeWeapon;
        PoseInertiaPose compositeTarget = yawPose(55.0f);
        compositeTarget.position[0] = 0.35f;
        compositeWeapon.Update(true, true, origin, 1.0f / 90.0f,
                               convergence, output);
        compositeWeapon.Update(true, true, compositeTarget, 1.0f / 90.0f,
                               convergence, output);
        Check(output.position[0] > 0.0f &&
                  output.position[0] < compositeTarget.position[0] &&
                  quaternionErrorDegrees(output, origin) > 0.0f &&
                  quaternionErrorDegrees(output, compositeTarget) > 0.0f,
            "One composite weapon spring advances translation and two-hand rotation together");

        PoseInertiaFilter unboundedPosition;
        unboundedPosition.Update(true, true, origin, 1.0f / 90.0f,
                                 settings, output);
        unboundedPosition.Update(true, true, positionTarget, 1.0f / 90.0f,
                                 settings, output);
        Check(std::fabs(positionTarget.position[0] - output.position[0]) > 0.15f,
            "Weapon inertia does not project position onto a 15 cm leash");

        PoseInertiaFilter unboundedRotation;
        unboundedRotation.Update(true, true, origin, 1.0f / 90.0f,
                                 settings, output);
        unboundedRotation.Update(true, true, rotationTarget, 1.0f / 90.0f,
                                 settings, output);
        Check(quaternionErrorDegrees(output, rotationTarget) > 20.0f,
            "Weapon inertia does not project rotation onto a 20 degree leash");

        // Reproduce the headset report: at 60% weight a rapid 5 m/s hand sweep
        // used to ride the old 15 cm hard boundary and repeatedly erase
        // velocity. Catch-up must remain continuous without any pose leash.
        PoseInertiaSettings fastMotion = settings;
        fastMotion.positionFollow = 17.2f;
        fastMotion.rotationFollow = 19.2f;
        fastMotion.catchupSpeed = 0.75f;
        const auto simulateFastSweep = [&](PoseInertiaSettings sweepSettings,
                                           float& maximumStep) {
            PoseInertiaFilter filter;
            PoseInertiaPose target = origin;
            PoseInertiaPose result{};
            filter.Update(true, true, target, 1.0f / 90.0f,
                          sweepSettings, result);
            float maximumLag = 0.0f;
            maximumStep = 0.0f;
            float previous = result.position[0];
            constexpr float inputStep = 5.0f / 90.0f;
            for (int frame = 0; frame < 90; ++frame)
            {
                target.position[0] += inputStep;
                filter.Update(true, true, target, 1.0f / 90.0f,
                              sweepSettings, result);
                maximumLag = std::max(
                    maximumLag, target.position[0] - result.position[0]);
                maximumStep = std::max(
                    maximumStep, result.position[0] - previous);
                previous = result.position[0];
            }
            return maximumLag;
        };
        float catchupStep = 0.0f;
        const float catchupLag = simulateFastSweep(fastMotion, catchupStep);
        PoseInertiaSettings noCatchup = fastMotion;
        noCatchup.catchupSpeed = 0.0f;
        float noCatchupStep = 0.0f;
        const float noCatchupLag = simulateFastSweep(noCatchup, noCatchupStep);
        Check(catchupLag < 0.14f && noCatchupLag > 0.20f &&
                  catchupStep < (5.0f / 90.0f) * 1.10f,
            "Weapon catch-up smoothly closes 60%-weight fast-sweep error without a leash");

        const auto simulateRate = [&](int hz) {
            PoseInertiaFilter filter;
            PoseInertiaPose result{};
            filter.Update(true, true, origin, 1.0f / hz,
                          convergence, result);
            for (int frame = 0; frame < hz; ++frame)
                filter.Update(true, true, positionTarget, 1.0f / hz,
                              convergence, result);
            return result.position[0];
        };
        const float rate72 = simulateRate(72);
        const float rate80 = simulateRate(80);
        const float rate90 = simulateRate(90);
        const float rate120 = simulateRate(120);
        const float rate144 = simulateRate(144);
        const float rateMin = std::min({rate72, rate80, rate90, rate120, rate144});
        const float rateMax = std::max({rate72, rate80, rate90, rate120, rate144});
        Check(rateMax - rateMin < 0.01f,
            "Weapon inertia remains similar at 72, 80, 90, 120 and 144 Hz");

        PoseInertiaFilter trackingReset;
        trackingReset.Update(true, true, origin, 1.0f / 90.0f,
                             settings, output);
        trackingReset.Update(true, true, positionTarget, 1.0f / 90.0f,
                             settings, output);
        Check(!trackingReset.Update(true, false, positionTarget,
                                   1.0f / 90.0f, settings, output),
            "Weapon inertia tracking loss invalidates and resets state");
        PoseInertiaPose reacquired = origin;
        reacquired.position[1] = 0.75f;
        trackingReset.Update(true, true, reacquired, 1.0f / 90.0f,
                             settings, output);
        Check(output.position[1] == reacquired.position[1],
            "Weapon inertia reacquisition initializes at the new raw pose");

        PoseInertiaFilter longGap;
        longGap.Update(true, true, origin, 1.0f / 90.0f, settings, output);
        longGap.Update(true, true, positionTarget, 0.5f, settings, output);
        Check(output.position[0] == positionTarget.position[0],
            "Weapon inertia snaps safely after a long frame gap");

        float quaternionLength = 0.0f;
        for (float component : output.orientation)
            quaternionLength += component * component;
        Check(std::isfinite(quaternionLength) &&
                  std::fabs(quaternionLength - 1.0f) < 0.0001f,
            "Weapon inertia quaternion output stays finite and normalized");

        PoseInertiaFilter leftHand;
        PoseInertiaFilter rightHand;
        PoseInertiaPose leftTarget = origin;
        PoseInertiaPose rightTarget = origin;
        leftTarget.position[0] = -1.0f;
        rightTarget.position[0] = 1.0f;
        PoseInertiaPose leftOutput{}, rightOutput{};
        leftHand.Update(true, true, origin, 1.0f / 90.0f, settings, leftOutput);
        rightHand.Update(true, true, origin, 1.0f / 90.0f, settings, rightOutput);
        leftHand.Update(true, true, leftTarget, 1.0f / 90.0f, settings, leftOutput);
        rightHand.Update(true, true, rightTarget, 1.0f / 90.0f, settings, rightOutput);
        Check(leftOutput.position[0] < 0.0f && rightOutput.position[0] > 0.0f,
            "Left and right weapon inertia state remains independent");

        PoseInertiaFilter reenabled;
        reenabled.Update(true, true, origin, 1.0f / 90.0f, settings, output);
        reenabled.Update(false, true, positionTarget, 1.0f / 90.0f,
                         settings, output);
        reenabled.Update(true, true, positionTarget, 1.0f / 90.0f,
                         settings, output);
        Check(output.position[0] == positionTarget.position[0],
            "Re-enabling weapon inertia cannot replay a stale lag jump");
    }

        constexpr std::array<uint8_t, 6> repeatedPattern{
            0xAA, 0xBB, 0xCC, 0xAA, 0xBB, 0xCC
        };
        constexpr std::array<uint8_t, 3> twice{ 0xAA, 0xBB, 0xCC };
        constexpr std::array<uint8_t, 3> once{ 0xBB, 0xCC, 0xAA };
        constexpr std::array<uint8_t, 2> absent{ 0xCC, 0xBB };
        Check(CountReachExactPattern(
                  repeatedPattern.data(), repeatedPattern.size(),
                  twice.data(), twice.size()) == 2,
            "Reach loaded-image proof rejects a multiple-match exact pattern");
        Check(CountReachExactPattern(
                  repeatedPattern.data(), repeatedPattern.size(),
                  once.data(), once.size()) == 1,
            "Reach loaded-image proof accepts one exact pattern match");
        Check(CountReachExactPattern(
                  repeatedPattern.data(), repeatedPattern.size(),
                  absent.data(), absent.size()) == 0,
            "Reach loaded-image proof rejects an absent exact pattern");
        Check(CountReachExactPattern(
                  nullptr, repeatedPattern.size(), twice.data(), twice.size()) == 0 &&
                  CountReachExactPattern(
                      repeatedPattern.data(), repeatedPattern.size(),
                      nullptr, twice.size()) == 0 &&
                  CountReachExactPattern(
                      repeatedPattern.data(), repeatedPattern.size(),
                      twice.data(), 0) == 0,
            "Reach exact-pattern counting fails closed on invalid inputs");

        std::array<uint8_t, 5> call{ 0xE8, 0, 0, 0, 0 };
        uintptr_t callTarget = 0;
        int32_t displacement = 0x20;
        std::memcpy(call.data() + 1, &displacement, sizeof(displacement));
        Check(ResolveReachRel32Call(
                  0x1000, call.data(), call.size(), callTarget) &&
                  callTarget == 0x1025,
            "Reach rel32 proof resolves a forward call target exactly");
        displacement = -0x20;
        std::memcpy(call.data() + 1, &displacement, sizeof(displacement));
        Check(ResolveReachRel32Call(
                  0x1000, call.data(), call.size(), callTarget) &&
                  callTarget == 0x0FE5,
            "Reach rel32 proof resolves a backward call target exactly");
        call[0] = 0xE9;
        Check(!ResolveReachRel32Call(
                  0x1000, call.data(), call.size(), callTarget),
            "Reach rel32 proof rejects a non-call opcode");
        call[0] = 0xE8;
        Check(!ResolveReachRel32Call(
                  0x1000, call.data(), 4, callTarget),
            "Reach rel32 proof rejects a truncated instruction");
        displacement = -6;
        std::memcpy(call.data() + 1, &displacement, sizeof(displacement));
        Check(!ResolveReachRel32Call(
                  0, call.data(), call.size(), callTarget),
            "Reach rel32 proof rejects target-address underflow");
        displacement = 1;
        std::memcpy(call.data() + 1, &displacement, sizeof(displacement));
        Check(!ResolveReachRel32Call(
                  std::numeric_limits<uintptr_t>::max() - 5,
                  call.data(), call.size(), callTarget),
            "Reach rel32 proof rejects target-address overflow");

        constexpr uintptr_t playerArray = 0x100000;
        constexpr size_t playerStride = 0xA40;
        constexpr size_t playerCount = 4;
        for (uint32_t slot = 0; slot < playerCount; ++slot)
        {
            const ReachObservedView view = ClassifyReachObservedView(
                playerArray + slot * playerStride,
                playerArray, playerStride, playerCount);
            Check(view.kind == ReachObservedViewKind::NormalPlayerSlot &&
                      view.slot == slot,
                "Reach active-view proof recognizes each exact 0xA40 player slot");
        }
        Check(ClassifyReachObservedView(
                  0, playerArray, playerStride, playerCount).kind ==
                  ReachObservedViewKind::None,
            "Reach active-view proof treats a null pointer as no transaction");
        Check(ClassifyReachObservedView(
                  playerArray + 1, playerArray, playerStride, playerCount).kind ==
                  ReachObservedViewKind::OutsidePlayerArray,
            "Reach active-view proof rejects an interior non-slot pointer");
        Check(ClassifyReachObservedView(
                  playerArray - 1, playerArray, playerStride, playerCount).kind ==
                  ReachObservedViewKind::OutsidePlayerArray &&
                  ClassifyReachObservedView(
                      playerArray + playerStride * playerCount,
                      playerArray, playerStride, playerCount).kind ==
                      ReachObservedViewKind::OutsidePlayerArray,
            "Reach active-view proof rejects pointers outside the four-slot array");
        Check(ClassifyReachObservedView(
                  playerArray, 0, playerStride, playerCount).kind ==
                  ReachObservedViewKind::OutsidePlayerArray &&
                  ClassifyReachObservedView(
                      playerArray, playerArray, 0, playerCount).kind ==
                      ReachObservedViewKind::OutsidePlayerArray,
            "Reach active-view proof fails closed on an invalid array description");

        ReachObserverTransactionGate transactionGate;
        Check(!transactionGate.Observe(playerArray),
            "Reach observer ignores a mid-transaction value at session start");
        Check(!transactionGate.Observe(0) &&
                  transactionGate.Observe(playerArray),
            "Reach observer requires a witnessed clear before its first transaction");
        Check(!transactionGate.Observe(playerArray),
            "Reach observer counts a latched pointer only once");
        Check(transactionGate.Observe(playerArray + playerStride),
            "Reach observer recognizes a changed non-null transaction owner");
        transactionGate.RequireClear();
        Check(!transactionGate.Observe(playerArray + playerStride) &&
                  !transactionGate.HasLatchedValue() &&
                  !transactionGate.Observe(0) &&
                  transactionGate.Observe(playerArray + playerStride),
            "Reach continuity reset requires a new witnessed clear");
    }

    {
        // Independent HREK literals: the validator must accept both optimized
        // official chud_draw_widget bodies and reject any ownership-bearing
        // byte drift. The rel32 itself is link-layout data and is intentionally
        // allowed to differ.
        constexpr std::array<uint8_t, 33> officialEntry{
            0x48, 0x8B, 0xC4, 0x44, 0x89, 0x48, 0x20, 0x48, 0x89, 0x50, 0x10,
            0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
            0x48, 0x8D, 0x68, 0xA9, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00};
        constexpr std::array<uint8_t, 3> tagDescriptorMove{
            0x4C, 0x8B, 0xF2};
        constexpr std::array<uint8_t, 3> sapienDescriptorMove{
            0x4C, 0x8B, 0xFA};
        constexpr std::array<uint8_t, 4> fifthArgumentLoad{
            0x4C, 0x8B, 0x4D, 0x7F};
        constexpr std::array<uint8_t, 6> tagClassRead{
            0x41, 0x0F, 0xBE, 0x56, 0x04, 0xE8};
        constexpr std::array<uint8_t, 6> sapienClassRead{
            0x41, 0x0F, 0xBE, 0x57, 0x04, 0xE8};
        constexpr std::array<uint8_t, 3> postClassCall{
            0x48, 0x8B, 0xD0};
        Check(kReachHrekChudDrawWidgetEntryBytes == officialEntry,
            "Reach CHUD entry bytes remain the independently recorded official HREK signature");

        std::array<uint8_t, kReachTagPlayChudDrawWidgetBodySize> tagPlay{};
        std::memcpy(tagPlay.data(), officialEntry.data(), officialEntry.size());
        std::memcpy(
            tagPlay.data() + kReachTagPlayChudDescriptorMoveOffset,
            tagDescriptorMove.data(), tagDescriptorMove.size());
        std::memcpy(
            tagPlay.data() + kReachTagPlayChudFifthArgumentLoadOffset,
            fifthArgumentLoad.data(), fifthArgumentLoad.size());
        std::memcpy(
            tagPlay.data() + kReachTagPlayChudClassReadOffset,
            tagClassRead.data(), tagClassRead.size());
        tagPlay[kReachTagPlayChudClassReadOffset + 6] = 0x12;
        tagPlay[kReachTagPlayChudClassReadOffset + 7] = 0x34;
        tagPlay[kReachTagPlayChudClassReadOffset + 8] = 0x56;
        tagPlay[kReachTagPlayChudClassReadOffset + 9] = 0x78;
        std::memcpy(
            tagPlay.data() + kReachTagPlayChudClassReadOffset + 10,
            postClassCall.data(), postClassCall.size());
        Check(ReachHrekChudDrawWidgetLayoutMatches(tagPlay),
            "Reach CHUD validator accepts the exact official reach_tag_play layout");
        Check(!ReachHrekChudDrawWidgetLayoutMatches(
                  std::span<const uint8_t>(
                      tagPlay.data(), tagPlay.size() - 1)),
            "Reach CHUD validator rejects a non-official tag-play body size");
        tagPlay[0] = 0x49;
        Check(!ReachHrekChudDrawWidgetLayoutMatches(tagPlay),
            "Reach CHUD validator rejects official-entry signature drift");
        tagPlay[0] = officialEntry[0];
        tagPlay[kReachTagPlayChudFifthArgumentLoadOffset + 3] = 0x7E;
        Check(!ReachHrekChudDrawWidgetLayoutMatches(tagPlay),
            "Reach CHUD validator rejects fifth-argument ABI drift in tag-play");
        tagPlay[kReachTagPlayChudFifthArgumentLoadOffset + 3] = 0x7F;
        tagPlay[kReachTagPlayChudClassReadOffset + 4] = 0x05;
        Check(!ReachHrekChudDrawWidgetLayoutMatches(tagPlay),
            "Reach CHUD validator rejects descriptor-class offset drift in tag-play");
        tagPlay[kReachTagPlayChudClassReadOffset + 4] = 0x04;

        std::array<uint8_t, kReachSapienPlayChudDrawWidgetBodySize> sapienPlay{};
        std::memcpy(
            sapienPlay.data(), officialEntry.data(), officialEntry.size());
        std::memcpy(
            sapienPlay.data() + kReachSapienPlayChudDescriptorMoveOffset,
            sapienDescriptorMove.data(), sapienDescriptorMove.size());
        std::memcpy(
            sapienPlay.data() + kReachSapienPlayChudFifthArgumentLoadOffset,
            fifthArgumentLoad.data(), fifthArgumentLoad.size());
        std::memcpy(
            sapienPlay.data() + kReachSapienPlayChudClassReadOffset,
            sapienClassRead.data(), sapienClassRead.size());
        sapienPlay[kReachSapienPlayChudClassReadOffset + 6] = 0xDE;
        sapienPlay[kReachSapienPlayChudClassReadOffset + 7] = 0xAD;
        sapienPlay[kReachSapienPlayChudClassReadOffset + 8] = 0xBE;
        sapienPlay[kReachSapienPlayChudClassReadOffset + 9] = 0xEF;
        std::memcpy(
            sapienPlay.data() + kReachSapienPlayChudClassReadOffset + 10,
            postClassCall.data(), postClassCall.size());
        Check(ReachHrekChudDrawWidgetLayoutMatches(sapienPlay),
            "Reach CHUD validator accepts the exact official sapien_play layout");
        sapienPlay[kReachSapienPlayChudDescriptorMoveOffset + 2] = 0xF2;
        Check(!ReachHrekChudDrawWidgetLayoutMatches(sapienPlay),
            "Reach CHUD validator rejects descriptor-register drift in sapien-play");
        sapienPlay[kReachSapienPlayChudDescriptorMoveOffset + 2] = 0xFA;
        sapienPlay[kReachSapienPlayChudClassReadOffset + 10] = 0x49;
        Check(!ReachHrekChudDrawWidgetLayoutMatches(sapienPlay),
            "Reach CHUD validator rejects post-class-call flow drift in sapien-play");

        using Action = ReachChudCrosshairAction;
        Check(ReachDecideChudCrosshairAction(
                  false, true, kReachChudCrosshairScriptingClass,
                  true, true, 0, false) == Action::DrawStock &&
              ReachDecideChudCrosshairAction(
                  false, false, kReachChudCrosshairScriptingClass,
                  true, true, -1, true) == Action::DrawStock,
            "Reach CHUD stays stock outside its owned stereo transaction");
        Check(ReachDecideChudCrosshairAction(
                  true, false, kReachChudCrosshairScriptingClass,
                  false, true, 0, false) == Action::RejectTransaction,
            "Owned Reach CHUD rejects the transaction when its scripting-class descriptor is unreadable");
        Check(ReachDecideChudCrosshairAction(
                  true, true, 1, false, true, 0, false) ==
                  Action::DrawStock,
            "Reach CHUD never suppresses a non-crosshair scripting class");
        Check(ReachDecideChudCrosshairAction(
                  true, true, kReachChudCrosshairScriptingClass,
                  false, true, 0, false) == Action::Suppress,
            "The universal crosshair switch suppresses only the native class-2 widget");
        Check(ReachDecideChudCrosshairAction(
                  true, true, kReachChudCrosshairScriptingClass,
                  true, false, 0, false) == Action::DrawStock,
            "The universal kill-reticle switch deliberately restores the stock widget");
        Check(ReachDecideChudCrosshairAction(
                  true, true, kReachChudCrosshairScriptingClass,
                  true, true, 0, false) == Action::CaptureAuthored &&
              ReachDecideChudCrosshairAction(
                  true, true, kReachChudCrosshairScriptingClass,
                  true, true, 1, false) == Action::Suppress,
            "Left-eye-first captures the authored widget once and suppresses its opposite-eye duplicate");
        Check(ReachDecideChudCrosshairAction(
                  true, true, kReachChudCrosshairScriptingClass,
                  true, true, 1, true) == Action::CaptureAuthored &&
              ReachDecideChudCrosshairAction(
                  true, true, kReachChudCrosshairScriptingClass,
                  true, true, 0, true) == Action::Suppress,
            "Right-eye-first captures the authored widget once and suppresses its opposite-eye duplicate");
        Check(ReachDecideChudCrosshairAction(
                  true, true, kReachChudCrosshairScriptingClass,
                  true, true, -1, true) == Action::RejectTransaction &&
              ReachDecideChudCrosshairAction(
                  true, true, kReachChudCrosshairScriptingClass,
                  true, true, 2, false) == Action::RejectTransaction,
            "Owned Reach CHUD rejects invalid eyes and never captures them as an authored fallback");

        Check(ReachAuthoredCrosshairPairComplete(false, true, false) &&
              ReachAuthoredCrosshairPairComplete(true, false, false) &&
              ReachAuthoredCrosshairPairComplete(true, true, true) &&
              !ReachAuthoredCrosshairPairComplete(true, true, false),
            "A required Reach authored crosshair rejects a pair only when class 2 was emitted without a completed capture");

        Check(ReachCanSubmitCompleteProjection(
                  false, false, true, false) &&
              ReachCanSubmitCompleteProjection(
                  false, true, false, false) &&
              ReachCanSubmitCompleteProjection(
                  true, true, false, true) &&
              !ReachCanSubmitCompleteProjection(
                  true, false, false, true) &&
              !ReachCanSubmitCompleteProjection(
                  true, true, true, true) &&
              !ReachCanSubmitCompleteProjection(
                  true, true, false, false),
            "Reach queues its world projection only after both eye uploads, authored upload success, and a live post-upload owner proof");
    }

    {
        static_assert(kReachSpartanFpBodyBoneMap.size() == 47);
        static_assert(kReachEliteFpBodyBoneMap.size() == 41);
        static_assert(kReachFpMaxSourceNodeCount == 120);

        // Independent evidence literals: do not derive these from the
        // production constants, so any fingerprint drift fails this test.
        constexpr std::array<int32_t, 47> expectedSpartan{{
            0, 3, 1, 2, 5, 4, 6, 9, 10, 8, 7, 13, 12, 14, 11, 22,
            26, 18, 21, 24, 19, 20, 15, 23, 16, 17, 25, 36, 35, 32,
            28, 33, 29, 27, 34, 31, 30, 46, 38, 42, 37, 40, 39, 43,
            45, 44, 41}};
        constexpr std::array<int32_t, 41> expectedElite{{
            0, 1, 2, 3, 5, 4, 6, 7, 8, 9, 10, 14, 12, 11, 13, 22,
            20, 18, 21, 19, 16, 23, 24, 17, 15, 27, 28, 25, 31, 32,
            30, 26, 29, 35, 34, 39, 33, 40, 36, 38, 37}};
        Check(kReachSpartanFpBodyBoneMap == expectedSpartan &&
                  kReachEliteFpBodyBoneMap == expectedElite,
            "Reach production fingerprints match independent full evidence literals");
        Check(expectedSpartan[6] == 6 && expectedSpartan[7] == 9 &&
                  expectedSpartan[8] == 10 && expectedSpartan[11] == 13 &&
                  expectedSpartan[13] == 14 &&
                  expectedSpartan[4] == 5 && expectedSpartan[10] == 7 &&
                  expectedSpartan[14] == 11 && expectedSpartan[5] == 4 &&
                  expectedElite[6] == 6 && expectedElite[9] == 9 &&
                  expectedElite[10] == 10 && expectedElite[11] == 14 &&
                  expectedElite[14] == 13 &&
                  expectedElite[4] == 5 && expectedElite[7] == 7 &&
                  expectedElite[13] == 11 && expectedElite[5] == 4,
            "Reach render-output chains map exactly to the shared source chains");

        const std::span<const int32_t> spartanMap{
            kReachSpartanFpBodyBoneMap};
        ReachFpBodyLayout spartan{};
        Check(ResolveReachFpBodyLayout(spartanMap, 65, spartan) &&
                  spartan.Valid() &&
                  spartan.kind == ReachFpBodyKind::Spartan &&
                  spartan.paletteBodyNodeCount == 47 &&
                  spartan.liveSourceNodeCount == 65,
            "Reach Spartan keeps the 47-node body distinct from the 65-node source");
        Check(spartan.rightShoulderSource == 6 &&
                  spartan.rightElbowSource == 9 &&
                  spartan.rightWristSource == 13 &&
                  spartan.leftShoulderSource == 5 &&
                  spartan.leftElbowSource == 7 &&
                  spartan.leftWristSource == 11 &&
                  spartan.cameraControlSource == 4,
            "Reach Spartan resolves the exact source arm and camera anchors");
        Check(spartan.leftHandSourceDescendants ==
                       0x000003E0F81F8800ull &&
                   spartan.rightHandSourceDescendants ==
                       0x00007C1F07E02000ull,
            "Reach Spartan maps exact hand descendants into source order");
        Check(kReachLeftControllerOwnedAuxiliarySourceMask ==
                      0x00000000000011A0ull &&
                  kReachRightControllerOwnedAuxiliarySourceMask ==
                      0x0000000000004640ull &&
                  spartan.leftControllerOwnedSourceBranch ==
                      0x000003E0F81F99A0ull &&
                  spartan.rightControllerOwnedSourceBranch ==
                      0x00007C1F07E06640ull &&
                  (spartan.leftControllerOwnedSourceBranch ^
                   spartan.leftHandSourceDescendants) ==
                      kReachLeftControllerOwnedAuxiliarySourceMask &&
                  (spartan.rightControllerOwnedSourceBranch ^
                   spartan.rightHandSourceDescendants) ==
                      kReachRightControllerOwnedAuxiliarySourceMask &&
                  (spartan.leftControllerOwnedSourceBranch &
                   ~spartan.leftHandSourceDescendants) ==
                      kReachLeftControllerOwnedAuxiliarySourceMask &&
                  (spartan.rightControllerOwnedSourceBranch &
                   ~spartan.rightHandSourceDescendants) ==
                      kReachRightControllerOwnedAuxiliarySourceMask &&
                  (spartan.leftHandSourceDescendants &
                   kReachLeftControllerOwnedAuxiliarySourceMask) == 0 &&
                  (spartan.rightHandSourceDescendants &
                   kReachRightControllerOwnedAuxiliarySourceMask) == 0 &&
                  (spartan.leftControllerOwnedSourceBranch &
                   spartan.rightHandSourceDescendants) == 0 &&
                  (spartan.leftControllerOwnedSourceBranch &
                   spartan.rightControllerOwnedSourceBranch) == 0 &&
                  (spartan.leftControllerOwnedSourceBranch >>
                   spartan.paletteBodyNodeCount) == 0 &&
                  (spartan.rightControllerOwnedSourceBranch >>
                   spartan.paletteBodyNodeCount) == 0,
            "Reach Spartan controllers own the exact hidden skin-influence closures");
        auto brokenSpartanOwnership = spartan;
        brokenSpartanOwnership.leftControllerOwnedSourceBranch ^=
            uint64_t{1} << 12;
        Check(!brokenSpartanOwnership.Valid(),
            "Reach Spartan rejects an incomplete left controller influence branch");
        auto brokenSpartanRightOwnership = spartan;
        brokenSpartanRightOwnership.rightControllerOwnedSourceBranch ^=
            uint64_t{1} << 14;
        Check(!brokenSpartanRightOwnership.Valid(),
            "Reach Spartan rejects an incomplete right controller influence branch");
        Check((spartan.leftHandSourceDescendants &
                   (uint64_t{1} << spartan.leftWristSource)) != 0 &&
                  (spartan.rightHandSourceDescendants &
                   (uint64_t{1} << spartan.rightWristSource)) != 0 &&
                  (spartan.leftHandSourceDescendants &
                   spartan.rightHandSourceDescendants) == 0 &&
                  (spartan.leftHandSourceDescendants >>
                   spartan.paletteBodyNodeCount) == 0,
            "Reach Spartan floating hands are disjoint exact wrist subtrees and exclude held objects");
    }

    {
        const std::span<const int32_t> spartanMap{
            kReachSpartanFpBodyBoneMap};
        ReachFpBodyLayout spartan{};
        Check(ResolveReachFpBodyLayout(spartanMap, 65, spartan) &&
                  spartan.leftHandPaletteDescendants ==
                      kReachSpartanLeftHandPaletteMask &&
                  spartan.rightHandPaletteDescendants ==
                      kReachSpartanRightHandPaletteMask &&
                  !ReachFpSourceIndexIsHeldObject(spartan, -1) &&
                  !ReachFpSourceIndexIsHeldObject(spartan, 46) &&
                  ReachFpSourceIndexIsHeldObject(spartan, 47) &&
                  ReachFpSourceIndexIsHeldObject(spartan, 64) &&
                  !ReachFpSourceIndexIsHeldObject(spartan, 65),
            "Reach Spartan held-object range covers appended indices 47 through 64");
        ReachFpBodyLayout maximumSource{};
        Check(ResolveReachFpBodyLayout(
                  spartanMap, kReachFpMaxSourceNodeCount, maximumSource) &&
                  maximumSource.paletteBodyNodeCount == 47 &&
                  maximumSource.liveSourceNodeCount == 120 &&
                  ReachFpSourceIndexIsHeldObject(maximumSource, 119),
            "Reach body resolution accepts the bounded 120-node source span");

        const std::span<const int32_t> eliteMap{kReachEliteFpBodyBoneMap};
        ReachFpBodyLayout elite{};
        Check(ResolveReachFpBodyLayout(eliteMap, 59, elite) &&
                  elite.Valid() && elite.kind == ReachFpBodyKind::Elite &&
                  elite.paletteBodyNodeCount == 41 &&
                  elite.liveSourceNodeCount == 59 &&
                  elite.rightShoulderSource == 6 &&
                  elite.rightElbowSource == 9 &&
                  elite.rightWristSource == 13 &&
                  elite.leftShoulderSource == 5 &&
                  elite.leftElbowSource == 7 &&
                  elite.leftWristSource == 11 &&
                  elite.cameraControlSource == 4,
            "Reach Elite resolves its 41-node body over the shared arm prefix");
        Check(elite.leftHandPaletteDescendants ==
                      kReachEliteLeftHandPaletteMask &&
                  elite.rightHandPaletteDescendants ==
                      kReachEliteRightHandPaletteMask &&
                  elite.leftHandSourceDescendants ==
                      0x0000001E1E0F8800ull &&
                  elite.rightHandSourceDescendants ==
                      0x000001E1E1F02000ull &&
                  !ReachFpSourceIndexIsHeldObject(elite, 40) &&
                  ReachFpSourceIndexIsHeldObject(elite, 41) &&
                  ReachFpSourceIndexIsHeldObject(elite, 58) &&
                  !ReachFpSourceIndexIsHeldObject(elite, 59),
            "Reach Elite maps exact hand sets and appended held-object range");
        Check(elite.leftControllerOwnedSourceBranch ==
                      0x0000001E1E0F99A0ull &&
                  elite.rightControllerOwnedSourceBranch ==
                      0x000001E1E1F06640ull &&
                  (elite.leftControllerOwnedSourceBranch ^
                   elite.leftHandSourceDescendants) ==
                      kReachLeftControllerOwnedAuxiliarySourceMask &&
                  (elite.rightControllerOwnedSourceBranch ^
                   elite.rightHandSourceDescendants) ==
                      kReachRightControllerOwnedAuxiliarySourceMask &&
                  (elite.leftControllerOwnedSourceBranch &
                   ~elite.leftHandSourceDescendants) ==
                      kReachLeftControllerOwnedAuxiliarySourceMask &&
                  (elite.rightControllerOwnedSourceBranch &
                   ~elite.rightHandSourceDescendants) ==
                      kReachRightControllerOwnedAuxiliarySourceMask &&
                  (elite.leftHandSourceDescendants &
                   kReachLeftControllerOwnedAuxiliarySourceMask) == 0 &&
                  (elite.rightHandSourceDescendants &
                   kReachRightControllerOwnedAuxiliarySourceMask) == 0 &&
                  (elite.leftControllerOwnedSourceBranch &
                   elite.rightHandSourceDescendants) == 0 &&
                  (elite.leftControllerOwnedSourceBranch &
                   elite.rightControllerOwnedSourceBranch) == 0 &&
                  (elite.leftControllerOwnedSourceBranch >>
                   elite.paletteBodyNodeCount) == 0 &&
                  (elite.rightControllerOwnedSourceBranch >>
                   elite.paletteBodyNodeCount) == 0,
            "Reach Elite controllers own the exact shared hidden arm branches");
        Check((elite.leftHandSourceDescendants &
                   (uint64_t{1} << elite.leftWristSource)) != 0 &&
                  (elite.rightHandSourceDescendants &
                   (uint64_t{1} << elite.rightWristSource)) != 0 &&
                  (elite.leftHandSourceDescendants &
                   elite.rightHandSourceDescendants) == 0 &&
                  (elite.leftHandSourceDescendants >>
                   elite.paletteBodyNodeCount) == 0,
            "Reach Elite floating hands are disjoint exact wrist subtrees and exclude held objects");
    }

    {
        const std::span<const int32_t> spartanMap{
            kReachSpartanFpBodyBoneMap};
        auto wrongFingerprint = kReachSpartanFpBodyBoneMap;
        const int32_t first = wrongFingerprint[0];
        wrongFingerprint[0] = wrongFingerprint[1];
        wrongFingerprint[1] = first;
        ReachFpBodyLayout rejected{};
        Check(!ResolveReachFpBodyLayout(
                  std::span<const int32_t>{wrongFingerprint}, 65, rejected) &&
                  !rejected.Valid() &&
                  rejected.kind == ReachFpBodyKind::None &&
                  rejected.paletteBodyNodeCount == 0 &&
                  rejected.liveSourceNodeCount == 0,
            "Reach rejects a full permutation with the wrong exact fingerprint");

        auto duplicate = kReachSpartanFpBodyBoneMap;
        duplicate[1] = duplicate[0];
        auto negative = kReachSpartanFpBodyBoneMap;
        negative[0] = -1;
        auto appendedInBody = kReachSpartanFpBodyBoneMap;
        appendedInBody[0] = 47;
        Check(!ResolveReachFpBodyLayout(
                  std::span<const int32_t>{duplicate}, 65, rejected) &&
                  !ResolveReachFpBodyLayout(
                      std::span<const int32_t>{negative}, 65, rejected) &&
                  !ResolveReachFpBodyLayout(
                      std::span<const int32_t>{appendedInBody}, 65,
                      rejected),
            "Reach rejects duplicate, negative, and appended body-map indices");

        ReachFpBodyLayout accepted{};
        Check(ResolveReachFpBodyLayout(spartanMap, 65, accepted),
            "Reach prepares a valid layout before fail-closed reset testing");
        rejected = accepted;
        Check(!ResolveReachFpBodyLayout(
                  spartanMap.first(46), 65, rejected) &&
                  !rejected.Valid() &&
                  rejected.rightShoulderSource == -1 &&
                  rejected.rightHandSourceDescendants == 0,
            "Reach rejects a wrong palette span and clears stale output");
        Check(!ResolveReachFpBodyLayout(spartanMap, 46, rejected) &&
                  !ResolveReachFpBodyLayout(
                      spartanMap, kReachFpMaxSourceNodeCount + 1, rejected) &&
                  !ResolveReachFpBodyLayout(
                      std::span<const int32_t>{}, 65, rejected) &&
                  !ResolveReachFpBodyLayout(
                      std::span<const int32_t>{kReachEliteFpBodyBoneMap},
                      40, rejected),
            "Reach rejects unsafe live source counts and empty maps");
    }

    {
        constexpr uint32_t kGeneration = 7;
        constexpr uint64_t kDiscoverySerial = 100;
        const auto decide = [](uint32_t learnedGeneration,
                                uint64_t learnedSerial,
                                uint32_t pairGeneration,
                                uint64_t pairSerial,
                                bool valid = true,
                                bool invalidated = false) {
            return DecideReachFpPairLayout(
                valid, invalidated, learnedGeneration, learnedSerial,
                pairGeneration, pairSerial);
        };

        Check(decide(
                  kGeneration, kDiscoverySerial, kGeneration,
                  kDiscoverySerial) == ReachFpPairLayoutDecision::Stock,
            "Reach keeps the discovery pair stock");
        Check(decide(
                  kGeneration, kDiscoverySerial, kGeneration,
                  kDiscoverySerial + 1) == ReachFpPairLayoutDecision::Active,
            "Reach activates a learned layout on a later prepared serial");
        Check(decide(
                  kGeneration, kDiscoverySerial, kGeneration,
                  kDiscoverySerial - 1) == ReachFpPairLayoutDecision::Stock &&
                  decide(
                      kGeneration, kDiscoverySerial, kGeneration,
                      kDiscoverySerial) == ReachFpPairLayoutDecision::Stock,
            "Reach rejects stale and same-serial layout activation");
        Check(decide(
                  kGeneration, kDiscoverySerial, kGeneration + 1,
                  kDiscoverySerial + 1) == ReachFpPairLayoutDecision::Stock &&
                  decide(
                      0, kDiscoverySerial, 0, kDiscoverySerial + 1) ==
                      ReachFpPairLayoutDecision::Stock,
            "Reach rejects generation mismatch and zero-generation replay");
        Check(decide(
                  kGeneration, kDiscoverySerial, kGeneration,
                  kDiscoverySerial + 1, true, true) ==
                  ReachFpPairLayoutDecision::Stock &&
                  decide(
                      kGeneration, kDiscoverySerial, kGeneration,
                      kDiscoverySerial + 1, false, false) ==
                      ReachFpPairLayoutDecision::Stock &&
                  decide(
                      kGeneration, 0, kGeneration, 1) ==
                      ReachFpPairLayoutDecision::Stock &&
                  decide(
                      kGeneration, kDiscoverySerial, kGeneration, 0) ==
                      ReachFpPairLayoutDecision::Stock,
            "Reach invalidation, missing learning, and zero serials remain stock");

        const auto evaluateEyeOrder = [&](const std::array<int, 2>& eyeOrder) {
            std::array<ReachFpPairLayoutDecision, 2> eyeDecisions{
                ReachFpPairLayoutDecision::Stock,
                ReachFpPairLayoutDecision::Stock};
            for (int eye : eyeOrder)
            {
                eyeDecisions[static_cast<size_t>(eye)] = decide(
                    kGeneration, kDiscoverySerial, kGeneration,
                    kDiscoverySerial + 1);
            }
            return eyeDecisions;
        };
        const auto leftFirst = evaluateEyeOrder({0, 1});
        const auto rightFirst = evaluateEyeOrder({1, 0});
        Check(leftFirst == rightFirst &&
                  leftFirst[0] == ReachFpPairLayoutDecision::Active &&
                  leftFirst[1] == ReachFpPairLayoutDecision::Active,
            "Reach pair activation is independent of stereo eye order");
    }

    {
        using Action = ReachFpPaletteAction;
        const auto decide = [](bool current, bool frozen, bool transformed,
                               bool exactBody, bool exactMatch,
                               bool learnedTag) {
            return DecideReachFpPaletteAction(
                current, frozen, transformed, exactBody, exactMatch,
                learnedTag);
        };
        Check(decide(true, false, false, true, false, false) ==
                  Action::LearnStockOnly,
            "Reach first exact body discovery is stock-only");
        const Action precedingWeapon =
            decide(true, true, true, false, false, false);
        const Action body = decide(true, true, true, true, true, true);
        const Action followingWeapon =
            decide(true, true, true, false, false, false);
        Check(precedingWeapon == Action::ArticulateKnownTransaction &&
                  body == Action::ArticulateKnownTransaction &&
                  followingWeapon == Action::ArticulateKnownTransaction,
            "Reach reconstructs every final palette in a known source transaction");
        Check(decide(true, true, true, true, false, true) ==
                  Action::RestoreStockAndInvalidate &&
                  decide(true, true, true, true, false, false) ==
                  Action::RestoreStockAndInvalidate &&
                  decide(true, true, false, false, false, true) ==
                  Action::RestoreStockAndInvalidate,
            "Reach changed maps, body-tag transitions, and invalid known-body maps restore stock");
        Check(decide(true, true, false, false, false, false) ==
                  Action::PassThroughLive &&
                  decide(false, true, true, true, true, true) ==
                  Action::PassThroughLive,
            "Reach unsolved and stale transactions remain stock");
    }

    {
        constexpr size_t kPlasmaLauncherNodes = 65;
        constexpr size_t kFloats =
            kPlasmaLauncherNodes * kReachFpBoneMatrixFloatCount;
        std::array<float, kFloats> stock{};
        for (size_t node = 0; node < kPlasmaLauncherNodes; ++node)
            stock[node * kReachFpBoneMatrixFloatCount] = 1.0f;
        auto candidate = stock;
        candidate[12] = 3.5f;
        candidate[kReachFpBoneMatrixFloatCount + 10] = -2.0f;
        auto live = stock;
        bool commitCalled = false;
        const bool committed = ReachFpCommitGraphIfFinite(
            std::span<const float>{candidate}, kPlasmaLauncherNodes, [&]() {
                commitCalled = true;
                live = candidate;
                return true;
            });
        Check(committed && commitCalled && live == candidate &&
                  ReachFpPackedGraphFinite(
                      std::span<const float>{live}, kPlasmaLauncherNodes),
            "Reach validates and atomically commits a real 65-node live graph");

        const auto committedLive = live;
        auto nanCandidate = candidate;
        nanCandidate[3 * kReachFpBoneMatrixFloatCount + 6] =
            std::numeric_limits<float>::quiet_NaN();
        commitCalled = false;
        Check(!ReachFpCommitGraphIfFinite(
                  std::span<const float>{nanCandidate},
                  kPlasmaLauncherNodes, [&]() {
                      commitCalled = true;
                      live = nanCandidate;
                      return true;
                  }) &&
                  !commitCalled && live == committedLive,
            "Reach NaN rejection leaves the stock/live graph byte-identical");

        auto invalidRoot = candidate;
        invalidRoot[0] = 0.0f;
        commitCalled = false;
        Check(!ReachFpCommitGraphIfFinite(
                  std::span<const float>{invalidRoot},
                  kPlasmaLauncherNodes, [&]() {
                      commitCalled = true;
                      return true;
                  }) &&
                  !commitCalled &&
                  !ReachFpPackedGraphFinite(
                      std::span<const float>{candidate}.first(kFloats - 1),
                      kPlasmaLauncherNodes) &&
                  !ReachFpPackedGraphFinite(
                      std::span<const float>{candidate},
                      kReachFpMaxSourceNodeCount + 1),
            "Reach invalid roots, malformed spans, and over-120 graphs never commit");
    }

    {
        static_assert(kReachMainRenderViewAob.size() == 32);
        static_assert(kReachPlayerViewRenderAob.size() == 69);
        static_assert(kReachFrustumHelperAob.size() == 25);
        Check(std::string_view(kReachMainRenderViewBodySha256) ==
                  "95DF3EFFF9AC6EE29887D1272CCA8D7BF3E58F87041BAD8032107825B733FE89" &&
              std::string_view(kReachPlayerViewRenderBodySha256) ==
                  "2628D1189621EACED7C95A1F295815D70E7783054F1C3CBA46799F838CC33C60" &&
              kReachMainRenderViewBodySize == 515 &&
              kReachPlayerViewRenderBodySize == 2314,
            "Reach render candidate pins both exact body identities");

        constexpr float kReachTestIpdMeters = 0.064f;
        Check(std::isfinite(kReachWorldUnitsPerMeter) &&
              kReachWorldUnitsPerMeter > 0.0f &&
              std::fabs(kReachWorldUnitsPerMeter * 3.048f - 1.0f) < 1.0e-6f &&
              std::fabs(kReachTestIpdMeters * kReachWorldUnitsPerMeter -
                        0.020997375f) < 1.0e-7f,
            "Reach head translation and runtime IPD use the exact ten-foot world-unit conversion");

        constexpr uintptr_t motionBlurTestBase = 0x10000000u;
        Check(ReachMotionBlurSlotsMatchPinnedImage(
                  motionBlurTestBase, kReachRetailImageSize,
                  motionBlurTestBase + kReachMotionBlurScaleValueRva,
                  motionBlurTestBase + kReachMotionBlurMaxValueRva) &&
              !ReachMotionBlurSlotsMatchPinnedImage(
                  motionBlurTestBase, kReachRetailImageSize,
                  motionBlurTestBase + kReachMotionBlurMaxValueRva,
                  motionBlurTestBase + kReachMotionBlurScaleValueRva) &&
              !ReachMotionBlurSlotsMatchPinnedImage(
                  motionBlurTestBase, kReachRetailImageSize - 1,
                  motionBlurTestBase + kReachMotionBlurScaleValueRva,
                  motionBlurTestBase + kReachMotionBlurMaxValueRva) &&
              kReachMotionBlurMaxOverScaleDivideRva == 0x00287561 &&
              kReachMotionBlurScaledMaxDivideRva == 0x002875AD &&
              ReachMotionBlurSuppressionValuesValid(0.35f, 0.08f) &&
              ReachMotionBlurSuppressionValuesValid(0.35f, 0.0f) &&
              !ReachMotionBlurSuppressionValuesValid(0.0f, 0.0f) &&
              !ReachMotionBlurSuppressionValuesValid(1.0e-6f, 0.08f) &&
              !ReachMotionBlurSuppressionValuesValid(0.35f, -0.01f) &&
              !ReachMotionBlurSuppressionValuesValid(
                  0.35f, std::numeric_limits<float>::infinity()) &&
              !ReachMotionBlurSuppressionValuesValid(
                  std::numeric_limits<float>::quiet_NaN(), 0.08f),
            "Reach blur suppression pins the exact slots/divisions and keeps a positive scale with zero max");

        bool patchyFogPolicyExact = true;
        for (unsigned original = 0; original <= 0xFFu; ++original)
        {
            const uint8_t originalByte = static_cast<uint8_t>(original);
            const uint8_t suppressed =
                ReachPatchyFogSuppressedFlags(originalByte);
            patchyFogPolicyExact = patchyFogPolicyExact &&
                (suppressed & kReachPatchyFogSkipMask) != 0 &&
                (suppressed & static_cast<uint8_t>(~kReachPatchyFogSkipMask)) ==
                    (originalByte &
                     static_cast<uint8_t>(~kReachPatchyFogSkipMask));
            for (unsigned current = 0; current <= 0xFFu; ++current)
            {
                const uint8_t currentByte = static_cast<uint8_t>(current);
                const uint8_t restored = ReachPatchyFogRestoredFlags(
                    currentByte, originalByte);
                patchyFogPolicyExact = patchyFogPolicyExact &&
                    (restored & kReachPatchyFogSkipMask) ==
                        (originalByte & kReachPatchyFogSkipMask) &&
                    (restored &
                     static_cast<uint8_t>(~kReachPatchyFogSkipMask)) ==
                        (currentByte &
                         static_cast<uint8_t>(~kReachPatchyFogSkipMask));
            }
        }
        Check(patchyFogPolicyExact &&
              kReachPatchyFogGateTestRva == 0x0026CC59 &&
              kReachPatchyFogSkipJumpRva == 0x0026CC60 &&
              kReachPatchyFogCallRva == 0x0026CC65 &&
              kReachPatchyFogTargetRva == 0x0026EFEC &&
              kReachPatchyFogFlagsRva == 0x00CA0240 &&
              kReachPatchyFogSkipMask == 0x08,
            "Reach VR patchy-fog policy sets and restores only the exact proven skip bit");

        Check(kReachFpWeaponIkDecisionPreludeRva == 0x002B506E &&
              kReachFpWeaponIkDisableCompareRva == 0x002B507F &&
              kReachFpWeaponIkDisableBranchRva == 0x002B5085 &&
              kReachFpWeaponIkDisabledEpilogueRva == 0x002B52D1 &&
              kReachFpWeaponIkDisableNameRva == 0x009F2AD8 &&
              kReachFpWeaponIkDisableEntryRva == 0x00B3AEB8 &&
              kReachFpWeaponIkDisableValueRva == 0x04E38B61 &&
              kReachDebugBooleanType == 5,
            "Reach native weapon-IK bypass pins the exact named control and stock no-IK edge");

        std::array<uint8_t, kReachMainRenderViewAob.size()> exactMask{};
        exactMask.fill(0xFF);
        std::array<uint8_t, kReachMainRenderViewAob.size() * 2 + 1>
            repeatedMain{};
        std::memcpy(repeatedMain.data(), kReachMainRenderViewAob.data(),
                    kReachMainRenderViewAob.size());
        std::memcpy(repeatedMain.data() + kReachMainRenderViewAob.size() + 1,
                    kReachMainRenderViewAob.data(),
                    kReachMainRenderViewAob.size());
        Check(CountReachMaskedPattern(
                  repeatedMain.data(), repeatedMain.size(),
                  kReachMainRenderViewAob.data(), exactMask.data(),
                  kReachMainRenderViewAob.size()) == 2,
            "Reach render scanner counts every exact executable candidate");

        auto playerEntry = kReachPlayerViewRenderAob;
        playerEntry[49] = 0x12;
        playerEntry[50] = 0x34;
        playerEntry[51] = 0x56;
        playerEntry[52] = 0x78;
        Check(CountReachMaskedPattern(
                  playerEntry.data(), playerEntry.size(),
                  kReachPlayerViewRenderAob.data(),
                  kReachPlayerViewRenderAobMask.data(),
                  kReachPlayerViewRenderAob.size()) == 1,
            "Reach inner signature wildcards only the four cookie-displacement bytes");
        playerEntry[48] ^= 1;
        Check(CountReachMaskedPattern(
                  playerEntry.data(), playerEntry.size(),
                  kReachPlayerViewRenderAob.data(),
                  kReachPlayerViewRenderAobMask.data(),
                  kReachPlayerViewRenderAob.size()) == 0,
            "Reach inner signature rejects a changed non-cookie byte");

        auto frustumEntry = kReachFrustumHelperAob;
        std::array<uint8_t, kReachFrustumHelperAob.size()> frustumMask{};
        frustumMask.fill(0xFF);
        Check(CountReachMaskedPattern(
                  frustumEntry.data(), frustumEntry.size(),
                  kReachFrustumHelperAob.data(), frustumMask.data(),
                  kReachFrustumHelperAob.size()) == 1,
            "Reach production pattern pins the canonical 25-byte frustum entry");
        frustumEntry.back() ^= 1;
        Check(CountReachMaskedPattern(
                  frustumEntry.data(), frustumEntry.size(),
                  kReachFrustumHelperAob.data(), frustumMask.data(),
                  kReachFrustumHelperAob.size()) == 0,
            "Reach production pattern checks the byte omitted by the historical observer");
        Check(CountReachMaskedPattern(
                  nullptr, 1, kReachMainRenderViewAob.data(), exactMask.data(),
                  kReachMainRenderViewAob.size()) == 0 &&
              CountReachMaskedPattern(
                  repeatedMain.data(), repeatedMain.size(), nullptr,
                  exactMask.data(), kReachMainRenderViewAob.size()) == 0,
            "Reach masked scanning fails closed on invalid buffers");

        auto fpCameraEntry = kReachFpCameraRebuildAob;
        fpCameraEntry[22] = 0x12;
        fpCameraEntry[23] = 0x34;
        fpCameraEntry[24] = 0x56;
        fpCameraEntry[25] = 0x78;
        Check(CountReachMaskedPattern(
                  fpCameraEntry.data(), fpCameraEntry.size(),
                  kReachFpCameraRebuildAob.data(),
                  kReachFpCameraRebuildAobMask.data(),
                  kReachFpCameraRebuildAob.size()) == 1,
            "Reach FP camera signature masks only the LEA displacement");
        fpCameraEntry.back() ^= 1;
        Check(CountReachMaskedPattern(
                  fpCameraEntry.data(), fpCameraEntry.size(),
                  kReachFpCameraRebuildAob.data(),
                  kReachFpCameraRebuildAobMask.data(),
                  kReachFpCameraRebuildAob.size()) == 0,
            "Reach FP camera signature rejects a changed fixed byte");
        auto fpUploadEntry = kReachFpCameraUploadAob;
        std::array<uint8_t, kReachFpCameraUploadAob.size()> fpUploadMask{};
        fpUploadMask.fill(0xFF);
        Check(CountReachMaskedPattern(
                  fpUploadEntry.data(), fpUploadEntry.size(),
                  kReachFpCameraUploadAob.data(), fpUploadMask.data(),
                  kReachFpCameraUploadAob.size()) == 1,
            "Reach FP camera uploader signature is exact");
        fpUploadEntry[19] ^= 1;
        Check(CountReachMaskedPattern(
                  fpUploadEntry.data(), fpUploadEntry.size(),
                  kReachFpCameraUploadAob.data(), fpUploadMask.data(),
                  kReachFpCameraUploadAob.size()) == 0,
            "Reach FP camera uploader rejects a changed fixed byte");

        struct ReachFpNestedSelectorInput
        {
            uintptr_t moduleBase;
            size_t moduleSize;
            uintptr_t stackTop;
            uintptr_t workspaceCallback;
            uintptr_t fpView;
        };
        const uintptr_t fpModuleBase = 0x10000000u;
        const ReachFpNestedSelectorInput validFpNested{
            fpModuleBase,
            kReachRetailImageSize,
            fpModuleBase + kReachFpCameraWorkspaceRva,
            fpModuleBase + kReachFpCameraWorkspaceCallbackRva,
            fpModuleBase + kReachFpCameraViewRva};
        const auto selectFpNested =
            [](const ReachFpNestedSelectorInput& input)
        {
            return SelectReachFpCameraNestedWorkspace(
                input.moduleBase, input.moduleSize, input.stackTop,
                input.workspaceCallback, input.fpView);
        };
        const auto fpNestedRejects =
            [&validFpNested, &selectFpNested](auto mutate)
        {
            auto candidate = validFpNested;
            mutate(candidate);
            return selectFpNested(candidate) == 0;
        };
        Check(selectFpNested(validFpNested) == validFpNested.stackTop,
            "Reach FP camera selector accepts the exact nested workspace, callback, and view");
        const uintptr_t selectedFpNested = selectFpNested(validFpNested);
        Check(selectedFpNested + kReachSecondaryDerivedOffset ==
                  fpModuleBase + kReachFpCameraWorkspaceRva + 0x1E4 &&
              kReachSecondaryDerivedOffset + kReachDerivedBlockSize <=
                  kReachFpCameraWorkspaceCallbackOffset,
            "Reach FP camera selector targets nested+0x1E4 without crossing the callback");
        Check(kReachFpCameraWorkspaceCallbackOffset + sizeof(uintptr_t) ==
                  kReachRenderScopeSnapshotSize &&
              kReachFpCameraWorkspaceRva <=
                  kReachRetailImageSize - kReachRenderScopeSnapshotSize,
            "Reach FP camera callback and full snapshot stay inside the fixed nested workspace range");
        Check(
            fpNestedRejects([](auto& v) {
                v.moduleBase = 0;
                v.stackTop = kReachFpCameraWorkspaceRva;
                v.workspaceCallback = kReachFpCameraWorkspaceCallbackRva;
                v.fpView = kReachFpCameraViewRva;
            }) &&
            fpNestedRejects([](auto& v) { v.moduleSize = 0; }) &&
            fpNestedRejects([](auto& v) {
                v.moduleSize = kReachRetailImageSize - 1;
            }) &&
            fpNestedRejects([](auto& v) {
                v.moduleSize = kReachRetailImageSize + 1;
            }) &&
            fpNestedRejects([](auto& v) { --v.stackTop; }) &&
            fpNestedRejects([](auto& v) { ++v.stackTop; }) &&
            fpNestedRejects([](auto& v) { v.workspaceCallback = 0; }) &&
            fpNestedRejects([](auto& v) { --v.workspaceCallback; }) &&
            fpNestedRejects([](auto& v) { ++v.workspaceCallback; }) &&
            fpNestedRejects([](auto& v) { v.fpView = 0; }) &&
            fpNestedRejects([](auto& v) { --v.fpView; }) &&
            fpNestedRejects([](auto& v) { ++v.fpView; }) &&
            fpNestedRejects([](auto& v) {
                v.stackTop =
                    v.moduleBase + kReachDefaultWorkspaceRva;
            }) &&
            fpNestedRejects([](auto& v) {
                v.moduleBase =
                    std::numeric_limits<uintptr_t>::max() -
                    kReachFpCameraWorkspaceRva + 2;
                v.stackTop = v.moduleBase + kReachFpCameraWorkspaceRva;
                v.workspaceCallback =
                    v.moduleBase + kReachFpCameraWorkspaceCallbackRva;
                v.fpView = v.moduleBase + kReachFpCameraViewRva;
            }),
            "Reach FP camera selector fails closed for every invalid identity component");
        const uintptr_t fpBoundaryBase =
            std::numeric_limits<uintptr_t>::max() -
            kReachFpCameraWorkspaceRva;
        const ReachFpNestedSelectorInput boundaryFpNested{
            fpBoundaryBase,
            kReachRetailImageSize,
            fpBoundaryBase + kReachFpCameraWorkspaceRva,
            fpBoundaryBase + kReachFpCameraWorkspaceCallbackRva,
            fpBoundaryBase + kReachFpCameraViewRva};
        Check(selectFpNested(boundaryFpNested) ==
                  std::numeric_limits<uintptr_t>::max(),
            "Reach FP camera selector accepts the last non-overflowing nested workspace");

        ReachRenderCandidateProof proof =
            CompleteReachRenderCandidateProof();
        Check(ReachRenderCandidateProofComplete(proof),
            "Reach render proof requires every exact static preflight gate");
        const auto proofRejects = [&proof](auto mutate)
        {
            auto candidate = proof;
            mutate(candidate);
            return !ReachRenderCandidateProofComplete(candidate);
        };
        Check(proofRejects([](auto& p) { p.retailIdentity = false; }) &&
              proofRejects([](auto& p) { p.mainRenderViewMatchCount = 0; }) &&
              proofRejects([](auto& p) { p.mainRenderViewMatchCount = 2; }) &&
              proofRejects([](auto& p) { p.mainRenderViewAtExpectedRva = false; }) &&
              proofRejects([](auto& p) { p.mainRenderViewBodyHash = false; }) &&
              proofRejects([](auto& p) { p.playerViewRenderMatchCount = 0; }) &&
              proofRejects([](auto& p) { p.playerViewRenderMatchCount = 2; }) &&
              proofRejects([](auto& p) { p.playerViewRenderAtExpectedRva = false; }) &&
              proofRejects([](auto& p) { p.playerViewRenderBodyHash = false; }) &&
              proofRejects([](auto& p) { p.frustumHelperMatchCount = 0; }) &&
              proofRejects([](auto& p) { p.frustumHelperMatchCount = 2; }) &&
               proofRejects([](auto& p) { p.frustumHelperAtExpectedRva = false; }) &&
               proofRejects([](auto& p) { p.frustumHelperExecutableRange = false; }) &&
               proofRejects([](auto& p) { p.fpCameraRebuildMatchCount = 0; }) &&
               proofRejects([](auto& p) { p.fpCameraRebuildMatchCount = 2; }) &&
               proofRejects([](auto& p) { p.fpCameraRebuildAtExpectedRva = false; }) &&
               proofRejects([](auto& p) { p.fpCameraRebuildBodyHash = false; }) &&
               proofRejects([](auto& p) { p.fpCameraUploadMatchCount = 0; }) &&
               proofRejects([](auto& p) { p.fpCameraUploadMatchCount = 2; }) &&
               proofRejects([](auto& p) { p.fpCameraUploadAtExpectedRva = false; }) &&
               proofRejects([](auto& p) { p.fpCameraUploadBodyHash = false; }) &&
               proofRejects([](auto& p) { p.fpCameraWrapperBodyHashes = false; }) &&
               proofRejects([](auto& p) { p.exactFpCameraFlowEdges = false; }) &&
               proofRejects([](auto& p) { p.exactOuterCallerEdges = false; }) &&
              proofRejects([](auto& p) { p.exactInnerCallerEdge = false; }) &&
              proofRejects([](auto& p) { p.fixedDataRanges = false; }),
            "Reach render proof fails closed when any identity gate is absent");

        const ReachModuleEpoch proofEpoch{0x10000000u, 1};
        ReachPreflightPublication preflightPublication;
        Check(preflightPublication.Publish(proofEpoch, proof),
            "Reach preflight publication accepts one complete exact proof");
        const ReachPreflightToken preflight =
            preflightPublication.Get(proofEpoch);
        auto incompleteProof = proof;
        incompleteProof.frustumHelperMatchCount = 0;
        ReachPreflightPublication incompletePublication;
        Check(preflight.Complete() &&
              IsPreflightCurrent(preflight) &&
              ReachSameModuleEpoch(preflight.Epoch(), proofEpoch) &&
              !incompletePublication.Publish(
                  proofEpoch, incompleteProof) &&
              !incompletePublication.Get(proofEpoch).Complete(),
            "Reach preflight authorization is publication-bound and requires the full proof");
        ReachPreflightPublication replacementPublication;
        Check(replacementPublication.Publish(proofEpoch, proof),
            "Reach preflight replacement test begins with one current proof");
        const ReachPreflightToken replacedPreflight =
            replacementPublication.Get(proofEpoch);
        Check(replacedPreflight.Complete() &&
              replacementPublication.IsCurrent(replacedPreflight) &&
              !replacementPublication.Publish(proofEpoch, incompleteProof) &&
              !replacementPublication.HasCurrent() &&
              !replacementPublication.IsCurrent(replacedPreflight),
            "A failed preflight replacement revokes prior readiness and copied tokens");

        ReachDisplaySurfaceProof displayProof =
            CompleteReachDisplaySurfaceProof(proofEpoch, preflight);
        Check(ReachDisplaySurfaceProofComplete(displayProof),
            "Reach display proof accepts exact buffer0 shape and record0 structural continuity");
        const auto displayRejects = [&displayProof](auto mutate)
        {
            auto candidate = displayProof;
            mutate(candidate);
            return !ReachDisplaySurfaceProofComplete(candidate);
        };
        Check(displayRejects([](auto& p) { p.preflight = {}; }) &&
              displayRejects([](auto& p) {
                  ++p.continuity.epoch.generation;
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.resourceRevision = 0;
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.lifecycleSerial = 0;
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.lifecycleSerial =
                      std::numeric_limits<uint64_t>::max();
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.swapchainIdentity = 0;
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.buffer0Identity = 0;
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.surfaceArrayIdentity = 0;
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.record0RtvIdentity = 0;
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.record0SrvIdentity = 0;
              }) &&
              displayRejects([](auto& p) {
                  ++p.continuity.selectedRtvIdentity;
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.deviceIdentity = 0;
              }) &&
              displayRejects([](auto& p) {
                  ++p.continuity.immediateContextIdentity;
              }) &&
              displayRejects([](auto& p) {
                  ++p.continuity.eyeResourceIdentities[0];
              }) &&
              displayRejects([](auto& p) {
                  ++p.continuity.eyeResourceIdentities[1];
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.specializationCount = 3;
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.selectedSpecialization = 1;
              }) &&
              displayRejects([](auto& p) {
                  p.continuity.teardownRequested = true;
              }) &&
              displayRejects([](auto& p) {
                  p.eyeResourceIdentities[1] =
                      p.eyeResourceIdentities[0];
              }) &&
              displayRejects([](auto& p) {
                  p.eyeResourceIdentities[0] =
                      p.continuity.buffer0Identity;
              }) &&
              displayRejects([](auto& p) {
                  p.immediateContextIdentity = 0;
              }) &&
              displayRejects([](auto& p) { p.source.width = 0; }) &&
              displayRejects([](auto& p) {
                  p.source.format = 0;
              }) &&
              displayRejects([](auto& p) { ++p.eyes[0].width; }) &&
              displayRejects([](auto& p) { ++p.eyes[1].sampleCount; }) &&
              displayRejects([](auto& p) { p.readyEyeMask = 1; }) &&
              displayRejects([](auto& p) {
                  p.engineSwapchainMatchesPresent = false;
              }) &&
              displayRejects([](auto& p) {
                  p.selectedRtvMatchesRecord0 = false;
              }) &&
              displayRejects([](auto& p) {
                  p.swapchainContract = false;
              }) &&
              displayRejects([](auto& p) { p.sameDevice = false; }) &&
              displayRejects([](auto& p) {
                  p.immediateContext = false;
              }),
            "Reach display proof rejects stale identity, aliasing, partial eyes, and every copy-shape/context mismatch");

        ReachDirectCopyGate displayGate;
        const ReachPreparedFrameToken displayFrame =
            ReachPreparedFrameToken::Create(proofEpoch, 11, true);
        Check(displayGate.AdvanceEpoch(proofEpoch) &&
              displayGate.Publish(displayProof),
            "Reach direct-copy gate admits one monotonic cold resource proof");
        const ReachDirectCopyToken firstCopy = displayGate.Prepare(
            displayFrame, displayProof.continuity);
        Check(firstCopy.Ready() && displayGate.IsCurrent(
                  firstCopy, displayProof.continuity),
            "A current display proof mints one live direct-copy token");
        const auto liveContinuityRejects =
            [&displayGate, &firstCopy, &displayFrame, &displayProof](
                auto mutate)
        {
            auto live = displayProof.continuity;
            mutate(live);
            return !displayGate.IsCurrent(firstCopy, live) &&
                !displayGate.Prepare(displayFrame, live).Ready();
        };
        Check(liveContinuityRejects([](auto& c) {
                  c.epoch.moduleBase += 0x10000;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.epoch.generation;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.resourceRevision;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.lifecycleSerial;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.swapchainIdentity;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.buffer0Identity;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.surfaceArrayIdentity;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.record0RtvIdentity;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.record0SrvIdentity;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.selectedRtvIdentity;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.deviceIdentity;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.immediateContextIdentity;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.eyeResourceIdentities[0];
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.eyeResourceIdentities[1];
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.specializationCount;
              }) &&
              liveContinuityRejects([](auto& c) {
                  ++c.selectedSpecialization;
              }) &&
              liveContinuityRejects([](auto& c) {
                  c.teardownRequested = true;
              }),
            "Every live Reach display-continuity field invalidates copied and newly prepared tokens when it drifts");
        ReachDisplaySurfaceProof invalidReplacement =
            CompleteReachDisplaySurfaceProof(
                proofEpoch, preflight, 2);
        invalidReplacement.sameDevice = false;
        Check(!displayGate.Publish(invalidReplacement) &&
              !displayGate.Ready() &&
              !displayGate.IsCurrent(
                  firstCopy, displayProof.continuity),
            "A failed display replacement revokes prior readiness and copied tokens");
        auto changedContinuity = displayProof.continuity;
        ++changedContinuity.record0RtvIdentity;
        Check(!displayGate.IsCurrent(firstCopy, changedContinuity) &&
              displayGate.Invalidate(proofEpoch) &&
              !displayGate.IsCurrent(
                  firstCopy, displayProof.continuity) &&
              !displayGate.Publish(displayProof),
            "Resize or live view drift invalidates copied tokens and forbids resource-revision replay");
        ReachDisplaySurfaceProof nextDisplayProof =
            CompleteReachDisplaySurfaceProof(
                proofEpoch, preflight, 2);
        const ReachPreparedFrameToken nextDisplayFrame =
            ReachPreparedFrameToken::Create(proofEpoch, 12, true);
        Check(displayGate.Publish(nextDisplayProof),
            "A higher same-epoch resource revision may re-arm after ResizeBuffers or resident title ambiguity");
        const ReachDirectCopyToken nextCopy = displayGate.Prepare(
            nextDisplayFrame, nextDisplayProof.continuity);
        Check(nextCopy.Ready() &&
              nextCopy.PreparedFrameSerial() == 12 &&
              nextCopy.ResourceRevision() == 2 &&
              !displayGate.IsCurrent(
                  firstCopy, nextDisplayProof.continuity),
            "A new resource publication invalidates its predecessor");

        ReachPreflightPublication differentPublication;
        Check(differentPublication.Publish(proofEpoch, proof) &&
              !differentPublication.IsCurrent(preflight),
            "A Reach preflight token cannot cross publication owners");
        const uint64_t firstPublicationNonce =
            preflight.PublicationNonce();
        Check(preflightPublication.Publish(proofEpoch, proof),
            "The same module epoch may receive a newer cold-proof publication");
        const ReachPreflightToken reissuedPreflight =
            preflightPublication.Get(proofEpoch);
        Check(reissuedPreflight.Complete() &&
              IsPreflightCurrent(reissuedPreflight) &&
              reissuedPreflight.PublicationNonce() >
                  firstPublicationNonce &&
              preflight.Complete() &&
              !IsPreflightCurrent(preflight) &&
              !ReachDisplaySurfaceProofComplete(nextDisplayProof) &&
              !displayGate.Ready() &&
              !displayGate.IsCurrent(
                  nextCopy, nextDisplayProof.continuity) &&
              !displayGate.Publish(nextDisplayProof),
            "A newer preflight nonce rejects copied-token replay and stale display publication");
        ReachDisplaySurfaceProof reissuedDisplayProof =
            CompleteReachDisplaySurfaceProof(
                proofEpoch, reissuedPreflight, 3);
        const ReachPreparedFrameToken reissuedDisplayFrame =
            ReachPreparedFrameToken::Create(proofEpoch, 13, true);
        Check(displayGate.Publish(reissuedDisplayProof),
            "A current preflight nonce can publish a newer display revision");
        const ReachDirectCopyToken reissuedCopy = displayGate.Prepare(
            reissuedDisplayFrame, reissuedDisplayProof.continuity);
        Check(reissuedCopy.Ready() &&
              displayGate.IsCurrent(
                  reissuedCopy, reissuedDisplayProof.continuity) &&
              displayGate.Teardown(proofEpoch) &&
              !displayGate.IsCurrent(
                  reissuedCopy, reissuedDisplayProof.continuity) &&
              !displayGate.AdvanceEpoch(proofEpoch) &&
              displayGate.AdvanceEpoch({proofEpoch.moduleBase, 2}) &&
              displayGate.Teardown({proofEpoch.moduleBase, 2}),
            "Reach direct-copy teardown rejects old resource and module-generation replay");

        ReachRenderFreshnessGate freshness;
        const ReachModuleEpoch freshnessEpoch{0x10000000u, 7};
        Check(freshness.AdvanceEpoch(freshnessEpoch) &&
              !freshness.Observe(
                  100, freshnessEpoch, 1, true, true).Stable() &&
              !freshness.Observe(
                  499, freshnessEpoch, 2, true, true).Stable() &&
              !freshness.Observe(
                  898, freshnessEpoch, 3, true, true).Stable() &&
              !freshness.Observe(
                  1100, freshnessEpoch, 4, true, true).Stable() &&
              freshness.CurrentSpanMs() == 1000,
            "Reach production freshness arms only on a new transaction after one second");
        const ReachFreshCameraToken stableFreshness =
            freshness.Observe(1101, freshnessEpoch, 5, true, true);
        Check(stableFreshness.Stable() &&
              freshness.IsCurrent(stableFreshness) &&
              stableFreshness.PreparedFrameSerial() == 5,
            "Reach stable freshness is bound to the exact observed frame serial");
        const ReachModuleEpoch staleFreshnessEpoch{0x10000000u, 6};
        const ReachFreshCameraToken nextFreshness =
            freshness.Observe(1200, freshnessEpoch, 6, true, true);
        Check(!freshness.IsCurrent(stableFreshness) &&
              nextFreshness.Stable() && freshness.IsCurrent(nextFreshness) &&
              !freshness.AdvanceEpoch(staleFreshnessEpoch) &&
              !freshness.Consume(
                  nextFreshness,
                  ReachPreparedFrameToken::Create(
                      freshnessEpoch, 6, true),
                  1700) &&
              !freshness.IsCurrent(nextFreshness) &&
              !freshness.Observe(
                  1700, freshnessEpoch, 7, true, true).Stable() &&
              freshness.TransactionCount() == 1 &&
              !freshness.Teardown(staleFreshnessEpoch),
            "Reach freshness invalidates saved tokens on a new observation or continuity gap");
        Check(freshness.Teardown(freshnessEpoch) &&
              !freshness.AdvanceEpoch(freshnessEpoch) &&
              freshness.AdvanceEpoch({0x10000000u, 8}) &&
              !freshness.Observe(
                  1800, {0x10000000u, 8}, 1, false, true).Stable() &&
              freshness.TransactionCount() == 0 &&
              freshness.Teardown({0x10000000u, 8}) &&
              !freshness.AdvanceEpoch({0x10000000u, 8}) &&
              freshness.AdvanceEpoch({0x10000000u, 9}),
            "Reach freshness rejects stale epochs, gaps, non-normal owners, and generation replay");
    }

    {
        constexpr uintptr_t moduleBase = 0x10000000;
        ReachOuterRenderInput outer{};
        outer.moduleBase = moduleBase;
        outer.moduleSize = kReachRetailImageSize;
        outer.returnAddress = moduleBase + kReachNormalOuterReturnRva;
        outer.workspace = moduleBase + kReachDefaultWorkspaceRva;
        outer.playerView = moduleBase + kReachPlayerViewArrayRva;
        outer.playerWindowIndex = 0;
        // Retail initializes the camera-stack depth to -1. The normal
        // top-level push increments that sentinel to slot/depth zero.
        outer.cameraStackDepthBefore = -1;
        outer.nowMs = 1101;
        const ReachModuleEpoch epoch{moduleBase, 9};
        const ReachRenderCandidateProof outerProof =
            CompleteReachRenderCandidateProof();
        ReachPreflightPublication outerPreflightPublication;
        Check(outerPreflightPublication.Publish(epoch, outerProof),
            "Reach outer preflight publishes for the selected module epoch");
        outer.preflight = outerPreflightPublication.Get(epoch);
        ReachRenderFreshnessGate ownerFreshness;
        Check(ownerFreshness.AdvanceEpoch(epoch) &&
              !ownerFreshness.Observe(
                  100, epoch, 38, true, true).Stable() &&
              !ownerFreshness.Observe(
                  400, epoch, 39, true, true).Stable() &&
              !ownerFreshness.Observe(
                  700, epoch, 40, true, true).Stable() &&
              !ownerFreshness.Observe(
                  1000, epoch, 41, true, true).Stable(),
            "Reach owner freshness starts from exact prepared-frame observations");
        outer.freshCamera = ownerFreshness.Observe(
            1101, epoch, 42, true, true);
        outer.preparedFrame = ReachPreparedFrameToken::Create(
            epoch, 42, true);
        const ReachModuleEpoch wrongPreflightEpoch{moduleBase, 10};
        ReachPreflightPublication wrongPreflightPublication;
        Check(wrongPreflightPublication.Publish(
                  wrongPreflightEpoch, outerProof),
            "Reach mismatch test publishes a distinct module generation");
        const ReachPreflightToken wrongPreflight =
            wrongPreflightPublication.Get(wrongPreflightEpoch);

        Check(ClassifyReachOuterRenderCaller(
                  moduleBase, kReachRetailImageSize,
                  moduleBase + kReachNormalOuterReturnRva) ==
                  ReachOuterRenderCaller::NormalPlayer &&
              ClassifyReachOuterRenderCaller(
                  moduleBase, kReachRetailImageSize,
                  moduleBase + kReachScreenshotOuterReturnRva) ==
                  ReachOuterRenderCaller::ScreenshotTileBloom &&
              ClassifyReachOuterRenderCaller(
                  moduleBase, kReachRetailImageSize, moduleBase + 1) ==
                  ReachOuterRenderCaller::Unknown &&
              ClassifyReachOuterRenderCaller(
                  moduleBase, kReachRetailImageSize - 1,
                  moduleBase + kReachNormalOuterReturnRva) ==
                  ReachOuterRenderCaller::Unknown,
            "Reach outer routing distinguishes normal, screenshot, and unknown callers exactly");

        const auto outerRejects = [&outer](auto mutate)
        {
            auto candidate = outer;
            mutate(candidate);
            return !ReachNormalOuterInputMatches(candidate);
        };
        Check(ReachNormalOuterInputMatches(outer) &&
              outerRejects([](auto& v) { v.preflight = {}; }) &&
              outerRejects([](auto& v) { v.freshCamera = {}; }) &&
              outerRejects([](auto& v) { v.preparedFrame = {}; }) &&
              outerRejects([&wrongPreflight](auto& v) {
                  v.preflight = wrongPreflight;
              }) &&
              outerRejects([](auto& v) {
                  v.preparedFrame = ReachPreparedFrameToken::Create(
                      v.preflight.Epoch(), 43, true);
              }) &&
              outerRejects([](auto& v) { v.teardownRequested = true; }) &&
              outerRejects([](auto& v) { v.nowMs = 0; }) &&
              outerRejects([](auto& v) { ++v.moduleBase; }) &&
              outerRejects([](auto& v) { ++v.playerWindowIndex; }) &&
              outerRejects([](auto& v) { v.cameraStackDepthBefore = -2; }) &&
              outerRejects([](auto& v) { v.cameraStackDepthBefore = 3; }) &&
              outerRejects([](auto& v) { ++v.workspace; }) &&
              outerRejects([](auto& v) { ++v.playerView; }) &&
              outerRejects([](auto& v) {
                  v.returnAddress = v.moduleBase +
                      kReachScreenshotOuterReturnRva;
              }),
            "Only the exact fresh slot-zero normal owner can mint a Reach token");

        ReachRenderOwnerGate owner;
        Check(!owner.TryBegin(outer, ownerFreshness) &&
              ownerFreshness.IsCurrent(outer.freshCamera) &&
              owner.AdvanceEpoch(epoch) &&
              !owner.AdvanceEpoch(epoch) &&
              !owner.AdvanceEpoch({moduleBase, 8}) &&
              owner.TryBegin(outer, ownerFreshness) &&
              !ownerFreshness.IsCurrent(outer.freshCamera) &&
              owner.Token().Active() &&
              owner.Token().PreparedFrameSerial() == 42 &&
              !owner.TryBegin(outer, ownerFreshness),
            "Reach owner gate requires an explicit monotonic epoch and rejects nesting");
        const ReachRenderOwnerToken ownerToken = owner.Token();
        Check(!owner.Finish(ownerToken, {}) && owner.IsCurrent(ownerToken),
            "A Reach owner cannot finish without a bound clean-completion token");

        ReachInnerRenderInput inner{};
        inner.returnAddress = moduleBase + kReachPlayerViewRenderReturnRva;
        inner.playerView = outer.playerView;
        inner.activeView = outer.playerView;
        inner.cameraStackDepth = 0;
        inner.topWorkspace = outer.workspace;
        inner.workspaceCallback = moduleBase + kReachCameraStackCallbackRva;
        inner.renderCameraOwner =
            outer.playerView + kReachPlayerViewCameraStateOffset;
        inner.selectedSpecialization = 0;
        inner.primaryCameraValid = true;
        inner.secondaryCameraValid = true;
        inner.preparedFrame = outer.preparedFrame;
        ReachDirectCopyGate directCopyGate;
        ReachDisplaySurfaceProof innerDisplayProof =
            CompleteReachDisplaySurfaceProof(
                epoch, outer.preflight);
        Check(directCopyGate.AdvanceEpoch(epoch) &&
              directCopyGate.Publish(innerDisplayProof),
            "Reach inner admission begins with a live cold display-resource owner");
        inner.displayContinuity = innerDisplayProof.continuity;
        inner.directCopy = directCopyGate.Prepare(
            inner.preparedFrame, inner.displayContinuity);

        const auto innerRejects =
            [&owner, &inner, &directCopyGate](auto mutate)
        {
            auto candidate = inner;
            mutate(candidate);
            return !ReachInnerScopeMatches(
                owner, owner.Token(), directCopyGate, candidate);
        };
        Check(ReachInnerScopeMatches(
                  owner, owner.Token(), directCopyGate, inner) &&
              innerRejects([](auto& v) { ++v.returnAddress; }) &&
              innerRejects([](auto& v) { v.preparedFrame = {}; }) &&
              innerRejects([](auto& v) {
                  v.preparedFrame = ReachPreparedFrameToken::Create(
                      v.directCopy.Epoch(), 43, true);
              }) &&
              innerRejects([](auto& v) { v.directCopy = {}; }) &&
              innerRejects([&directCopyGate](auto& v) {
                  v.directCopy = directCopyGate.Prepare(
                      ReachPreparedFrameToken::Create(
                          v.preparedFrame.Epoch(), 43, true),
                      v.displayContinuity);
              }) &&
              innerRejects([](auto& v) {
                  ++v.displayContinuity.surfaceArrayIdentity;
              }) &&
              innerRejects([](auto& v) { ++v.playerView; }) &&
              innerRejects([](auto& v) { ++v.activeView; }) &&
              innerRejects([](auto& v) { ++v.cameraStackDepth; }) &&
              innerRejects([](auto& v) { ++v.topWorkspace; }) &&
              innerRejects([](auto& v) { ++v.workspaceCallback; }) &&
              innerRejects([](auto& v) { ++v.renderCameraOwner; }) &&
              innerRejects([](auto& v) { ++v.selectedSpecialization; }) &&
              innerRejects([](auto& v) { v.primaryCameraValid = false; }) &&
              innerRejects([](auto& v) { v.secondaryCameraValid = false; }) &&
              innerRejects([](auto& v) { v.teardownRequested = true; }),
            "Reach inner admission checks the exact return edge, epoch, stack, camera, and target");

        const ReachPreflightToken incompletePreflight{};
        auto wrongInner = inner;
        ++wrongInner.returnAddress;
        Check(SelectReachRenderAction(
                  false, outer.preflight, owner, ownerToken,
                  directCopyGate, inner) ==
                  ReachRenderAction::StockOnce &&
              SelectReachRenderAction(
                  true, incompletePreflight, owner, ownerToken,
                  directCopyGate, inner) ==
                  ReachRenderAction::StockOnce &&
              SelectReachRenderAction(
                  true, outer.preflight, owner, ownerToken,
                  directCopyGate, wrongInner) ==
                  ReachRenderAction::StockOnce &&
              SelectReachRenderAction(
                  true, outer.preflight, owner, ownerToken,
                  directCopyGate, inner) ==
                  ReachRenderAction::StereoTransaction,
            "Reach action selection consumes bound proof, owner, and inner-scope inputs");
        Check(SelectReachRenderAction(
                  ReachAdapter_RuntimeHooksPermitted(), outer.preflight,
                  owner, ownerToken, directCopyGate, inner) ==
                  ReachRenderAction::StockOnce,
            "The adapter hard gate keeps an otherwise complete Reach scope stock-once");
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
        ReachLoadedImagePreflight invalidLoadedImage{};
        ReachLoadedImageModulePin invalidModulePin;
        ReachRenderCandidate_ColdPoll(0, 0, 0, false);
        Check(ReachRenderCandidate_Compiled() &&
              !ReachRenderCandidate_RuntimeHooksEnabled() &&
              !ReachRender_RunLoadedImagePreflight(
                  0, kReachRetailImageSize,
                  invalidLoadedImage, invalidModulePin) &&
              !invalidModulePin.Valid() &&
              invalidLoadedImage.failure ==
                  ReachLoadedImageFailure::InvalidInput &&
              !ReachRenderCandidate_GetPreflight(epoch).Complete() &&
              !ReachRenderCandidate_IsPreflightCurrent({}) &&
              ReachRenderCandidate_SelectAction(
                  outer.preflight, owner, ownerToken,
                  directCopyGate, inner) ==
                  ReachRenderAction::StockOnce,
            "The compiled DLL-facing Reach wrapper remains hard-disabled");
#endif

        ReachRollbackGate rollback;
        Check(rollback.Bind(owner, ownerToken) &&
              rollback.BeginFirstPass(ownerToken) &&
              rollback.NeedsRollback() &&
              !rollback.BeginFinalPass(ownerToken) &&
              rollback.MarkFirstPassRestored(ownerToken) &&
              !rollback.NeedsRollback() &&
              rollback.BeginFinalPass(ownerToken) &&
              rollback.NeedsRollback(),
            "Reach cleanup state tracks dirty first and final passes independently");
        const ReachCleanupToken completedCleanup =
            rollback.MarkFinalPassRestoredAndComplete(ownerToken);
        Check(completedCleanup.Valid() && rollback.Finished() &&
              owner.Finish(ownerToken, completedCleanup) &&
              !owner.Token().Active() &&
              owner.LastCompletedSerial() == 42 &&
              SelectReachRenderAction(
                  true, outer.preflight, owner, ownerToken,
                  directCopyGate, inner) ==
                  ReachRenderAction::StockOnce,
            "Owner completion requires clean rollback and invalidates every copied owner token");

        outer.freshCamera = ownerFreshness.Observe(
            1200, epoch, 43, true, true);
        outer.nowMs = 1200;
        outer.preparedFrame = ReachPreparedFrameToken::Create(
            epoch, 43, true);
        Check(owner.TryBegin(outer, ownerFreshness),
            "A fresh prepared-frame serial can mint the next Reach owner token");
        const ReachRenderOwnerToken abortedToken = owner.Token();
        ReachRollbackGate abortedRollback;
        Check(abortedRollback.Bind(owner, abortedToken) &&
              abortedRollback.BeginFirstPass(abortedToken) &&
              abortedRollback.NeedsRollback() &&
              !owner.Abort(abortedToken, {}) &&
              abortedRollback.MarkDirtyPassRestoredForAbort(abortedToken),
            "A dirty aborted pass must explicitly transition through restored state");
        const ReachCleanupToken abortedCleanup =
            abortedRollback.AbortClean(abortedToken);
        Check(abortedCleanup.Valid() && abortedRollback.Aborted() &&
              owner.Abort(abortedToken, abortedCleanup) &&
              !owner.TryBegin(outer, ownerFreshness),
            "A clean abort consumes its exact prepared-frame serial");
        Check(owner.Teardown(epoch) && ownerFreshness.Teardown(epoch) &&
              !owner.AdvanceEpoch(epoch) &&
              !owner.AdvanceEpoch({moduleBase, 8}),
            "Teardown retains the generation high-water mark against stale replay");
        const ReachModuleEpoch nextEpoch{moduleBase, 10};
        Check(outerPreflightPublication.Publish(nextEpoch, outerProof),
            "A newer module generation receives a newer preflight publication");
        outer.preflight = outerPreflightPublication.Get(nextEpoch);
        Check(owner.AdvanceEpoch(nextEpoch) &&
              ownerFreshness.AdvanceEpoch(nextEpoch) &&
              !ownerFreshness.Observe(
                  100, nextEpoch, 39, true, true).Stable() &&
              !ownerFreshness.Observe(
                  400, nextEpoch, 40, true, true).Stable() &&
              !ownerFreshness.Observe(
                  700, nextEpoch, 41, true, true).Stable() &&
              !ownerFreshness.Observe(
                  1000, nextEpoch, 42, true, true).Stable(),
            "A newer Reach generation starts fresh owner and freshness state");
        outer.freshCamera = ownerFreshness.Observe(
            1101, nextEpoch, 43, true, true);
        outer.nowMs = 1101;
        outer.preparedFrame = ReachPreparedFrameToken::Create(
            nextEpoch, 43, true);
        Check(owner.TryBegin(outer, ownerFreshness),
            "A newer Reach module generation starts a fresh serial namespace");
        const ReachRenderOwnerToken nextToken = owner.Token();
        ReachRollbackGate nextRollback;
        Check(nextRollback.Bind(owner, nextToken),
            "The next generation binds cleanup to its live owner");
        const ReachCleanupToken nextAbort =
            nextRollback.AbortClean(nextToken);
        Check(!owner.Abort(nextToken, abortedCleanup) &&
              owner.IsCurrent(nextToken) &&
              owner.Abort(nextToken, nextAbort) &&
              owner.Teardown(nextEpoch) &&
              ownerFreshness.Teardown(nextEpoch),
            "A same-serial cleanup from an old generation cannot close the new owner");

        Check(ReachEyeForPass(0, false) == 0 &&
              ReachEyeForPass(1, false) == 1 &&
              ReachEyeForPass(0, true) == 1 &&
              ReachEyeForPass(1, true) == 0 &&
              ReachEyeForPass(2, false) == -1,
            "Reach pass order matches Halo 3 right-eye-first behavior exactly");
        const ReachStereoPassPolicy firstPass =
            SelectReachStereoPassPolicy(0, false, 1);
        const ReachStereoPassPolicy finalPass =
            SelectReachStereoPassPolicy(1, false, 1);
        const ReachStereoPassPolicy invalidPass =
            SelectReachStereoPassPolicy(2, false, 1);
        Check(firstPass.valid && firstPass.eye == 0 &&
              firstPass.writeLastWindow && firstPass.lastWindowInput == 0 &&
              firstPass.restoreLastWindowAfterPass &&
              finalPass.valid && finalPass.eye == 1 &&
              finalPass.writeLastWindow && finalPass.lastWindowInput == 1 &&
              !finalPass.restoreLastWindowAfterPass &&
              !invalidPass.valid && invalidPass.eye == -1 &&
              !invalidPass.writeLastWindow &&
              invalidPass.restoreLastWindowAfterPass,
            "Reach last-window policy fails closed for invalid passes and preserves the final result");
        Check(kReachRollbackLayout.workspaceSize == 0x2B0 &&
              kReachCameraPairDataSize == 0x2A8 &&
              kReachSecondaryDerivedOffset + kReachDerivedBlockSize ==
                  kReachCameraPairDataSize &&
              kReachRollbackLayout.cameraStateOffset == 0x3B0 &&
              kReachRollbackLayout.cameraStateSize == 0xC8 &&
              kReachRollbackLayout.currentMatricesOffset == 0x490 &&
              kReachRollbackLayout.currentMatricesSize == 0x2D0 &&
              kReachRollbackLayout.previousMatricesOffset == 0x760 &&
              kReachRollbackLayout.previousMatricesSize == 0x2D0 &&
              kReachRollbackLayout.excludedLastWindowOffset == 0xA30 &&
              kReachRollbackLayout.workspaceSize < kReachPlayerViewStride,
            "Reach rollback policy snapshots bounded regions and excludes the whole player view");
        Check(kReachVisibilityClusterLookupCallRva == 0x000C3320 &&
              kReachVisibilityClusterLookupTargetRva == 0x00273458 &&
              kReachVisibilitySecondaryCompactLeaRva == 0x00273468 &&
              kReachVisibilityBuildCallRva == 0x000C335C &&
              kReachVisibilityBuildTargetRva == 0x0027F408 &&
              kReachVisibilitySecondaryDerivedLeaRva == 0x000C3339 &&
              kReachVisibilitySecondaryCompactAddressRva == 0x00C9FC34 &&
              kReachVisibilitySecondaryDerivedAddressRva == 0x00C9FCC4,
            "Reach visibility evidence identifies the exact secondary camera pair consumer");

        constexpr float reachRad = 3.14159265358979323846f / 180.0f;
        const ReachSymmetricFovCover fovCover = SelectReachSymmetricFovCover(
            -61.5f * reachRad, 43.4f * reachRad,
            53.0f * reachRad, -53.0f * reachRad, 2912, 2100);
        const float halfV = fovCover.verticalFov * 0.5f;
        const float halfH = std::atan(
            std::tan(halfV) * (2912.0f / 2100.0f));
        const ReachProjectionHalfFovs decodedFov =
            DecodeReachProjectionHalfFovs(
                1.0f / std::tan(halfH), 1.0f / std::tan(halfV));
        Check(fovCover.valid && decodedFov.valid &&
              std::fabs(decodedFov.horizontal - 61.5f * reachRad) < 0.0001f &&
              ReachProjectionCoversOpenXr(decodedFov, fovCover),
            "Reach raster and OpenXR FOVs cover the same headset view");
        Check(!SelectReachSymmetricFovCover(
                   0.1f, 0.2f, 0.3f, -0.3f, 2912, 2100).valid &&
              !SelectReachSymmetricFovCover(
                   -0.8f, 0.8f, 0.8f, -0.8f, 0, 2100).valid &&
              !DecodeReachProjectionHalfFovs(0.0f, 1.0f).valid &&
              !ReachProjectionCoversOpenXr(
                   DecodeReachProjectionHalfFovs(2.0f, 2.0f), fovCover),
            "Reach FOV proof rejects invalid and under-covering projections");

        const std::array<ReachEyeCullFrustum, 2> asymmetricEyes{{
            {-61.5f * reachRad, 43.4f * reachRad,
             53.0f * reachRad, -48.0f * reachRad,
             {0.0f, 0.0f, 0.0f, 1.0f}},
            {-43.4f * reachRad, 61.5f * reachRad,
             49.0f * reachRad, -53.0f * reachRad,
             {0.0f, 0.0f, 0.0f, 1.0f}},
        }};
        const ReachSymmetricFovCover stereoIdentityCover =
            SelectReachStereoCullFovCover(asymmetricEyes, 2912, 2100);
        const float stereoIdentityHalfV =
            stereoIdentityCover.verticalFov * 0.5f;
        const float stereoIdentityHalfH = std::atan(
            std::tan(stereoIdentityHalfV) * (2912.0f / 2100.0f));
        Check(stereoIdentityCover.valid &&
              std::fabs(stereoIdentityCover.requiredHalfHorizontal -
                        61.5f * reachRad) < 0.0001f &&
              std::fabs(stereoIdentityCover.requiredHalfVertical -
                        53.0f * reachRad) < 0.0001f &&
              stereoIdentityHalfH + 0.0001f >=
                  stereoIdentityCover.requiredHalfHorizontal &&
              stereoIdentityHalfV + 0.0001f >=
                  stereoIdentityCover.requiredHalfVertical,
            "Reach stereo cull FOV encloses all eight asymmetric identity-eye corners");

        const auto yawQuaternion = [](float yaw, float scale = 1.0f)
        {
            return std::array<float, 4>{
                0.0f, std::sin(yaw * 0.5f) * scale, 0.0f,
                std::cos(yaw * 0.5f) * scale};
        };

        // Adversarial headset geometry: each raw OpenXR eye is much wider on
        // its nasal side, while opposed cant rotates the originally narrow
        // temporal side outward. Reach rasterizes a symmetric per-eye FOV, so
        // the outer cull must contain those widened raster corners rather than
        // only the raw asymmetric angles.
        const std::array<ReachEyeCullFrustum, 2> adversarialRawEyes{{
            {-20.0f * reachRad, 70.0f * reachRad,
             45.0f * reachRad, -35.0f * reachRad,
             yawQuaternion(10.0f * reachRad)},
            {-70.0f * reachRad, 20.0f * reachRad,
             42.0f * reachRad, -38.0f * reachRad,
             yawQuaternion(-10.0f * reachRad)},
        }};
        const std::array<ReachSymmetricFovCover, 2> adversarialRasterCovers{{
            SelectReachSymmetricFovCover(
                adversarialRawEyes[0].angleLeft,
                adversarialRawEyes[0].angleRight,
                adversarialRawEyes[0].angleUp,
                adversarialRawEyes[0].angleDown, 2912, 2100),
            SelectReachSymmetricFovCover(
                adversarialRawEyes[1].angleLeft,
                adversarialRawEyes[1].angleRight,
                adversarialRawEyes[1].angleUp,
                adversarialRawEyes[1].angleDown, 2912, 2100),
        }};
        std::array<ReachEyeCullFrustum, 2> adversarialRasterEyes{};
        const bool builtAdversarialLeft =
            BuildReachSymmetricRasterCullFrustum(
                adversarialRasterCovers[0],
                adversarialRawEyes[0].relativeOrientation,
                2912, 2100, adversarialRasterEyes[0]);
        const bool builtAdversarialRight =
            BuildReachSymmetricRasterCullFrustum(
                adversarialRasterCovers[1],
                adversarialRawEyes[1].relativeOrientation,
                2912, 2100, adversarialRasterEyes[1]);
        const float adversarialRasterHalfV =
            adversarialRasterCovers[0].verticalFov * 0.5f;
        const float adversarialRasterHalfH = std::atan(
            std::tan(adversarialRasterHalfV) * (2912.0f / 2100.0f));
        Check(builtAdversarialLeft && builtAdversarialRight &&
              std::fabs(adversarialRasterEyes[0].angleLeft +
                        adversarialRasterHalfH) < 0.000001f &&
              std::fabs(adversarialRasterEyes[0].angleRight -
                        adversarialRasterHalfH) < 0.000001f &&
              std::fabs(adversarialRasterEyes[0].angleUp -
                        adversarialRasterHalfV) < 0.000001f &&
              std::fabs(adversarialRasterEyes[0].angleDown +
                        adversarialRasterHalfV) < 0.000001f &&
              adversarialRasterEyes[0].angleLeft <
                  adversarialRawEyes[0].angleLeft - 40.0f * reachRad &&
              adversarialRasterEyes[1].angleRight >
                  adversarialRawEyes[1].angleRight + 40.0f * reachRad &&
              adversarialRasterEyes[0].relativeOrientation ==
                  adversarialRawEyes[0].relativeOrientation &&
              adversarialRasterEyes[1].relativeOrientation ==
                  adversarialRawEyes[1].relativeOrientation,
            "Reach cull inputs describe the actual widened symmetric eye rasters");

        const ReachSymmetricFovCover adversarialRawCull =
            SelectReachStereoCullFovCover(
                adversarialRawEyes, 2912, 2100);
        const ReachSymmetricFovCover adversarialRasterCull =
            SelectReachStereoCullFovCover(
                adversarialRasterEyes, 2912, 2100);
        const float adversarialCullHalfV =
            adversarialRasterCull.verticalFov * 0.5f;
        const float adversarialCullHalfH = std::atan(
            std::tan(adversarialCullHalfV) * (2912.0f / 2100.0f));
        Check(adversarialRawCull.valid && adversarialRasterCull.valid &&
              std::fabs(adversarialRasterCull.requiredHalfHorizontal -
                        80.0f * reachRad) < 0.0001f &&
              adversarialRasterCull.requiredHalfHorizontal >
                  adversarialRawCull.requiredHalfHorizontal +
                      15.0f * reachRad &&
              adversarialRasterCull.requiredHalfVertical >
                  adversarialRawCull.requiredHalfVertical +
                      15.0f * reachRad &&
              adversarialCullHalfH + 0.0001f >=
                  adversarialRasterCull.requiredHalfHorizontal &&
              adversarialCullHalfV + 0.0001f >=
                  adversarialRasterCull.requiredHalfVertical,
            "Reach binocular cull encloses widened asymmetric-and-canted raster corners");

        ReachEyeCullFrustum rejectedRasterFrustum{
            -0.5f, 0.5f, 0.5f, -0.5f, {0.0f, 0.0f, 0.0f, 1.0f}};
        ReachSymmetricFovCover invalidRasterCover =
            adversarialRasterCovers[0];
        invalidRasterCover.valid = false;
        const bool rejectsInvalidRasterCover =
            !BuildReachSymmetricRasterCullFrustum(
                invalidRasterCover,
                adversarialRawEyes[0].relativeOrientation,
                2912, 2100, rejectedRasterFrustum);
        const bool clearsRejectedRasterFrustum =
            rejectedRasterFrustum.angleLeft == 0.0f &&
            rejectedRasterFrustum.angleRight == 0.0f &&
            rejectedRasterFrustum.angleUp == 0.0f &&
            rejectedRasterFrustum.angleDown == 0.0f;
        Check(rejectsInvalidRasterCover && clearsRejectedRasterFrustum &&
              !BuildReachSymmetricRasterCullFrustum(
                  adversarialRasterCovers[0],
                  {0.0f, 0.0f, 0.0f, 0.0f},
                  2912, 2100, rejectedRasterFrustum) &&
              !BuildReachSymmetricRasterCullFrustum(
                  adversarialRasterCovers[0],
                  adversarialRawEyes[0].relativeOrientation,
                  2912, 0, rejectedRasterFrustum),
            "Reach symmetric raster cull conversion fails closed on invalid input");

        const std::array<ReachEyeCullFrustum, 2> cantedEyes{{
            {-40.0f * reachRad, 40.0f * reachRad,
             35.0f * reachRad, -35.0f * reachRad,
             yawQuaternion(10.0f * reachRad)},
            {-40.0f * reachRad, 40.0f * reachRad,
             35.0f * reachRad, -35.0f * reachRad,
             yawQuaternion(-10.0f * reachRad)},
        }};
        const ReachSymmetricFovCover cantedCover =
            SelectReachStereoCullFovCover(cantedEyes, 2912, 2100);
        std::array<ReachEyeCullFrustum, 2> scaledCantedEyes = cantedEyes;
        for (ReachEyeCullFrustum& eye : scaledCantedEyes)
            for (float& component : eye.relativeOrientation)
                component *= -3.25f;
        const ReachSymmetricFovCover scaledCantedCover =
            SelectReachStereoCullFovCover(scaledCantedEyes, 2912, 2100);
        const std::array<ReachEyeCullFrustum, 2> swappedCantedEyes{{
            cantedEyes[1], cantedEyes[0]}};
        const ReachSymmetricFovCover swappedCantedCover =
            SelectReachStereoCullFovCover(swappedCantedEyes, 2912, 2100);
        Check(cantedCover.valid && scaledCantedCover.valid &&
              swappedCantedCover.valid &&
              std::fabs(cantedCover.requiredHalfHorizontal -
                        50.0f * reachRad) < 0.0001f &&
              cantedCover.requiredHalfVertical > 35.0f * reachRad &&
              std::fabs(cantedCover.verticalFov -
                        scaledCantedCover.verticalFov) < 0.000001f &&
              std::fabs(cantedCover.requiredHalfHorizontal -
                        swappedCantedCover.requiredHalfHorizontal) < 0.000001f &&
              std::fabs(cantedCover.requiredHalfVertical -
                        swappedCantedCover.requiredHalfVertical) < 0.000001f,
            "Reach stereo cull FOV normalizes quaternion scale and is invariant to eye order");

        bool yawEnvelopeProperty = true;
        constexpr std::array<float, 5> yawDegrees{
            0.0f, 2.5f, 6.0f, 10.0f, 15.0f};
        constexpr std::array<float, 4> quaternionScales{
            0.25f, 1.0f, 3.0f, -2.0f};
        for (float yawDegreesValue : yawDegrees)
        {
            const float yaw = yawDegreesValue * reachRad;
            ReachSymmetricFovCover reference{};
            for (size_t scaleIndex = 0;
                 scaleIndex < quaternionScales.size(); ++scaleIndex)
            {
                const float scale = quaternionScales[scaleIndex];
                const std::array<ReachEyeCullFrustum, 2> propertyEyes{{
                    {-30.0f * reachRad, 30.0f * reachRad,
                     25.0f * reachRad, -25.0f * reachRad,
                     yawQuaternion(yaw, scale)},
                    {-30.0f * reachRad, 30.0f * reachRad,
                     25.0f * reachRad, -25.0f * reachRad,
                     yawQuaternion(-yaw, scale)},
                }};
                const ReachSymmetricFovCover candidate =
                    SelectReachStereoCullFovCover(
                        propertyEyes, 2912, 2100);
                yawEnvelopeProperty = yawEnvelopeProperty && candidate.valid &&
                    std::fabs(candidate.requiredHalfHorizontal -
                              (30.0f + yawDegreesValue) * reachRad) < 0.0001f;
                if (scaleIndex == 0)
                    reference = candidate;
                else
                    yawEnvelopeProperty = yawEnvelopeProperty &&
                        std::fabs(candidate.verticalFov -
                                  reference.verticalFov) < 0.000001f &&
                        std::fabs(candidate.requiredHalfVertical -
                                  reference.requiredHalfVertical) < 0.000001f;
            }
        }
        Check(yawEnvelopeProperty,
            "Reach stereo cull FOV encloses deterministic opposed-yaw properties");

        std::array<ReachEyeCullFrustum, 2> invalidStereoEyes = asymmetricEyes;
        invalidStereoEyes[0].relativeOrientation = {0.0f, 0.0f, 0.0f, 0.0f};
        const bool rejectsZeroQuaternion =
            !SelectReachStereoCullFovCover(
                invalidStereoEyes, 2912, 2100).valid;
        invalidStereoEyes = asymmetricEyes;
        invalidStereoEyes[1].relativeOrientation[2] =
            std::numeric_limits<float>::quiet_NaN();
        const bool rejectsNonFiniteQuaternion =
            !SelectReachStereoCullFovCover(
                invalidStereoEyes, 2912, 2100).valid;
        invalidStereoEyes = asymmetricEyes;
        invalidStereoEyes[0].angleLeft =
            -std::numeric_limits<float>::infinity();
        const bool rejectsNonFiniteFov =
            !SelectReachStereoCullFovCover(
                invalidStereoEyes, 2912, 2100).valid;
        invalidStereoEyes = asymmetricEyes;
        invalidStereoEyes[0].angleRight = 1.57079632679489661923f;
        const bool rejectsRearHemisphereFov =
            !SelectReachStereoCullFovCover(
                invalidStereoEyes, 2912, 2100).valid;
        const std::array<ReachEyeCullFrustum, 2> behindCenterEyes{{
            {-20.0f * reachRad, 20.0f * reachRad,
             20.0f * reachRad, -20.0f * reachRad,
             yawQuaternion(80.0f * reachRad)},
            {-20.0f * reachRad, 20.0f * reachRad,
             20.0f * reachRad, -20.0f * reachRad,
             yawQuaternion(-80.0f * reachRad)},
        }};
        Check(rejectsZeroQuaternion && rejectsNonFiniteQuaternion &&
              rejectsNonFiniteFov && rejectsRearHemisphereFov &&
              !SelectReachStereoCullFovCover(
                  behindCenterEyes, 2912, 2100).valid &&
              !SelectReachStereoCullFovCover(
                  asymmetricEyes, 0, 2100).valid,
            "Reach stereo cull FOV rejects degenerate, non-finite, and behind-centre inputs");
    }

    {
        std::array<uint8_t, 0x90> compactCamera{};
        const auto store = [&compactCamera](
                               size_t offset, const auto& value)
        {
            std::memcpy(
                compactCamera.data() + offset, &value, sizeof(value));
        };
        const float position[3]{ 10.0f, -2.0f, 5.0f };
        const float forward[3]{ 1.0f, 0.0f, 0.0f };
        const float up[3]{ 0.0f, 0.0f, 1.0f };
        const float verticalFov = 1.0f;
        const ReachObservedRect windowBounds{ 0, 0, 1080, 1920 };
        const ReachObservedRect renderBounds{ 0, 0, 720, 1280 };
        const ReachObservedRect clientBounds{ 0, 0, 720, 1280 };
        store(0x00, position);
        store(0x0C, forward);
        store(0x18, up);
        store(0x28, verticalFov);
        store(0x38, windowBounds);
        store(0x4C, renderBounds);
        store(0x5C, clientBounds);

        ReachCompactCameraObservation observedCamera{};
        Check(ValidateReachCompactCamera(
                  compactCamera.data(), compactCamera.size(), observedCamera) &&
                  observedCamera.position[0] == position[0] &&
                  observedCamera.position[1] == position[1] &&
                  observedCamera.position[2] == position[2] &&
                  observedCamera.verticalFov == verticalFov &&
                  observedCamera.clientBounds.x1 == clientBounds.x1,
            "Reach compact-camera proof accepts and decodes its exact validated layout");
        Check(!ValidateReachCompactCamera(
                  nullptr, compactCamera.size(), observedCamera) &&
                  !ValidateReachCompactCamera(
                      compactCamera.data(), compactCamera.size() - 1,
                      observedCamera),
            "Reach compact-camera proof rejects null or truncated snapshots");

        const auto validateWithFloat =
            [&compactCamera, &observedCamera](size_t offset, float value)
        {
            auto candidate = compactCamera;
            std::memcpy(candidate.data() + offset, &value, sizeof(value));
            return ValidateReachCompactCamera(
                candidate.data(), candidate.size(), observedCamera);
        };
        Check(!validateWithFloat(
                  0x00, std::numeric_limits<float>::quiet_NaN()) &&
                  !validateWithFloat(
                      0x0C, std::numeric_limits<float>::infinity()),
            "Reach compact-camera proof rejects NaN and infinite vectors");
        Check(validateWithFloat(0x0C, std::sqrt(1.0005f)) &&
                  !validateWithFloat(0x0C, std::sqrt(1.0015f)),
            "Reach compact-camera proof enforces the HREK axis-length tolerance");

        {
            auto candidate = compactCamera;
            const float nearOrthogonalUp[3]{
                0.0005f, 0.0f, std::sqrt(1.0f - 0.0005f * 0.0005f)
            };
            std::memcpy(
                candidate.data() + 0x18, nearOrthogonalUp,
                sizeof(nearOrthogonalUp));
            Check(ValidateReachCompactCamera(
                      candidate.data(), candidate.size(), observedCamera),
                "Reach compact-camera proof accepts dot products inside tolerance");
        }
        {
            auto candidate = compactCamera;
            const float nonOrthogonalUp[3]{
                0.0015f, 0.0f, std::sqrt(1.0f - 0.0015f * 0.0015f)
            };
            std::memcpy(
                candidate.data() + 0x18, nonOrthogonalUp,
                sizeof(nonOrthogonalUp));
            Check(!ValidateReachCompactCamera(
                      candidate.data(), candidate.size(), observedCamera),
                "Reach compact-camera proof rejects non-orthogonal axes");
        }

        Check(!validateWithFloat(0x28, kReachCameraFovMin) &&
                  validateWithFloat(
                      0x28, std::nextafter(
                                kReachCameraFovMin,
                                std::numeric_limits<float>::infinity())) &&
                  validateWithFloat(
                      0x28, std::nextafter(kReachCameraFovMax, 0.0f)) &&
                  !validateWithFloat(0x28, kReachCameraFovMax),
            "Reach compact-camera proof enforces strict finite FOV bounds");
        {
            auto candidate = compactCamera;
            const ReachObservedRect unorderedWindow{ 1, 0, 1, 1920 };
            std::memcpy(
                candidate.data() + 0x38, &unorderedWindow,
                sizeof(unorderedWindow));
            Check(!ValidateReachCompactCamera(
                      candidate.data(), candidate.size(), observedCamera),
                "Reach compact-camera proof rejects unordered viewport bounds");
        }
        {
            auto candidate = compactCamera;
            const ReachObservedRect nonzeroClientOrigin{ 1, 0, 720, 1280 };
            std::memcpy(
                candidate.data() + 0x5C, &nonzeroClientOrigin,
                sizeof(nonzeroClientOrigin));
            Check(!ValidateReachCompactCamera(
                      candidate.data(), candidate.size(), observedCamera),
                "Reach compact-camera proof requires the proven zero client origin");
        }
        {
            auto candidate = compactCamera;
            const ReachObservedRect undersizedClient{ 0, 0, 7, 1280 };
            std::memcpy(
                candidate.data() + 0x5C, &undersizedClient,
                sizeof(undersizedClient));
            Check(!ValidateReachCompactCamera(
                      candidate.data(), candidate.size(), observedCamera),
                "Reach compact-camera proof enforces the producer's minimum extent");
        }

        ReachObserverFreshnessWindow freshness;
        Check(!freshness.ObserveTransaction(10),
            "One Reach transaction cannot satisfy freshness stability");
        Check(!freshness.ObserveTransaction(400),
            "A sub-500 ms Reach transaction preserves but cannot yet arm freshness");
        Check(!freshness.ObserveTransaction(800),
            "Repeated fresh Reach transactions stay below the safety interval");
        Check(!freshness.ObserveTransaction(1010),
            "Reach freshness remains unarmed through exactly one continuous second");
        Check(freshness.IsFresh(1010) &&
                  freshness.TransactionCount() == 4 &&
                  freshness.CurrentSpanMs() == 1000,
            "Reach freshness counts only sub-500 ms transaction intervals");
        Check(freshness.ObserveTransaction(1011) &&
                  freshness.IsStable(1011),
            "Reach freshness becomes observationally stable only after one second");
        Check(freshness.Tick(1510),
            "A 499 ms gap keeps the Reach transaction window fresh");
        Check(!freshness.Tick(1511) && freshness.TransactionCount() == 0,
            "A 500 ms gap resets the Reach freshness window");
        Check(!freshness.ObserveTransaction(2000),
            "A new Reach transaction starts a fresh observational window");
        Check(!freshness.ObserveTransaction(2500) &&
                  freshness.TransactionCount() == 1 &&
                  freshness.CurrentSpanMs() == 0,
            "A missed Reach pulse is inconclusive and begins a new window");
        Check(!freshness.ObserveTransaction(2400) &&
                  freshness.TransactionCount() == 1,
            "A non-monotonic Reach observation also fails closed");
        freshness.Reset();
        Check(!freshness.IsFresh(3000) &&
                  !freshness.IsStable(3000) &&
                  freshness.LastTransactionMs() == 0,
            "Reach freshness reset clears all observational state");

        Check(!freshness.ObserveTransaction(4000) &&
                  !freshness.ObserveTransaction(4400) &&
                  !freshness.ObserveTransaction(4800) &&
                  !freshness.Tick(5001) &&
                  freshness.CurrentSpanMs() == 800,
            "Elapsed wall time cannot satisfy a Reach gate without a new transaction");
        Check(freshness.ObserveTransaction(5101) &&
                  freshness.CurrentSpanMs() == 1101,
            "A new Reach transaction may satisfy the strict one-second span");
        Check(!freshness.Tick(5601) &&
                  freshness.TransactionCount() == 0,
            "A post-gate 500 ms pause expires the Reach freshness window");
        Check(!freshness.ObserveTransaction(6000) &&
                  !freshness.ObserveTransaction(6400) &&
                  !freshness.ObserveTransaction(6800) &&
                  freshness.ObserveTransaction(7001),
            "Reach freshness can establish a new stable window after pause recovery");

        std::array<ReachObserverFreshnessWindow, 4> slotFreshness{};
        Check(ReachObserverUniqueFreshOwner(
                  slotFreshness.data(), slotFreshness.size(), 10) ==
                  kReachNoFreshOwner,
            "Reach observer reports no owner before a slot heartbeat");
        slotFreshness[0].ObserveTransaction(10);
        Check(ReachObserverUniqueFreshOwner(
                  slotFreshness.data(), slotFreshness.size(), 10) == 0,
            "Reach observer identifies one fresh player-view owner");
        slotFreshness[1].ObserveTransaction(20);
        Check(ReachObserverUniqueFreshOwner(
                  slotFreshness.data(), slotFreshness.size(), 20) ==
                  kReachMultipleFreshOwners,
            "Reach observer fails closed when two player slots are fresh");
        Check(ReachObserverUniqueFreshOwner(
                  slotFreshness.data(), slotFreshness.size(), 510) == 1,
            "Reach observer expires a stale slot independently");
    }

    {
        Check(kTitleRuntimeSlotCount == 6 &&
                  TitleRuntimeSlotIndex(GameTitle::Halo3) == 0 &&
                  TitleRuntimeSlotIndex(GameTitle::Halo3ODST) == 1 &&
                  TitleRuntimeSlotIndex(GameTitle::HaloReach) == 2 &&
                  TitleRuntimeSlotIndex(GameTitle::Halo4) == 3 &&
                  TitleRuntimeSlotIndex(GameTitle::HaloCE) == 4 &&
                  TitleRuntimeSlotIndex(GameTitle::Halo2) == 5 &&
                  TitleRuntimeSlotIndex(GameTitle::None) ==
                      kInvalidTitleRuntimeSlot &&
                  TitleRuntimeSlotIndex(GameTitle::Unknown) ==
                      kInvalidTitleRuntimeSlot,
            "title-runtime slots cover only concrete MCC game modules");
        Check(TitleRuntimeSlotTitle(0) == GameTitle::Halo3 &&
                  TitleRuntimeSlotTitle(1) == GameTitle::Halo3ODST &&
                  TitleRuntimeSlotTitle(2) == GameTitle::HaloReach &&
                  TitleRuntimeSlotTitle(3) == GameTitle::Halo4 &&
                  TitleRuntimeSlotTitle(4) == GameTitle::HaloCE &&
                  TitleRuntimeSlotTitle(5) == GameTitle::Halo2 &&
                  TitleRuntimeSlotTitle(6) == GameTitle::None,
            "title-runtime slot mapping is reversible and bounds checked");
        Check(TitleRuntimeAvailabilityBit(GameTitle::Halo3) == 0x01u &&
                  TitleRuntimeAvailabilityBit(GameTitle::Halo3ODST) == 0x02u &&
                  TitleRuntimeAvailabilityBit(GameTitle::HaloReach) == 0x04u &&
                  TitleRuntimeAvailabilityBit(GameTitle::Halo4) == 0x08u &&
                  TitleRuntimeAvailabilityBit(GameTitle::HaloCE) == 0x10u &&
                  TitleRuntimeAvailabilityBit(GameTitle::Halo2) == 0x20u &&
                  TitleRuntimeAvailabilityBit(GameTitle::Unknown) == 0 &&
                  kTitleRuntimeAvailabilityMask == 0x3Fu,
            "title-runtime availability bits are stable and non-overlapping");
        Check(static_cast<uint32_t>(TitleCapability_ControllerInput) == 0x40u &&
                  static_cast<uint32_t>(TitleCapability_Haptics) == 0x80u &&
                  kTitleRuntimeKnownCapabilities == 0xFFu,
            "controller input and haptics extend the known capability mask exactly");
    }

    {
        constexpr uint64_t kEpoch = 1000;
        constexpr uint64_t kFreshFor = 500;
        constexpr uint32_t kUnknownCapability = 0x80000000u;
        constexpr uint32_t kPublishedCapabilities =
            TitleCapability_Stereo |
            TitleCapability_ControllerAim |
            TitleCapability_RuntimeModes |
            TitleCapability_ControllerInput |
            TitleCapability_Haptics;
        constexpr uint32_t kArmRequiredCapabilities =
            TitleCapability_Stereo |
            TitleCapability_ControllerAim |
            TitleCapability_Hud |
            TitleCapability_ArmIk |
            TitleCapability_RoomScale |
            TitleCapability_Haptics;
        constexpr uint32_t kUnarmedCapabilities =
            TitleCapability_RuntimeModes |
            TitleCapability_ControllerInput;

        const auto isCanonicalFailure = [](
            const TitleRuntimeSnapshot& snapshot, uint32_t ownerCount)
        {
            return snapshot.owner == GameTitle::None &&
                snapshot.generation == 0 &&
                !snapshot.installed && !snapshot.armed &&
                !snapshot.teardownRequested &&
                snapshot.mode == RuntimeMode::Shell &&
                snapshot.heartbeatMs == 0 &&
                snapshot.enabledCapabilities == TitleCapability_None &&
                snapshot.qualifyingOwnerCount == ownerCount;
        };

        TitleRuntimeResolveInput single{};
        single.availabilityMask =
            TitleRuntimeAvailabilityBit(GameTitle::Halo3);
        single.availabilitySetEpochMs = kEpoch;
        single.nowMs = kEpoch + 1;
        single.titles[TitleRuntimeSlotIndex(GameTitle::Halo3)] = {
            GameTitle::Halo3,
            11,
            900,
            true,
            false,
            false,
            RuntimeMode::Vehicle,
            kEpoch + 1,
            kFreshFor,
            kPublishedCapabilities | kUnknownCapability,
        };

        const TitleRuntimeSnapshot owner = ResolveTitleRuntime(single);
        Check(owner.owner == GameTitle::Halo3 &&
                  owner.generation == 11 && owner.installed &&
                  !owner.armed && !owner.teardownRequested &&
                  owner.mode == RuntimeMode::Vehicle &&
                  owner.heartbeatMs == kEpoch + 1 &&
                  owner.enabledCapabilities == kPublishedCapabilities &&
                  owner.qualifyingOwnerCount == 1,
            "one fresh installed title owns an exact generation-tagged snapshot");
        Check(TitleRuntimeMaskUnarmedCapabilities(
                  owner, kArmRequiredCapabilities) == kUnarmedCapabilities,
            "an unarmed owner retains only caller-designated non-render capabilities");
        TitleRuntimeSnapshot armedOwner = owner;
        armedOwner.armed = true;
        armedOwner.enabledCapabilities |= kUnknownCapability;
        Check(TitleRuntimeMaskUnarmedCapabilities(
                  armedOwner, kArmRequiredCapabilities) ==
                  kPublishedCapabilities,
            "an armed owner keeps all known capabilities and strips unknown bits");

        TitleRuntimeResolveInput zero{};
        zero.availabilityMask =
            TitleRuntimeAvailabilityBit(GameTitle::Halo3);
        zero.availabilitySetEpochMs = kEpoch;
        zero.nowMs = kEpoch + 1;
        Check(isCanonicalFailure(ResolveTitleRuntime(zero), 0),
            "zero fresh owners produces the canonical fail-open snapshot");

        auto epochBoundary = single;
        epochBoundary.nowMs = kEpoch;
        epochBoundary.titles[0].heartbeatMs = kEpoch;
        auto generationBoundary = single;
        generationBoundary.availabilitySetEpochMs = 800;
        generationBoundary.titles[0].generationStartMs = kEpoch + 1;
        generationBoundary.titles[0].heartbeatMs = kEpoch + 1;
        auto future = single;
        future.nowMs = kEpoch;
        auto zeroWindow = single;
        zeroWindow.titles[0].heartbeatFreshForMs = 0;
        Check(isCanonicalFailure(ResolveTitleRuntime(epochBoundary), 0) &&
                  isCanonicalFailure(
                      ResolveTitleRuntime(generationBoundary), 0) &&
                  isCanonicalFailure(ResolveTitleRuntime(future), 0) &&
                  isCanonicalFailure(ResolveTitleRuntime(zeroWindow), 0),
            "epoch-equal, generation-equal, future, and zero-window heartbeats fail closed");

        auto lastFresh = single;
        lastFresh.nowMs = kEpoch + 1 + kFreshFor - 1;
        auto expired = lastFresh;
        expired.nowMs += 1;
        Check(ResolveTitleRuntime(lastFresh).owner == GameTitle::Halo3 &&
                  isCanonicalFailure(ResolveTitleRuntime(expired), 0),
            "heartbeat freshness accepts age window-minus-one and rejects exact expiry");

        auto uninstalled = single;
        uninstalled.titles[0].installed = false;
        auto teardown = single;
        teardown.titles[0].teardownRequested = true;
        auto zeroGeneration = single;
        zeroGeneration.titles[0].generation = 0;
        auto unavailable = single;
        unavailable.availabilityMask = 0;
        auto foreign = single;
        foreign.titles[0].title = GameTitle::Halo3ODST;
        Check(isCanonicalFailure(ResolveTitleRuntime(uninstalled), 0) &&
                  isCanonicalFailure(ResolveTitleRuntime(teardown), 0) &&
                  isCanonicalFailure(ResolveTitleRuntime(zeroGeneration), 0) &&
                  isCanonicalFailure(ResolveTitleRuntime(unavailable), 0) &&
                  isCanonicalFailure(ResolveTitleRuntime(foreign), 0),
            "uninstalled, teardown, zero-generation, unavailable, and foreign claims fail closed");

        auto multiple = single;
        multiple.availabilityMask |=
            TitleRuntimeAvailabilityBit(GameTitle::Halo3ODST);
        multiple.nowMs = kEpoch + 2;
        multiple.titles[TitleRuntimeSlotIndex(GameTitle::Halo3ODST)] = {
            GameTitle::Halo3ODST,
            22,
            900,
            true,
            true,
            false,
            RuntimeMode::Paused,
            kEpoch + 2,
            kFreshFor,
            TitleCapability_Stereo,
        };
        Check(isCanonicalFailure(ResolveTitleRuntime(multiple), 2),
            "two fresh title owners discard both complete snapshots");
        multiple.nowMs = kEpoch + 1 + kFreshFor;
        const TitleRuntimeSnapshot soleRemainder = ResolveTitleRuntime(multiple);
        Check(soleRemainder.owner == GameTitle::Halo3ODST &&
                  soleRemainder.generation == 22 &&
                  soleRemainder.mode == RuntimeMode::Paused &&
                  soleRemainder.qualifyingOwnerCount == 1,
            "one title wins only after the other expires at the exact freshness boundary");

        TitleRuntimeResolveInput pending{};
        pending.availabilityMask =
            TitleRuntimeAvailabilityBit(GameTitle::Halo3) |
            TitleRuntimeAvailabilityBit(GameTitle::Halo3ODST);
        pending.availabilitySetEpochMs = kEpoch;
        pending.nowMs = kEpoch;
        pending.titles[TitleRuntimeSlotIndex(GameTitle::Halo3)] = {
            GameTitle::Halo3,
            11,
            900,
            true,
            true,
            false,
            RuntimeMode::Gameplay,
            kEpoch,
            kFreshFor,
            kPublishedCapabilities,
        };
        Check(TitleRuntimeOwnershipMayBePending(
                  pending, GameTitle::Halo3, 100),
            "a safe retained owner gets a short zero-owner post-transition grace");
        pending.nowMs = kEpoch + 99;
        Check(TitleRuntimeOwnershipMayBePending(
                  pending, GameTitle::Halo3, 100),
            "pending ownership remains valid through grace-minus-one");
        pending.nowMs = kEpoch + 100;
        Check(!TitleRuntimeOwnershipMayBePending(
                  pending, GameTitle::Halo3, 100),
            "pending ownership expires at the exact grace boundary");
        pending.nowMs = kEpoch - 1;
        Check(!TitleRuntimeOwnershipMayBePending(
                  pending, GameTitle::Halo3, 100),
            "a future availability-set epoch cannot grant pending ownership");

        auto soleModulePending = pending;
        soleModulePending.nowMs = kEpoch;
        soleModulePending.availabilityMask =
            TitleRuntimeAvailabilityBit(GameTitle::Halo3);
        auto teardownPending = pending;
        teardownPending.nowMs = kEpoch;
        teardownPending.titles[0].teardownRequested = true;
        auto uninstalledPending = pending;
        uninstalledPending.nowMs = kEpoch;
        uninstalledPending.titles[0].installed = false;
        auto duplicatePending = pending;
        duplicatePending.nowMs = kEpoch;
        duplicatePending.titles[TitleRuntimeSlotIndex(GameTitle::HaloReach)] =
            duplicatePending.titles[0];
        auto multiplePending = multiple;
        multiplePending.nowMs = kEpoch + 2;
        Check(!TitleRuntimeOwnershipMayBePending(
                  soleModulePending, GameTitle::Halo3, 100) &&
                  !TitleRuntimeOwnershipMayBePending(
                      teardownPending, GameTitle::Halo3, 100) &&
                  !TitleRuntimeOwnershipMayBePending(
                      uninstalledPending, GameTitle::Halo3, 100) &&
                  !TitleRuntimeOwnershipMayBePending(
                      duplicatePending, GameTitle::Halo3, 100),
            "sole-title, teardown, uninstalled, and duplicate retained claims get no grace");
        Check(!TitleRuntimeOwnershipMayBePending(
                  multiplePending, GameTitle::Halo3, 100),
            "multiple fresh owners fail closed immediately without pending grace");
    }

    {
        constexpr size_t kHalo3Slot =
            TitleRuntimeSlotIndex(GameTitle::Halo3);
        constexpr size_t kOdstSlot =
            TitleRuntimeSlotIndex(GameTitle::Halo3ODST);
        constexpr size_t kReachSlot =
            TitleRuntimeSlotIndex(GameTitle::HaloReach);
        constexpr uintptr_t kHalo3Base = 0x100000;
        constexpr uintptr_t kHalo3ReboundBase = 0x110000;
        constexpr uintptr_t kOdstBase = 0x200000;

        TitleRuntimeState state;
        const TitleRuntimeAvailabilitySnapshot initial =
            state.LoadAvailability();
        Check(initial.stable && initial.availabilityMask == 0 &&
                  initial.availabilitySetEpochMs == 0 &&
                  state.Generation(GameTitle::Halo3) == 0 &&
                  state.Generation(GameTitle::None) == 0,
            "title-runtime atomic state starts stable, unavailable, and generation zero");

        TitleRuntimeModuleSet modules{};
        modules.availabilityMask =
            TitleRuntimeAvailabilityBit(GameTitle::Halo3);
        modules.moduleBases[kHalo3Slot] = kHalo3Base;
        Check(state.PublishModuleSet(modules, 100) &&
                  state.Generation(GameTitle::Halo3) == 1,
            "the first Halo 3 module load creates generation one");
        TitleRuntimeAvailabilitySnapshot availability =
            state.LoadAvailability();
        Check(availability.stable &&
                  availability.availabilityMask == modules.availabilityMask &&
                  availability.availabilitySetEpochMs == 100 &&
                  availability.moduleBases[kHalo3Slot] == kHalo3Base,
            "the first available set publishes its exact base and epoch");

        Check(state.PublishModuleSet(modules, 150) &&
                  state.Generation(GameTitle::Halo3) == 1 &&
                  state.LoadAvailability().availabilitySetEpochMs == 100,
            "an unchanged module poll preserves generation and set epoch");

        modules.availabilityMask |=
            TitleRuntimeAvailabilityBit(GameTitle::Halo3ODST);
        modules.moduleBases[kOdstSlot] = kOdstBase;
        Check(state.PublishModuleSet(modules, 200) &&
                  state.Generation(GameTitle::Halo3) == 1 &&
                  state.Generation(GameTitle::Halo3ODST) == 1 &&
                  state.LoadAvailability().availabilitySetEpochMs == 200,
            "adding ODST advances the set epoch without changing resident Halo 3 generation");

        const TitleRuntimeLifecycle odstLifecycle{
            true, false, false, TitleCapability_ControllerInput
        };
        const uint32_t odstGeneration =
            state.Generation(GameTitle::Halo3ODST);
        Check(state.PublishLifecycle(
                  GameTitle::Halo3ODST, odstGeneration, odstLifecycle) &&
                  state.PublishHeartbeat(
                      GameTitle::Halo3ODST, odstGeneration, 201),
            "resident ODST establishes a pre-rebind heartbeat");

        modules.moduleBases[kHalo3Slot] = kHalo3ReboundBase;
        Check(state.PublishModuleSet(modules, 250) &&
                  state.Generation(GameTitle::Halo3) == 2 &&
                  state.Generation(GameTitle::Halo3ODST) == 1 &&
                  state.LoadAvailability().availabilitySetEpochMs == 250,
            "a Halo 3 base change advances its generation and the complete-set epoch");
        TitleRuntimeHeartbeatPolicy baseChangePolicy{};
        baseChangePolicy.freshForMs[kOdstSlot] = 500;
        Check(state.Resolve(250, baseChangePolicy).owner == GameTitle::None,
            "a pre-rebind heartbeat from another resident title cannot survive the complete-set epoch");

        modules.availabilityMask &=
            ~TitleRuntimeAvailabilityBit(GameTitle::Halo3ODST);
        modules.moduleBases[kOdstSlot] = 0;
        Check(state.PublishModuleSet(modules, 300) &&
                  state.Generation(GameTitle::Halo3ODST) == 2 &&
                  state.LoadAvailability().availabilitySetEpochMs == 300,
            "ODST removal invalidates its load generation and advances the set epoch");
        modules.availabilityMask |=
            TitleRuntimeAvailabilityBit(GameTitle::Halo3ODST);
        modules.moduleBases[kOdstSlot] = kOdstBase;
        Check(state.PublishModuleSet(modules, 400) &&
                  state.Generation(GameTitle::Halo3ODST) == 3 &&
                  state.LoadAvailability().availabilitySetEpochMs == 400,
            "ODST reload receives generation three rather than reusing stale state");

        const uint32_t h3GenerationBeforeInvalid =
            state.Generation(GameTitle::Halo3);
        const uint32_t odstGenerationBeforeInvalid =
            state.Generation(GameTitle::Halo3ODST);
        auto unknownBitSet = modules;
        unknownBitSet.availabilityMask |= 0x80000000u;
        auto inconsistentSet = modules;
        inconsistentSet.moduleBases[kReachSlot] = 0x300000;
        Check(!state.PublishModuleSet(unknownBitSet, 450) &&
                  !state.PublishModuleSet(inconsistentSet, 450) &&
                  state.Generation(GameTitle::Halo3) ==
                      h3GenerationBeforeInvalid &&
                  state.Generation(GameTitle::Halo3ODST) ==
                      odstGenerationBeforeInvalid,
            "unknown availability bits and mask/base disagreement are rejected atomically");

        modules.availabilityMask &=
            ~TitleRuntimeAvailabilityBit(GameTitle::Halo3ODST);
        modules.moduleBases[kOdstSlot] = 0;
        Check(state.PublishModuleSet(modules, 350) &&
                  state.Generation(GameTitle::Halo3ODST) == 4 &&
                  state.LoadAvailability().availabilitySetEpochMs == 400,
            "a non-monotonic module observation cannot move the set epoch backward");
    }

    {
        constexpr size_t kHalo3Slot =
            TitleRuntimeSlotIndex(GameTitle::Halo3);
        constexpr size_t kOdstSlot =
            TitleRuntimeSlotIndex(GameTitle::Halo3ODST);
        constexpr uint64_t kFreshFor = 500;
        constexpr uint32_t kUnknownCapability = 0x80000000u;
        constexpr uint32_t kCapabilities =
            TitleCapability_Stereo |
            TitleCapability_ControllerAim |
            TitleCapability_ControllerInput |
            TitleCapability_Haptics;

        TitleRuntimeState state;
        TitleRuntimeModuleSet modules{};
        modules.availabilityMask =
            TitleRuntimeAvailabilityBit(GameTitle::Halo3);
        modules.moduleBases[kHalo3Slot] = 0x100000;
        Check(state.PublishModuleSet(modules, 100),
            "atomic publication test establishes a Halo 3 module generation");
        const uint32_t generation1 = state.Generation(GameTitle::Halo3);

        Check(!state.PublishMode(
                  GameTitle::Halo3, generation1, RuntimeMode::Gameplay),
            "mode publication requires a current installed lifecycle");
        const TitleRuntimeLifecycle unarmed{
            true, false, false, kCapabilities | kUnknownCapability
        };
        Check(state.PublishLifecycle(
                  GameTitle::Halo3, generation1, unarmed) &&
                  state.PublishMode(
                      GameTitle::Halo3, generation1, RuntimeMode::Vehicle) &&
                  !state.PublishMode(
                      GameTitle::Halo3, generation1,
                      static_cast<RuntimeMode>(255)),
            "current lifecycle tags a valid mode and rejects invalid mode values");
        Check(!state.PublishHeartbeat(GameTitle::Halo3, generation1, 100) &&
                  state.PublishHeartbeat(
                      GameTitle::Halo3, generation1, 101) &&
                  !state.PublishHeartbeat(
                      GameTitle::Halo3, generation1, 101) &&
                  !state.PublishHeartbeat(
                      GameTitle::Halo3, generation1, 100),
            "heartbeats must be post-epoch and strictly monotonic within a generation");

        TitleRuntimeCandidate candidate{};
        Check(state.LoadCandidate(
                  GameTitle::Halo3, kFreshFor, candidate) &&
                  candidate.title == GameTitle::Halo3 &&
                  candidate.generation == generation1 &&
                  candidate.generationStartMs == 100 &&
                  candidate.installed && !candidate.armed &&
                  !candidate.teardownRequested &&
                  candidate.mode == RuntimeMode::Vehicle &&
                  candidate.heartbeatMs == 101 &&
                  candidate.heartbeatFreshForMs == kFreshFor &&
                  candidate.enabledCapabilities == kCapabilities,
            "atomic candidate loads one coherent lifecycle, mode, heartbeat, and sanitized mask");

        TitleRuntimeHeartbeatPolicy policy{};
        policy.freshForMs[kHalo3Slot] = kFreshFor;
        TitleRuntimeSnapshot snapshot = state.Resolve(101, policy);
        Check(snapshot.owner == GameTitle::Halo3 &&
                  snapshot.generation == generation1 &&
                  !snapshot.armed &&
                  snapshot.mode == RuntimeMode::Vehicle &&
                  snapshot.enabledCapabilities == kCapabilities,
            "the production atomic resolver preserves an unarmed unique owner");

        modules.availabilityMask |=
            TitleRuntimeAvailabilityBit(GameTitle::Halo3ODST);
        modules.moduleBases[kOdstSlot] = 0x200000;
        Check(state.PublishModuleSet(modules, 200) &&
                  state.Resolve(200, policy).owner == GameTitle::None &&
                  !state.PublishHeartbeat(
                      GameTitle::Halo3, generation1, 200) &&
                  state.PublishHeartbeat(
                      GameTitle::Halo3, generation1, 201) &&
                  state.Resolve(201, policy).owner == GameTitle::Halo3,
            "a set transition requires a strictly newer heartbeat from a resident generation");

        modules.moduleBases[kHalo3Slot] = 0x110000;
        Check(state.PublishModuleSet(modules, 250),
            "a rebound Halo 3 base starts a new atomic generation");
        const uint32_t generation2 = state.Generation(GameTitle::Halo3);
        Check(generation2 == 2 &&
                  !state.PublishLifecycle(
                      GameTitle::Halo3, generation1, unarmed) &&
                  !state.PublishMode(
                      GameTitle::Halo3, generation1, RuntimeMode::Paused) &&
                  !state.PublishHeartbeat(
                      GameTitle::Halo3, generation1, 251) &&
                  !state.ClearHeartbeat(GameTitle::Halo3, generation1),
            "stale lifecycle, mode, heartbeat, and clear publications are rejected");
        Check(!state.PublishLifecycle(
                  GameTitle::Halo3ODST, generation2, unarmed) &&
                  !state.PublishMode(
                      GameTitle::Halo3ODST, generation2,
                      RuntimeMode::Gameplay) &&
                  !state.PublishHeartbeat(
                      GameTitle::Halo3ODST, generation2, 251) &&
                  !state.ClearHeartbeat(
                      GameTitle::Halo3ODST, generation2),
            "a generation token from another title cannot publish foreign state");

        Check(state.LoadCandidate(
                  GameTitle::Halo3, kFreshFor, candidate) &&
                  candidate.generation == generation2 &&
                  candidate.generationStartMs == 250 &&
                  !candidate.installed && !candidate.armed &&
                  !candidate.teardownRequested &&
                  candidate.mode == RuntimeMode::Shell &&
                  candidate.heartbeatMs == 0 &&
                  candidate.enabledCapabilities == TitleCapability_None,
            "a new module generation cannot inherit prior lifecycle, mode, heartbeat, or capabilities");
        Check(!state.PublishMode(
                  GameTitle::Halo3, generation2, RuntimeMode::Gameplay) &&
                  state.PublishLifecycle(
                      GameTitle::Halo3, generation2, unarmed) &&
                  state.PublishMode(
                      GameTitle::Halo3, generation2, RuntimeMode::Vehicle) &&
                  !state.PublishHeartbeat(
                      GameTitle::Halo3, generation2, 250) &&
                  state.PublishHeartbeat(
                      GameTitle::Halo3, generation2, 251),
            "the new generation admits mode and heartbeat only after its lifecycle and boundary");
        snapshot = state.Resolve(251, policy);
        Check(snapshot.owner == GameTitle::Halo3 &&
                  snapshot.generation == generation2 &&
                  snapshot.mode == RuntimeMode::Vehicle,
            "current generation publications restore one coherent owner");
        Check(!state.ClearHeartbeat(
                  GameTitle::Halo3ODST, generation2) &&
                  state.Resolve(251, policy).owner == GameTitle::Halo3 &&
                  state.ClearHeartbeat(GameTitle::Halo3, generation2) &&
                  state.Resolve(251, policy).owner == GameTitle::None &&
                  state.LoadCandidate(
                      GameTitle::Halo3, kFreshFor, candidate) &&
                  candidate.heartbeatMs == 0,
            "only the exact title generation can clear a resident heartbeat");

        Check(state.PublishHeartbeat(
                  GameTitle::Halo3, generation2, 252),
            "a cleared current generation can publish a fresh heartbeat again");
        const TitleRuntimeLifecycle teardown{
            true, true, true, kCapabilities
        };
        Check(state.PublishLifecycle(
                  GameTitle::Halo3, generation2, teardown) &&
                  !state.PublishMode(
                      GameTitle::Halo3, generation2, RuntimeMode::Paused) &&
                  state.Resolve(252, policy).owner == GameTitle::None,
            "teardown vetoes ownership and further runtime-mode publication");
        const TitleRuntimeLifecycle notInstalled{
            false, false, false, kCapabilities
        };
        Check(state.PublishLifecycle(
                  GameTitle::Halo3, generation2, notInstalled) &&
                  state.Resolve(252, policy).owner == GameTitle::None,
            "an uninstalled title cannot own through a retained fresh heartbeat");
        Check(state.PublishLifecycle(
                  GameTitle::Halo3, generation2, unarmed) &&
                  state.PublishMode(
                      GameTitle::Halo3, generation2,
                      RuntimeMode::Gameplay) &&
                  state.Resolve(252, policy).owner == GameTitle::Halo3 &&
                  state.Resolve(252, policy).mode == RuntimeMode::Gameplay,
            "a safe unarmed lifecycle restores its exact generation-tagged mode");

        Check(!state.PublishLifecycle(
                  GameTitle::None, generation2, unarmed) &&
                  !state.PublishMode(
                      GameTitle::Unknown, generation2,
                      RuntimeMode::Gameplay) &&
                  !state.PublishHeartbeat(
                      GameTitle::None, generation2, 253) &&
                  !state.ClearHeartbeat(GameTitle::Unknown, generation2) &&
                  !state.LoadCandidate(
                      GameTitle::None, kFreshFor, candidate),
            "non-module titles cannot enter the atomic runtime state API");
    }

    {
        constexpr size_t kReachSlot =
            TitleRuntimeSlotIndex(GameTitle::HaloReach);
        TitleRuntimeState reachState;
        TitleRuntimeModuleSet modules{};
        modules.availabilityMask =
            TitleRuntimeAvailabilityBit(GameTitle::HaloReach);
        modules.moduleBases[kReachSlot] = 0x300000;
        Check(reachState.PublishModuleSet(modules, 100),
            "the generic runtime state can record Reach as available");
        const uint32_t generation =
            reachState.Generation(GameTitle::HaloReach);
        const TitleRuntimeLifecycle evidenceOnly{
            true, false, false, TitleCapability_None
        };
        Check(reachState.PublishLifecycle(
                  GameTitle::HaloReach, generation, evidenceOnly) &&
                  reachState.PublishMode(
                      GameTitle::HaloReach, generation,
                      RuntimeMode::Unsupported) &&
                  reachState.PublishHeartbeat(
                      GameTitle::HaloReach, generation, 101),
            "a synthetic Reach observation is generation tagged without enabling behavior");
        TitleRuntimeHeartbeatPolicy policy{};
        policy.freshForMs[kReachSlot] = 500;
        const TitleRuntimeSnapshot reachSnapshot =
            reachState.Resolve(101, policy);
        Check(reachSnapshot.owner == GameTitle::HaloReach &&
                  reachSnapshot.generation == generation &&
                  !reachSnapshot.armed &&
                  reachSnapshot.mode == RuntimeMode::Unsupported &&
                  reachSnapshot.enabledCapabilities == TitleCapability_None &&
                  TitleRuntimeMaskUnarmedCapabilities(
                      reachSnapshot, kTitleRuntimeKnownCapabilities) ==
                      TitleCapability_None,
            "the shared foundation manufactures zero Reach capabilities");
    }

    {
        Check(!OdstCameraOnlyScopeRequired(false, true, false),
            "a public build never claims the private ODST camera core");
        Check(OdstCameraOnlyScopeRequired(true, true, false),
            "the adapter transition remains camera-only after module polling");
        Check(OdstCameraOnlyScopeRequired(false, false, true),
            "owned teardown state remains isolated until presentation detaches");
        Check(OdstManualArmEligible(true, true, true, false),
            "stable camera plus explicit head/stereo toggles permits manual arm");
        Check(!OdstManualArmEligible(false, true, true, false),
            "manual ODST arm still requires the fresh-camera debounce");
        Check(!OdstManualArmEligible(true, true, true, true),
            "teardown always vetoes manual ODST arm");
        OdstHalo3LookAngles look{};
        Check(ComputeOdstHalo3LookAngles(
                  1.0f, 0.25f, 0.5f, 0.2f, 0.4f, -1.0f, 1.0f, 0.1f, look),
            "ODST Halo 3 look ownership accepts finite tracked angles");
        Check(std::fabs(look.yaw - 0.75f) < 1e-5f &&
                  std::fabs(look.pitch - 0.3f) < 1e-5f &&
                  std::fabs(look.roll - 0.4f) < 1e-5f,
            "ODST look matches Halo 3 recentered-yaw and absolute pitch/roll");
        Check(ComputeOdstHalo3LookAngles(
                  0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, 1.0f, 0.0f, look) &&
                  std::fabs(look.pitch - 1.5f) < 1e-5f,
            "ODST Halo 3 look retains the proven pitch safety clamp");
        Check(OdstVrOwnsLookStick(true, true),
            "tracked private ODST consumes the stock look-stick axes");
        Check(!OdstVrOwnsLookStick(true, false),
            "ODST menus retain ordinary look-stick input before head tracking");
        Check(!OdstVrOwnsLookStick(false, true),
            "the ODST input rule cannot affect Halo 3 or public title paths");
        Check(OdstMotionAimEligible(true, true, true, false),
            "owned+armed+tracked private ODST permits the narrow motion-aim gate");
        Check(!OdstMotionAimEligible(false, true, true, false),
            "ODST motion aim requires the camera-only context (never a public title)");
        Check(!OdstMotionAimEligible(true, false, true, false),
            "ODST motion aim stays closed until the camera hooks are armed");
        Check(!OdstMotionAimEligible(true, true, false, false),
            "ODST motion aim follows head tracking off");
        Check(!OdstMotionAimEligible(true, true, true, true),
            "teardown always vetoes ODST motion aim");
        Check(OdstFirstPersonControlBlend(1.0f) &&
                  OdstFirstPersonControlBlend(0.998f),
            "on-foot and near-1 camera transitions retain the FP render classification");
        Check(!OdstFirstPersonControlBlend(0.0f),
            "blend-0 cameras stay outside the FP-only render classification");
        Check(OdstShouldStereoRedirect(true, true, true, true),
            "a proven slot-0 camera is stereo-redirected");
        Check(!OdstShouldStereoRedirect(true, true, true, false),
            "a non-redirectable custom camera renders stock");
        Check(!OdstShouldStereoRedirect(false, true, true, true),
            "a foreign camera slot is never stereo-redirected");
        Check(!OdstShouldStereoRedirect(true, false, true, true),
            "a broken single-user tail is never stereo-redirected");
        Check(!OdstShouldStereoRedirect(true, true, false, true),
            "a mismatched nested FP source is never stereo-redirected");
        Check(OdstCamCopyRequestsTeardown(true, true, false),
            "a broken slot-0 single-user tail tears down (level unload/transition)");
        Check(!OdstCamCopyRequestsTeardown(true, true, true),
            "an active non-FP camera with a valid tail never tears down (3D recovers)");
        Check(!OdstCamCopyRequestsTeardown(false, true, false),
            "camera-copy teardown requires the core to be armed");
        Check(!OdstCamCopyRequestsTeardown(true, false, false),
            "camera-copy teardown only fires for our own primary slot");
        Check(PausePresentationInputAllowed(true),
            "proven Halo 3 gameplay may control pause presentation");
        Check(!PausePresentationInputAllowed(false),
            "MCC shell and private ODST Start edges cannot head-lock presentation");
        Check(OdstMustClearForeignPause(true, true, false) &&
                  OdstMustClearForeignPause(true, false, true),
            "private ODST entry clears either pending or active foreign pause state");
        Check(!OdstMustClearForeignPause(false, true, true),
            "foreign pause cleanup cannot affect non-ODST title ownership");
        Check(OdstNestedSourceIsCompatible(0, 0x1234),
            "ODST installation may precede the first nested FP source publish");
        Check(OdstNestedSourceIsCompatible(0x1234, 0x1234),
            "ODST accepts the proven nested FP source pointer");
        Check(!OdstNestedSourceIsCompatible(0x5678, 0x1234),
            "ODST rejects a nested FP source owned by another camera");
        Check(OdstInactiveCameraSlotsAreSafe(false, false, false),
            "constructed but inactive ODST split-screen slots are safe");
        Check(!OdstInactiveCameraSlotsAreSafe(false, true, false),
            "another active ODST camera blocks the single-user bring-up");
        Check(EvaluateOdstStereoFrame(false) ==
                  OdstStereoFrameAction::RenderStockWithoutCapture,
            "OpenXR no-render frames preserve ODST hooks without eye validation");
        Check(EvaluateOdstStereoFrame(true) ==
                  OdstStereoFrameAction::RenderStereoAndValidate,
            "active OpenXR frames still require validated stereo eye redirects");

        OdstHalo3FovMatch matchedFov{};
        Check(ComputeOdstHalo3FovMatch(
                  std::atan(1.8418f), std::atan(1.3290f), matchedFov),
            "ODST accepts the live Halo 3 headset FOV pair");
        Check(std::fabs(matchedFov.compactVerticalInput - 1.8418f) < 0.0001f &&
                  std::fabs(matchedFov.compactReferenceInput - 1.3290f) < 0.0001f,
            "ODST feeds both compact FOV inputs from Halo 3's matched pair");
        Check(std::fabs(matchedFov.projectionX - 0.54295f) < 0.0001f &&
                  std::fabs(matchedFov.projectionY - 0.75244f) < 0.0001f,
            "ODST reproduces the live Halo 3 projection scales");
        Check(!ComputeOdstHalo3FovMatch(0.0f, 0.9f, matchedFov),
            "ODST rejects an invalid headset FOV pair");

        for (int count : {41, 42, 43, 45, 46})
        {
            OdstFpSkeletonLayout fp{};
            Check(ComputeOdstFpSkeletonLayout(count, fp),
                "official ODST FP combined-skeleton counts are accepted");
            Check(fp.rightShoulder == 2 && fp.rightElbow == 4 &&
                      fp.rightWrist == 6 && fp.leftShoulder == 1 &&
                      fp.leftElbow == 3 && fp.leftWrist == 5,
                "ODST uses its editing-kit-proven arm chains");
            Check(fp.cameraControl == count - 1,
                "ODST camera_control is the final combined node");
            Check((fp.leftHandDescendants & (uint64_t{1} << 5)) != 0 &&
                      (fp.leftHandDescendants & (uint64_t{1} << 6)) == 0,
                "ODST left-hand mask owns only the authored left subtree");
            Check((fp.rightHandAndWeaponDescendants &
                       (uint64_t{1} << 6)) != 0 &&
                      (fp.rightHandAndWeaponDescendants &
                       (uint64_t{1} << 37)) != 0 &&
                      (fp.rightHandAndWeaponDescendants &
                       (uint64_t{1} << (fp.cameraControl - 1))) != 0 &&
                      (fp.rightHandAndWeaponDescendants &
                       (uint64_t{1} << fp.cameraControl)) == 0,
                "ODST right carrier owns weapon nodes but excludes camera_control");
        }
        OdstFpSkeletonLayout invalidFp{};
        Check(!ComputeOdstFpSkeletonLayout(38, invalidFp) &&
                  !ComputeOdstFpSkeletonLayout(65, invalidFp),
            "ODST rejects unsafe or structurally incomplete FP skeletons");

        Check(TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::None, false, false),
            "the MCC shell retains shared controller behavior");
        Check(TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Halo3, false, false),
            "an explicitly detected Halo 3 session retains shared features");
        Check(TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Unknown, true, false),
            "a fresh Halo 3 camera heartbeat resolves resident-module ambiguity");
        Check(!TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Halo3ODST, false, false),
            "public ODST XInput and presentation remain pass-through");
        Check(!TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Halo3ODST, true, false),
            "an explicit unsupported title beats a stale Halo 3 heartbeat");
        Check(!TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Unknown, false, false),
            "an ambiguous title without camera ownership fails closed");
        Check(TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Unknown, false, false, true, false),
            "the private frontend exception retains controller input");
        Check(TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Unknown, true, false, false, false),
            "a resolved owner's controller capability admits ambiguous input");
        Check(!TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Unknown, false, false, false, false),
            "the normal build keeps ambiguous title input fail-closed");
        Check(TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::None, false, false, false, false),
            "the unambiguous MCC shell retains ordinary controller input");
        Check(TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Halo3, false, false, false, true),
            "explicit Halo 3 retains ordinary controller input");
        Check(TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Halo3ODST, false, true, false, true),
            "private ODST camera ownership permits ordinary gamepad input");
        Check(TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Halo3ODST, false, false, false, true),
            "private ODST admits controller input before camera ownership");
        Check(!TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Halo3ODST, false, true, false, false),
            "public ODST camera ownership keeps controller input stock");
        Check(!TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::HaloCE, false, false, true, false),
            "an explicit title without controller admission keeps stock input");
        Check(!TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Unknown, false, true, true, true),
            "owned ODST teardown beats resident-module ambiguity");
        Check(TitleRegistry_AllowsSharedControllerInput(
                  GameTitle::Unknown, true, true, true, true),
            "a unique ambiguous owner may retain ordinary controller input");
        Check(!TitleRegistry_AllowsSharedGameplayFeatures(
                  GameTitle::Halo3, true, true),
            "private camera ownership overrides a stale Halo 3 signal");
        Check(!TitleRegistry_Halo3CameraOwnsAmbiguousState(
                  1050, 999, 1000),
            "an ambiguous title cannot inherit a pre-transition heartbeat");
        Check(!TitleRegistry_Halo3CameraOwnsAmbiguousState(
                  1050, 1000, 1000),
            "a heartbeat at the transition boundary is not new ownership");
        Check(TitleRegistry_Halo3CameraOwnsAmbiguousState(
                  1099, 1001, 1000),
            "a post-transition Halo 3 heartbeat owns a short ambiguous window");
        Check(!TitleRegistry_Halo3CameraOwnsAmbiguousState(
                  1101, 1001, 1000),
            "ambiguous Halo 3 ownership expires at the dedicated 100 ms limit");
        Check(!TitleRegistry_Halo3CameraOwnsAmbiguousState(
                  999, 1001, 1000),
            "a non-monotonic camera timestamp fails closed");

        OdstCameraRearmGate gate;
        Check(gate.CanAttemptInstall(), "ODST camera install begins unblocked");
        gate.BlockUntilReload(true);
        Check(!gate.CanAttemptInstall(),
            "stale active camera memory cannot immediately rearm ODST hooks");
        gate.Observe(true, true);
        Check(!gate.CanAttemptInstall(),
            "an active-to-active observation is not a reload edge");
        gate.Observe(true, false);
        Check(!gate.CanAttemptInstall(),
            "an inactive camera waits for the next active level");
        gate.Observe(true, true);
        Check(gate.CanAttemptInstall(),
            "inactive-to-active camera transition rearms ODST hooks");
        gate.BlockUntilReload(true);
        gate.Observe(false, false);
        Check(gate.CanAttemptInstall(),
            "a genuine title exit clears the ODST rearm gate");

        gate.BlockUntilTitleExit();
        gate.Observe(true, false);
        gate.Observe(true, true);
        Check(!gate.CanAttemptInstall(),
            "unsupported/menu camera transitions cannot rearm in-session");
        gate.Observe(false, false);
        Check(gate.CanAttemptInstall(),
            "title exit clears the unsupported-camera session latch");

        OdstPauseRearmGate pauseGate;
        const uint64_t pauseStable = kOdstPauseRearmStableMs;
        pauseGate.Block();
        pauseGate.Observe(100, true, true, true);
        Check(!pauseGate.CanAttemptInstall(),
            "native ODST pause blocks camera-hook reinstallation");
        pauseGate.Observe(200, true, false, true);
        pauseGate.Observe(200 + pauseStable, true, false, true);
        Check(!pauseGate.CanAttemptInstall(),
            "pause exit requires more than the pause-rearm stable interval");
        pauseGate.Observe(201 + pauseStable, true, false, true);
        Check(pauseGate.CanAttemptInstall(),
            "stable gameplay after pause exit permits camera-hook reinstall");
        // Flicker tolerance: with the copy hook removed the live camera array
        // reads ready/not-ready frame-to-frame. A momentary not-ready sample
        // must NOT restart the settle window (a continuous-ready reset stalled
        // the post-pause rearm for tens of seconds in the headset log).
        pauseGate.Block();
        pauseGate.Observe(5000, true, false, true);   // pause cleared, camera seen live
        pauseGate.Observe(5100, true, false, false);  // momentary not-ready flicker
        pauseGate.Observe(5000 + pauseStable, true, false, false);
        Check(!pauseGate.CanAttemptInstall(),
            "a not-ready flicker does not release the gate before the window");
        pauseGate.Observe(5001 + pauseStable, true, false, true);
        Check(pauseGate.CanAttemptInstall(),
            "a momentary flicker no longer restarts the pause-exit window");
        // A genuine re-pause, however, does restart the window from its exit.
        pauseGate.Block();
        pauseGate.Observe(6000, true, false, true);   // window starts at 6000
        pauseGate.Observe(6100, true, true, true);    // re-pause before it elapses
        Check(!pauseGate.CanAttemptInstall(),
            "a re-pause during the settle window blocks reinstall again");
        pauseGate.Observe(6200, true, false, true);   // window restarts at 6200
        pauseGate.Observe(6200 + pauseStable, true, false, true);
        Check(!pauseGate.CanAttemptInstall(),
            "the re-pause restarted the settle window from its exit");
        pauseGate.Observe(6201 + pauseStable, true, false, true);
        Check(pauseGate.CanAttemptInstall(),
            "a full settle window after the last pause exit clears the gate");
        pauseGate.Block();
        pauseGate.Observe(9000, false, false, false);
        Check(pauseGate.CanAttemptInstall(),
            "title exit clears the native-pause reinstall gate");

        const HudLayoutAdapter* halo3Hud =
            HudLayoutAdapterFor(HudLayoutProfile::Halo3);
        const HudLayoutAdapter* odstHud =
            HudLayoutAdapterFor(HudLayoutProfile::Halo3ODST);
        Check(halo3Hud && halo3Hud->expectedBlocks == 3,
            "Halo 3 HUD layout retains its three proven skin blocks");
        Check(odstHud && odstHud->expectedBlocks == 1,
            "ODST HUD layout accepts only its uniquely proven globals block");
        const HudLayoutAdapter* reachHud =
            HudLayoutAdapterFor(HudLayoutProfile::HaloReach);
        constexpr uint8_t expectedHalo3HudAnchor[24] = {
            0x00, 0x05, 0x00, 0x00, 0xD0, 0x02, 0x00, 0x00,
            0x00, 0x00, 0x5C, 0x42, 0x00, 0x40, 0x25, 0x44,
            0x00, 0x00, 0x68, 0x42, 0x00, 0x00, 0x80, 0x40,
        };
        constexpr uint8_t expectedOdstHudAnchor[24] = {
            0x00, 0x05, 0x00, 0x00, 0xD0, 0x02, 0x00, 0x00,
            0x00, 0x00, 0xFA, 0x44, 0x00, 0x00, 0xFA, 0x44,
            0x00, 0x00, 0x68, 0x42, 0x00, 0x00, 0x80, 0x40,
        };
        const auto anchorPrefixEquals = [](
            const HudLayoutAdapter* adapter, const uint8_t* expected, int n)
        {
            if (!adapter || adapter->anchorLength != n)
                return false;
            for (int i = 0; i < n; ++i)
            {
                if (adapter->anchor[i] != expected[i] ||
                    adapter->mask[i] != 0xFF)
                    return false;
            }
            return true;
        };
        Check(anchorPrefixEquals(halo3Hud, expectedHalo3HudAnchor, 24),
            "Halo 3 HUD adapter retains its evidence-exact anchor");
        Check(anchorPrefixEquals(odstHud, expectedOdstHudAnchor, 24),
            "ODST HUD adapter retains its evidence-exact 2000/2000 anchor");
        Check(halo3Hud && odstHud && halo3Hud->anchor != odstHud->anchor,
            "ODST cannot reuse Halo 3's title-specific safe-frame anchor");
        // Reach's record is a different shape, proven from HREK's own
        // chud_curvature_info_block postprocess and tag export. Copying Halo 3's
        // offsets would land 36 bytes short, inside the minimap points.
        Check(halo3Hud && odstHud && halo3Hud->safeFrameOffset == 24 &&
                  odstHud->safeFrameOffset == 24 &&
                  halo3Hud->depthFromSlot == -28 &&
                  odstHud->depthFromSlot == -28,
            "Halo 3 and ODST keep the shared s_chud_curvature_info offsets");
        Check(reachHud && reachHud->expectedBlocks == 3 &&
                  reachHud->safeFrameOffset == 60 &&
                  reachHud->anchorLength == 76,
            "Reach HUD adapter uses its own record shape, not Halo 3's");
        Check(reachHud && !HudLayoutHasDepthField(*reachHud),
            "Reach declares no HUD depth field: its curvature is baked at "
            "tag-block load, so writing it would be inert");
        Check(halo3Hud && odstHud && HudLayoutHasDepthField(*halo3Hud) &&
                  HudLayoutHasDepthField(*odstHud),
            "Halo 3 and ODST keep their live dest-offset-z depth control");
        Check(reachHud && reachHud->anchor[0] == 0x00 &&
                  reachHud->anchor[1] == 0x05 && reachHud->anchor[20] == 0x00 &&
                  reachHud->anchor[21] == 0x00 &&
                  reachHud->anchor[22] == 0xA0 &&
                  reachHud->anchor[23] == 0x42,
            "Reach compares its own vehicle-3d-sensor-radius field, which "
            "Halo 3's record does not contain at all");
        Check(reachHud && reachHud->mask[12] == 0x00 &&
                  reachHud->mask[16] == 0x00 && reachHud->mask[60] == 0x00 &&
                  reachHud->mask[64] == 0x00,
            "Reach wildcards its per-skin sensor values and the safe-frame "
            "pair this feature writes");
        Check(HudLayoutAdapterWellFormed(*halo3Hud) &&
                  HudLayoutAdapterWellFormed(*odstHud) &&
                  HudLayoutAdapterWellFormed(*reachHud),
            "every HUD layout adapter keeps a fully compared scan prefix");
        // Halo 3 and ODST keep their proven exact cardinality and their proven
        // private-read-write-only search. Only Reach may look wider.
        Check(HudLayoutAcceptedCountOk(*halo3Hud, 3) &&
                  !HudLayoutAcceptedCountOk(*halo3Hud, 2) &&
                  !HudLayoutAcceptedCountOk(*halo3Hud, 4),
            "Halo 3 still accepts exactly three layout blocks");
        Check(HudLayoutAcceptedCountOk(*odstHud, 1) &&
                  !HudLayoutAcceptedCountOk(*odstHud, 0) &&
                  !HudLayoutAcceptedCountOk(*odstHud, 2),
            "ODST still accepts exactly its one proven globals block");
        Check(HudLayoutAcceptedCountOk(*reachHud, 3) &&
                  HudLayoutAcceptedCountOk(*reachHud, 6) &&
                  !HudLayoutAcceptedCountOk(*reachHud, 2) &&
                  !HudLayoutAcceptedCountOk(*reachHud, 17),
            "Reach accepts a second identity-verified copy of its record but "
            "never fewer than its three skins");
        Check(!halo3Hud->scanMappedRegions && !odstHud->scanMappedRegions &&
                  reachHud->scanMappedRegions,
            "only Reach widens the search beyond private read-write memory");
        Check(!halo3Hud->forceWriteEveryPass && !odstHud->forceWriteEveryPass &&
                  reachHud->forceWriteEveryPass,
            "only Reach reasserts every pass, because it overwrites its own "
            "record live and a skip-when-matching write loses that race");
        Check(HudLayoutCanReacquireFromRemembered(1, 1) &&
                  HudLayoutCanReacquireFromRemembered(3, 3),
            "exact per-title remembered cardinality permits stock restoration");
        Check(!HudLayoutCanReacquireFromRemembered(0, 1) &&
                  !HudLayoutCanReacquireFromRemembered(1, 3) &&
                  !HudLayoutCanReacquireFromRemembered(0, 0),
            "missing, partial, and unproven remembered HUD sets fail closed");
        Check(!HudLayoutAdapterFor(HudLayoutProfile::None),
            "an unowned title has no writable HUD layout adapter");
        Check(!HudLayoutPublicationMatches(
                  HudLayoutProfile::None, 9,
                  HudLayoutProfile::None, 9),
            "matching generations cannot grant HUD ownership to no title");
        Check(HudLayoutPublicationMatches(
                  HudLayoutProfile::Halo3ODST, 9,
                  HudLayoutProfile::Halo3ODST, 9),
            "HUD scan results publish only to their exact title generation");
        Check(!HudLayoutPublicationMatches(
                  HudLayoutProfile::Halo3ODST, 9,
                  HudLayoutProfile::Halo3, 9) &&
                  !HudLayoutPublicationMatches(
                      HudLayoutProfile::Halo3ODST, 9,
                      HudLayoutProfile::Halo3ODST, 8),
            "foreign-title and stale-generation HUD results are rejected");

        Check(OdstHudLayoutEligible(true, true, true, true, false, false),
            "ODST starts shared HUD layout work on its first eligible camera heartbeat");
        Check(!OdstHudLayoutEligible(false, true, true, true, false, false) &&
                  !OdstHudLayoutEligible(true, false, true, true, false, false) &&
                  !OdstHudLayoutEligible(true, true, false, true, false, false) &&
                  !OdstHudLayoutEligible(true, true, true, false, false, false),
            "ODST HUD layout writes require private title ownership and a fresh camera");
        Check(!OdstHudLayoutEligible(true, true, true, true, true, false) &&
                  !OdstHudLayoutEligible(true, true, true, true, false, true),
            "teardown and native pause veto ODST HUD layout writes");

        OdstFreshCameraDebounce debounce;
        Check(!debounce.Update(100, true),
            "ODST camera does not arm on its first fresh frame");
        Check(!debounce.Update(1100, true),
            "ODST camera remains flat at the exact Halo 3 one-second boundary");
        Check(debounce.Update(1101, true),
            "ODST camera arms on the first millisecond after Halo 3 stability");
        debounce.Reset();
        Check(!debounce.Update(5000, true),
            "ODST session re-entry resets the stability debounce");

        OdstFreshCameraDebounce interruptedDebounce;
        Check(!interruptedDebounce.Update(2000, true) &&
                  !interruptedDebounce.Update(2500, false) &&
                  !interruptedDebounce.Update(3000, true),
            "loss of camera freshness restarts the ODST stability interval");
        Check(!interruptedDebounce.Update(4000, true),
            "the restarted interval remains flat at its exact one-second boundary");
        Check(interruptedDebounce.Update(4001, true),
            "only a continuous fresh-camera interval arms ODST after reset");

        // ODST's camera tail boolean toggles about ten times a second during
        // ordinary play. Restarting the interval on each toggle meant the
        // camera armed only when it caught a lucky quiet gap, which is why
        // arming was slow and a quick pause/unpause often never re-armed.
        OdstFreshCameraDebounce flickerDebounce;
        Check(!flickerDebounce.Update(1000, true),
            "the flicker-tolerant debounce still requires a stability interval");
        Check(!flickerDebounce.Update(1100, false) &&
                  !flickerDebounce.Update(1200, true) &&
                  !flickerDebounce.Update(1300, false) &&
                  !flickerDebounce.Update(1400, true),
            "a 100ms tail toggle does not arm ODST early");
        Check(flickerDebounce.Update(2001, true),
            "a flickering tail no longer restarts the interval, so ODST arms "
            "one second after the camera first became fresh");

        OdstFreshCameraDebounce sustainedLossDebounce;
        Check(!sustainedLossDebounce.Update(1000, true) &&
                  !sustainedLossDebounce.Update(1400, false),
            "a gap beyond the tolerance is a genuine camera loss");
        Check(!sustainedLossDebounce.Update(2001, true),
            "a genuine loss still restarts the full stability interval");

        Check(EvaluateOdstHeartbeat(1700, 1000, 0, false, false) ==
                  OdstHeartbeatAction::None,
            "ODST tolerates a short delay before its first heartbeat");
        Check(EvaluateOdstHeartbeat(1800, 1000, 0, false, false) ==
                  OdstHeartbeatAction::LevelUnloaded,
            "an inactive camera after install is treated as an unload");
        Check(EvaluateOdstHeartbeat(6100, 1000, 0, false, true) ==
                  OdstHeartbeatAction::NoFirstHeartbeat,
            "active-looking memory cannot retain a hook without a heartbeat");
        Check(EvaluateOdstHeartbeat(7000, 1000, 6200, true, true) ==
                  OdstHeartbeatAction::None,
            "a short heartbeat gap with a ready camera is tolerated");
        Check(EvaluateOdstHeartbeat(6701, 1000, 6000, true, false) ==
                  OdstHeartbeatAction::None,
            "a transient heartbeat gap does not detach presentation even when camera readiness flickers");
        Check(EvaluateOdstHeartbeat(6751, 1000, 6000, true, false) ==
                  OdstHeartbeatAction::LevelUnloaded,
            "an unready camera must exceed the soft timeout before presentation detaches");
        Check(EvaluateOdstHeartbeat(12001, 1000, 7000, true, true) ==
                  OdstHeartbeatAction::LevelUnloaded,
            "a hard heartbeat timeout falls back even with stale ready bytes");
    }

    const TitleDescriptor* halo3 = TitleRegistry_FromModuleName(L"halo3.dll");
    Check(halo3 != nullptr, "Halo 3 module is recognized");
    Check(halo3 && halo3->runtimeSupported, "Halo 3 is the supported baseline adapter");
    Check(halo3 && halo3->admissionCapabilities ==
              TitleCapability_ControllerInput,
        "Halo 3 retains ordinary shared-controller admission");

    const TitleDescriptor* odst =
        TitleRegistry_FromModuleName(L"N:/MCC/HALO3ODST.DLL");
    Check(odst && odst->title == GameTitle::Halo3ODST,
        "ODST paths are matched case-insensitively");
    Check(odst && !odst->runtimeSupported,
        "ODST stays disabled until its adapter passes the title gate");
    Check(odst && odst->capabilities == TitleCapability_None,
        "ODST advertises no public capabilities during private bring-up");
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    Check(odst && odst->admissionCapabilities ==
              TitleCapability_ControllerInput,
        "The private preset grants ODST controller admission");
#else
    Check(odst && odst->admissionCapabilities == TitleCapability_None,
        "The normal preset grants ODST no controller admission");
#endif

    Check(TitleRegistry_HookPlan(GameTitle::Halo3) == TitleHookPlan::Halo3Full,
        "Halo 3 keeps the full headset-confirmed hook plan");
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    Check(TitleRegistry_HookPlan(GameTitle::Halo3ODST) ==
              TitleHookPlan::OdstExperimentalCameraCore,
        "The explicit private build enables only the ODST camera core");
#else
    Check(TitleRegistry_HookPlan(GameTitle::Halo3ODST) == TitleHookPlan::None,
        "A normal build leaves ODST completely stock");
#endif

    const TitleDescriptor* reach = TitleRegistry_FromModuleName(L"haloreach.dll");
    Check(reach && reach->title == GameTitle::HaloReach, "Reach module is recognized");
    Check(reach && reach->runtimeSupported,
        "Reach is a permanent runtime-supported title");
    Check(reach && reach->capabilities ==
              (TitleCapability_Stereo | TitleCapability_ControllerAim |
               TitleCapability_ArmIk | TitleCapability_Hud |
               TitleCapability_RuntimeModes |
               TitleCapability_RoomScale |
               TitleCapability_ControllerInput | TitleCapability_Haptics),
        "Reach advertises controller aim and arm IK with its 3D motion core");
    Check(reach && (reach->capabilities &
              TitleCapability_Hud) != 0u,
        "Reach advertises native HUD capability now that its layout adapter "
        "locates Reach's own curvature record");
    Check(TitleRegistry_HookPlan(GameTitle::HaloReach) ==
              TitleHookPlan::ReachCameraCore,
        "Reach receives its permanent camera-core hook plan");
#if HALOMCCVR_EXPERIMENTAL_REACH_BRINGUP
    Check(reach && reach->admissionCapabilities ==
              TitleCapability_ControllerInput,
        "The private preset grants Reach only controller admission");
    Check(ReachAdapter_GetStage() == ReachAdapterStage::ControllerInputOnly,
        "The private preset compiles only Reach controller transport");
#else
    Check(reach && reach->admissionCapabilities == TitleCapability_None,
        "The normal preset grants Reach no controller admission");
    Check(ReachAdapter_GetStage() == ReachAdapterStage::Disabled,
        "The normal Release preset keeps the Reach adapter disabled");
#endif
    Check(!ReachAdapter_RuntimeHooksPermitted(),
        "Neither controller admission nor the hard-off render foundation can install Reach runtime hooks");
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    Check(HALOMCCVR_EXPERIMENTAL_REACH_BRINGUP == 1,
        "The hard-off Reach render foundation compiles only with controller admission retained");
#else
    Check(true,
        "The normal and controller-only presets omit the Reach render foundation");
#endif
    const ReachEvidenceIdentity& reachIdentity =
        ReachAdapter_GetEvidenceIdentity();
    Check(std::wstring_view(reachIdentity.moduleName) == L"haloreach.dll" &&
          std::string_view(reachIdentity.moduleSha256) ==
              "738DD2D24EA3AEA12E1EE9AA4A61094BF116027D42004C35A19E5048608B0894" &&
          reachIdentity.peTimestamp == 0x68A0EFE1u &&
          reachIdentity.sizeOfImage == 0x04EDA000u &&
          std::string_view(reachIdentity.hrekBuild) ==
              "2023.07.17.176677.1-QFE1",
        "The Reach adapter pins the independently verified retail and HREK identities");

    ReachHookProof proof{ true, 1, true, true, true, true, true, true };
    Check(ReachAdapter_HookProofComplete(proof),
        "A synthetic proof is complete only when every evidence gate is present");
    proof.loadedImageMatchCount = 0;
    Check(!ReachAdapter_HookProofComplete(proof),
        "A zero-match loaded-image signature fails the Reach proof closed");
    proof.loadedImageMatchCount = 2;
    Check(!ReachAdapter_HookProofComplete(proof),
        "A multiple-match loaded-image signature fails the Reach proof closed");
    proof.loadedImageMatchCount = 1;
    proof.executableRange = false;
    Check(!ReachAdapter_HookProofComplete(proof),
        "A non-executable candidate range fails the Reach proof closed");
    proof.executableRange = true;
    proof.abi = false;
    Check(!ReachAdapter_HookProofComplete(proof),
        "An unproven ABI fails the Reach proof closed");
    Check(!TitleRegistry_AllowsSharedGameplayFeatures(
              GameTitle::HaloReach, true, false),
        "Stale Halo 3 ownership cannot admit Reach gameplay features");
    const bool reachControllerAdmission = reach &&
        (reach->admissionCapabilities & TitleCapability_ControllerInput) != 0;
    Check(TitleRegistry_AllowsSharedControllerInput(
              GameTitle::HaloReach, true, false, true,
              reachControllerAdmission) == reachControllerAdmission,
        "Explicit Reach obeys its immutable controller admission policy");
    Check(!TitleRegistry_AllowsSharedControllerInput(
              GameTitle::HaloReach, false, true, false,
              reachControllerAdmission),
        "Camera-only ownership cannot leak controller input into Reach");
    const GameTitle unsupportedTitles[] = {
        GameTitle::Halo4, GameTitle::HaloCE,
        GameTitle::Halo2, GameTitle::Unknown, GameTitle::None,
    };
    for (GameTitle title : unsupportedTitles)
        Check(TitleRegistry_HookPlan(title) == TitleHookPlan::None,
            "Unsupported titles never receive game hooks");
    const GameTitle stockControllerTitles[] = {
        GameTitle::Halo4, GameTitle::HaloCE, GameTitle::Halo2,
    };
    for (GameTitle title : stockControllerTitles)
    {
        const TitleDescriptor* descriptor = TitleRegistry_Find(title);
        const bool admitted = descriptor &&
            (descriptor->admissionCapabilities &
                TitleCapability_ControllerInput) != 0;
        Check(!admitted && !TitleRegistry_AllowsSharedControllerInput(
                  title, false, false, true, admitted),
            "CE, H2, and H4 remain stock despite private title flags");
    }
    Check(TitleRegistry_FromModuleName(L"MCC-Win64-Shipping.exe") == nullptr,
        "The MCC host is not mistaken for a game title");
    Check(std::string_view(RuntimeModeName(RuntimeMode::Vehicle)) == "vehicle",
        "Runtime modes have stable diagnostic names");

    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    const std::filesystem::path configDir = std::filesystem::path(tempPath) /
        (L"halomccvr-config-tests-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(configDir);
    const std::filesystem::path primary = configDir / L"halomccvr.cfg";
    const std::filesystem::path legacy = configDir / L"halo3xr.cfg";
    {
        std::ofstream file(legacy);
        file << "screen_width_m = 6.25\n";
        file << "haptic_intensity = malformed\n";
    }
    ConfigLoadMigrating(primary.c_str(), legacy.c_str());
    Check(std::filesystem::exists(primary), "Legacy config migration creates halomccvr.cfg");
    Check(std::filesystem::exists(legacy), "Legacy config migration retains halo3xr.cfg");
    Check(g_config.screen_width_m == 6.25f, "Legacy values survive migration");
    Check(g_config.haptic_intensity == 0.86f,
        "Malformed new values retain their individual default");
    const Config inertiaDefaults{};
    Check(!g_config.weapon_inertia &&
          g_config.weapon_position_follow == inertiaDefaults.weapon_position_follow &&
          g_config.weapon_rotation_follow == inertiaDefaults.weapon_rotation_follow &&
          g_config.weapon_catchup_speed == inertiaDefaults.weapon_catchup_speed,
        "Older configs receive disabled weapon inertia with safe defaults");
    const std::string organizedConfig = ReadTextFile(primary);
    const size_t openXrSection = organizedConfig.find("#  OPENXR & COMFORT");
    const size_t controlsSection = organizedConfig.find("#  CONTROLS & TURNING");
    const size_t aimingSection = organizedConfig.find("#  RETICLE & AIMING");
    const size_t weaponSection = organizedConfig.find("#  WEAPON CALIBRATION");
    const size_t scopeSection = organizedConfig.find("#  EXPERIMENTAL SCOPE");
    const size_t displaySection = organizedConfig.find("#  HUD, PRESENTATION & PERFORMANCE");
    const size_t handsSection = organizedConfig.find("#  GAMEPLAY, HANDS & IK");
    const size_t diagnosticsSection = organizedConfig.find("#  DEVELOPMENT DIAGNOSTICS");
    Check(openXrSection < controlsSection && controlsSection < aimingSection &&
          aimingSection < weaponSection && weaponSection < scopeSection &&
          scopeSection < displaySection && displaySection < handsSection &&
          handsSection < diagnosticsSection,
        "The generated universal config has stable, readable section ordering");
    Check(organizedConfig.find("This ONE file is shared by every supported MCC game") !=
              std::string::npos,
        "The generated config explains that preferences are shared across titles");
    Check(organizedConfig.find("\nreach_") == std::string::npos &&
          organizedConfig.find("\nhalo3_") == std::string::npos &&
          organizedConfig.find("\nodst_") == std::string::npos,
        "The universal config contains no title-prefixed assignments");
    constexpr const char* universalKeys[] = {
        "config_version", "haptic_intensity", "headset_smoothing",
        "aim_stabilization", "weapon_inertia", "weapon_position_follow",
        "weapon_rotation_follow", "weapon_catchup_speed",
        "screen_width_m", "screen_distance_m",
        "turn_smooth", "turn_snap_deg", "turn_smooth_deg_s", "dpad_hand",
        "crosshair", "crosshair_distance_m", "crosshair_size_deg",
        "reticle_r", "reticle_g", "reticle_b", "kill_reticle",
        "gun_scale", "left_hand_scale", "gun_pitch_deg", "gun_yaw_deg",
        "gun_roll_deg", "gun_forward_m", "muzzle_height_m",
        "scope_enabled", "scope_zoom",
        "scope_screen_width_m", "scope_screen_right_m", "scope_screen_up_m",
        "scope_screen_forward_m", "scope_refresh_divisor", "game_brightness",
        "resolution_scale", "upscale_filter", "sharpness", "aa_mode",
        "hud_size", "hud_aspect", "hud_curvature",
        "hud_vertical_offset", "motion_blur", "auto_vr", "two_handed_aim",
        "two_hand_toggle", "left_hand_forward_m", "two_hand_zone_right_m",
        "left_grip_forward_m", "arm_ik", "floating_hands",
        "right_shoulder_drop", "shoulder_level", "body_wip", "weapon_probe",
        "hud_probe", "fsr_probe", "bullet_probe", "right_eye_first"
    };
    for (const char* key : universalKeys)
    {
        const std::string assignment = std::string("\n") + key + " = ";
        const std::string message = std::string("Generated config writes exactly one '") +
            key + "' assignment";
        Check(CountText(organizedConfig, assignment) == 1, message.c_str());
    }
    {
        std::ofstream file(primary);
        file << "config_version = 1\n";
        file << "haptic_intensity = 2.0\n";
        file << "headset_smoothing = 1.0\n";
        file << "aim_stabilization = -1.0\n";
        file << "aa_mode = 4\n";
    }
    Check(organizedConfig.find("weapon_max_lag_m") == std::string::npos &&
          organizedConfig.find("weapon_max_lag_deg") == std::string::npos,
        "Generated config retires hard weapon pose limits");

    ConfigLoad(primary.c_str());
    Check(g_config.haptic_intensity == 1.0f, "Haptic intensity is safely clamped");
    Check(g_config.headset_smoothing == 0.10f,
        "Headset smoothing is capped at the low-latency maximum");
    Check(g_config.aim_stabilization == 0.0f, "Aim stabilization is safely clamped");
    Check(g_config.aa_mode == 4,
        "SMAA 1x plus FXAA Strong survives config loading");

    {
        std::ofstream file(primary);
        file << "weapon_inertia = 1\n";
        file << "weapon_position_follow = -5\n";
        file << "weapon_rotation_follow = 99\n";
        file << "weapon_catchup_speed = 2\n";
        file << "weapon_max_lag_m = 0.5\n";
        file << "weapon_max_lag_deg = 99\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.weapon_inertia &&
          g_config.weapon_position_follow == 2.0f &&
          g_config.weapon_rotation_follow == 45.0f &&
          g_config.weapon_catchup_speed == 1.0f,
        "Weapon inertia config values parse while retired pose limits are ignored");
    g_config.weapon_position_follow = 12.5f;
    g_config.weapon_rotation_follow = 20.5f;
    g_config.weapon_catchup_speed = 0.65f;
    ConfigSave();
    ConfigLoad(primary.c_str());
    Check(g_config.weapon_inertia &&
          g_config.weapon_position_follow == 12.5f &&
          g_config.weapon_rotation_follow == 20.5f &&
          g_config.weapon_catchup_speed == 0.65f,
        "Weapon inertia settings survive a save/reload round trip");
    {
        std::ofstream file(primary);
        file << "weapon_position_follow = nan\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.weapon_position_follow == inertiaDefaults.weapon_position_follow,
        "Malformed weapon inertia values retain their individual defaults");

    // resolution_scale is free-form: a hand-typed value must survive exactly,
    // not snap to one of the six installer tiers (the pre-2026-07-20 behavior).
    {
        std::ofstream file(primary);
        file << "resolution_scale = 0.90\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.resolution_scale == 0.90f,
        "A custom resolution scale is honored exactly, not snapped to a preset");
    ConfigSave();
    const std::string savedResolutionConfig = ReadTextFile(primary);
    Check(CountText(savedResolutionConfig, "resolution_scale = 0.90") == 1,
        "Organized config keeps the launcher's resolution_scale line compatible");
    ConfigLoad(primary.c_str());
    Check(g_config.resolution_scale == 0.90f,
        "A custom resolution scale survives a save/reload round trip");
    {
        std::ofstream file(primary);
        file << "resolution_scale = 0.05\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.resolution_scale == kResolutionScaleMin,
        "A too-small resolution scale is pulled up to the minimum");
    {
        std::ofstream file(primary);
        file << "resolution_scale = 5.0\n";
    }
    ConfigLoad(primary.c_str());
    Check(!UpdateTwoHandHold(false, true, false) &&
          UpdateTwoHandHold(false, true, true) &&
          UpdateTwoHandHold(true, true, false) &&
          !UpdateTwoHandHold(true, false, true), __func__);

    Check(g_config.resolution_scale == kResolutionScaleMax,
        "A too-large resolution scale is pulled down to the maximum");

    {
        std::ofstream file(primary);
        file << "hud_height = 1.25\n"; // compatibility alias from the first test build
        file << "hud_aspect = 1.35\n";
        file << "hud_vertical_offset = -125\n";
    }
    ConfigLoad(primary.c_str());
    const float migratedCurvature = (0.30f - 1.25f * 0.1f) / 0.60f;
    Check(std::abs(g_config.hud_curvature - migratedCurvature) < 0.0001f &&
          g_config.hud_aspect == 1.35f &&
          g_config.hud_vertical_offset == -125.0f,
        "Legacy HUD curvature, aspect trim, and height migrate exactly");
    ConfigSave();
    ConfigLoad(primary.c_str());
    Check(std::abs(g_config.hud_curvature - migratedCurvature) < 0.01f &&
          g_config.hud_aspect == 1.35f &&
          g_config.hud_vertical_offset == -125.0f,
        "Normalized HUD curvature, aspect, and height survive save/reload");
    {
        std::ofstream file(primary);
        file << "config_version = 2\n";
        file << "hud_curvature = 99\n";
        file << "hud_aspect = 99\n";
        file << "hud_vertical_offset = -999\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.hud_curvature == kHudCurvatureMax &&
          g_config.hud_aspect == kHudAspectMax &&
          g_config.hud_vertical_offset == kHudHeightMin,
        "Excessive HUD curvature, aspect, and height values are safely clamped");

    {
        std::ofstream file(primary);
        file << "config_version = 2\n";
        file << "scope_zoom = 3.39\n";
    }
    ConfigLoad(primary.c_str());
    Check(std::fabs(g_config.scope_zoom - 11.865f) < 1e-4f,
        "Version 2 scope zoom migrates into the tighter gameplay-origin lens");
    ConfigSave();
    ConfigLoad(primary.c_str());
    Check(std::fabs(g_config.scope_zoom - 11.87f) < 1e-4f,
        "Migrated scope zoom is not strengthened again after saving version 4");

    {
        std::ofstream file(primary);
        file << "scope_enabled = 1\n";
        file << "scope_zoom = 99\n";
        file << "scope_screen_width_m = 1\n";
        file << "scope_screen_right_m = -1\n";
        file << "scope_screen_up_m = 1\n";
        file << "scope_screen_forward_m = 0\n";
        file << "scope_refresh_divisor = 99\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.scope_enabled && g_config.scope_zoom == 24.0f &&
          g_config.scope_screen_width_m == 0.25f &&
          g_config.scope_screen_right_m == -0.30f &&
          g_config.scope_screen_up_m == 0.30f &&
          g_config.scope_screen_forward_m == 0.05f &&
          g_config.scope_refresh_divisor == 4,
        "Universal scope settings are safely clamped");
    g_config.scope_zoom = 8.25f;
    g_config.scope_screen_width_m = 0.12f;
    g_config.scope_screen_right_m = 0.03f;
    g_config.scope_screen_up_m = 0.09f;
    g_config.scope_screen_forward_m = 0.35f;
    g_config.scope_refresh_divisor = 3;
    ConfigSave();
    ConfigLoad(primary.c_str());
    Check(g_config.scope_zoom == 8.25f &&
          g_config.scope_screen_width_m == 0.12f &&
          g_config.scope_screen_right_m == 0.03f &&
          g_config.scope_screen_up_m == 0.09f &&
          g_config.scope_screen_forward_m == 0.35f &&
          g_config.scope_refresh_divisor == 3,
        "Universal scope settings survive a save/reload round trip");

    // Deleting the file is the documented "put everything back" escape hatch.
    {
        std::ofstream file(primary);
        file << "gun_scale = 2.5\n";
    }
    ConfigLoad(primary.c_str());
    Check(g_config.gun_scale == 2.5f, "A tuned value loads before the reset test");
    std::filesystem::remove(primary);
    ConfigLoad(primary.c_str());
    Check(std::filesystem::exists(primary),
        "Deleting the config file recreates it on the next load");
    const Config defaults{};
    Check(g_config.gun_scale == defaults.gun_scale &&
          g_config.resolution_scale == defaults.resolution_scale &&
          g_config.hud_size == defaults.hud_size &&
          g_config.hud_aspect == defaults.hud_aspect &&
          g_config.hud_curvature == defaults.hud_curvature &&
          g_config.hud_vertical_offset == defaults.hud_vertical_offset &&
          g_config.scope_enabled &&
          g_config.scope_zoom == defaults.scope_zoom &&
          !g_config.weapon_inertia &&
          g_config.weapon_position_follow == defaults.weapon_position_follow &&
          g_config.weapon_rotation_follow == defaults.weapon_rotation_follow &&
          g_config.weapon_catchup_speed == defaults.weapon_catchup_speed &&
          g_config.scope_screen_width_m == defaults.scope_screen_width_m &&
          g_config.scope_screen_right_m == defaults.scope_screen_right_m &&
          g_config.scope_screen_up_m == defaults.scope_screen_up_m &&
          g_config.scope_screen_forward_m == defaults.scope_screen_forward_m &&
          g_config.scope_refresh_divisor == defaults.scope_refresh_divisor,
        "The recreated config file carries the struct defaults");
    std::filesystem::remove_all(configDir);

    MenuChordDetector chord;
    MenuChordResult chordResult = chord.Update(1000, true, false);
    Check(!chordResult.toggled, "One stick click does not toggle the menu");
    chordResult = chord.Update(1249, true, true);
    Check(chordResult.toggled && chordResult.consumeClicks,
        "L3+R3 toggles inside the 250 ms window and consumes both clicks");
    Check(!chord.Update(1300, true, true).toggled,
        "A held chord toggles only once");
    Check(chord.Update(1350, false, true).consumeClicks,
        "Chord clicks stay consumed until both are released");
    chord.Update(1400, false, false);
    chord.Update(2000, true, false);
    Check(!chord.Update(2251, true, true).toggled,
        "A chord outside the 250 ms window does not toggle");
    chord.Update(2300, false, false);
    Check(chord.Update(2400, true, true).toggled,
        "A simultaneous chord works after release");

    ScopeToggleDetector scopeToggle;
    Check(!scopeToggle.Update(true, true, false).changed,
        "R3 press arms the scope without toggling early");
    ScopeToggleUpdate scopeResult = scopeToggle.Update(true, false, false);
    Check(scopeResult.changed && scopeResult.active,
        "R3 release opens the universal scope");
    scopeToggle.Update(true, true, false);
    scopeResult = scopeToggle.Update(true, false, false);
    Check(scopeResult.changed && !scopeResult.active,
        "A second R3 click closes the universal scope");

    scopeToggle.Reset();
    scopeToggle.Update(true, true, false);      // R3 begins first
    scopeToggle.Update(true, true, true);       // L3 joins; menu consumes chord
    scopeResult = scopeToggle.Update(true, false, true);
    Check(!scopeResult.changed && !scopeResult.active,
        "A staggered L3+R3 menu chord cancels the pending scope toggle");
    scopeToggle.Reset();
    scopeToggle.Update(true, true, true);       // simultaneous menu chord
    scopeResult = scopeToggle.Update(true, false, true);
    Check(!scopeResult.changed && !scopeResult.active,
        "A simultaneous L3+R3 menu chord never toggles the scope");

    scopeToggle.Update(true, true, false);
    scopeToggle.Update(true, false, false);
    scopeResult = scopeToggle.Update(false, false, false);
    Check(scopeResult.changed && !scopeResult.active,
        "Losing gameplay or disabling the feature resets an active scope");
    scopeToggle.Update(false, true, false);
    scopeToggle.Update(true, true, false);
    scopeResult = scopeToggle.Update(true, false, false);
    Check(!scopeResult.changed && !scopeResult.active,
        "R3 held across gameplay entry cannot cause a surprise toggle");

    const float identity[4] = {0, 0, 0, 1};
    const float scopeOrigin[3] = {1, 2, 3};
    const ScopeQuadTransform quad = ComputeScopeQuadTransform(
        identity, scopeOrigin, 0.04f, 0.08f, 0.30f, 0.10f);
    Check(std::fabs(quad.position[0] - 1.04f) < 1e-5f &&
          std::fabs(quad.position[1] - 2.08f) < 1e-5f &&
          std::fabs(quad.position[2] - 2.70f) < 1e-5f,
        "Scope offsets follow the gun's local right/up/forward axes");
    Check(std::fabs(quad.width - 0.10f) < 1e-5f &&
          std::fabs(quad.height - 0.075f) < 1e-5f,
        "Scope is fixed-size 4:3 geometry independent of headset distance");

    const float gameBasis[9] = {
        1, 0, 0,  // forward
        0, 1, 0,  // left
        0, 0, 1}; // up
    const float safeCameraOrigin[3] = {4.0f, 5.0f, 6.0f};
    const float bulletForward[3] = {1.0f, 0.02f, -0.01f};
    ScopeCameraPose scopeCamera{};
    Check(ComputeScopeCameraPose(gameBasis, safeCameraOrigin,
                                 bulletForward, scopeCamera),
        "Scope camera accepts valid gameplay-camera and bullet inputs");
    Check(std::fabs(scopeCamera.position[0] - 4.0f) < 1e-5f &&
          std::fabs(scopeCamera.position[1] - 5.0f) < 1e-5f &&
          std::fabs(scopeCamera.position[2] - 6.0f) < 1e-5f,
        "Scope camera keeps Halo's collision-safe gameplay origin");
    const float bulletLength = std::sqrt(1.0f + 0.02f * 0.02f + 0.01f * 0.01f);
    Check(std::fabs(scopeCamera.forward[0] - bulletForward[0] / bulletLength) < 1e-5f &&
          std::fabs(scopeCamera.forward[1] - bulletForward[1] / bulletLength) < 1e-5f &&
          std::fabs(scopeCamera.forward[2] - bulletForward[2] / bulletLength) < 1e-5f,
        "Remote scope center continues along Halo's actual bullet direction");

    const ScopeProjectionTangents scopeLens =
        ComputeScopeProjectionTangents(2.5f, 16.0f / 9.0f);
    Check(std::fabs(scopeLens.horizontal / scopeLens.vertical - 16.0f / 9.0f) < 1e-5f,
        "Scope render projection matches its source surface before cropping");
    const float croppedHorizontal = scopeLens.horizontal * (4.0f / 3.0f) / (16.0f / 9.0f);
    Check(std::fabs(croppedHorizontal / scopeLens.vertical - 4.0f / 3.0f) < 1e-5f &&
          std::fabs(croppedHorizontal - 0.70020754f / 2.5f) < 1e-5f,
        "Center-cropped scope image is an undistorted 4:3 2.5x lens");

    ScopeRefreshScheduler refresh;
    Check(!refresh.Advance(true, 2) && refresh.Advance(true, 2),
        "Scope refresh divisor 2 renders every second active frame");
    Check(!refresh.Advance(false, 2) && !refresh.Advance(true, 2) &&
          refresh.Advance(true, 2),
        "Closing the scope resets its image refresh schedule");
    Check(refresh.Advance(true, 0),
        "Scope refresh divisor clamps safely to one");

    ScopeZoomController runtimeZoom;
    Check(std::fabs(runtimeZoom.Update(true, 1.0f, 0.5f, 8.0f) - 8.0f) < 1e-5f,
        "Opening the scope restores configured zoom before applying stick input");
    Check(std::fabs(runtimeZoom.Update(true, 1.0f, 0.10f, 8.0f) - 9.0f) < 1e-5f,
        "Right-stick up increases runtime scope zoom");
    Check(std::fabs(runtimeZoom.Update(true, -1.0f, 0.10f, 8.0f) - 8.0f) < 1e-5f,
        "Right-stick down decreases runtime scope zoom");
    Check(std::fabs(runtimeZoom.Update(true, 0.15f, 0.10f, 8.0f) - 8.0f) < 1e-5f,
        "Scope zoom ignores right-stick deadzone noise");
    runtimeZoom.Update(false, 0.0f, 0.0f, 8.0f);
    Check(std::fabs(runtimeZoom.Update(true, 0.0f, 0.0f, 12.0f) - 12.0f) < 1e-5f,
        "Reopening the scope resets temporary zoom to the configured default");

    ScopeZoomResolver zoomResolver;
    zoomResolver.RequestToggle();
    Check(!zoomResolver.Update(true, false) && zoomResolver.Update(true, false),
        "A weapon with no native zoom opens the fallback scope after detection");
    zoomResolver.RequestToggle();
    Check(zoomResolver.Update(true, false) && !zoomResolver.Update(true, false),
        "A second non-zoom weapon click closes the fallback scope");
    zoomResolver.Reset();
    Check(zoomResolver.Update(true, true),
        "Halo native zoom immediately owns scope visibility");
    zoomResolver.RequestToggle();
    Check(zoomResolver.Update(true, true),
        "A second authored zoom stage keeps the scope visible");
    zoomResolver.RequestToggle();
    Check(!zoomResolver.Update(true, false) && !zoomResolver.Update(true, false),
        "Leaving Halo native zoom closes without enabling fallback");
    zoomResolver.Reset();
    zoomResolver.Update(true, true);
    Check(!zoomResolver.Update(true, false),
        "Native zoom can close before its R3 release is observed");
    zoomResolver.RequestToggle();
    Check(!zoomResolver.Update(true, false) && !zoomResolver.Update(true, false),
        "The late release from native zoom-off is not mistaken for fallback");

    PauseLevelRecovery pauseRecovery;
    Check(!pauseRecovery.Update(true, false, false),
        "Pause recovery stays armed during an ordinary pause");
    Check(!pauseRecovery.Update(true, true, false),
        "Pause recovery records loading without resuming early");
    Check(pauseRecovery.Update(true, false, true),
        "Pause recovery restores 3D when restarted level becomes stable");
    Check(!pauseRecovery.Update(true, false, true),
        "Pause recovery fires only once per loading gap");
    pauseRecovery.Update(true, true, false);
    Check(!pauseRecovery.Update(false, false, true),
        "Leaving pause resets an incomplete restart recovery");

    const float rayOrigin[3] = { 0.0f, 0.0f, 0.0f };
    const float rayForward[3] = { 0.0f, 0.0f, -1.0f };
    MenuPointerHit hit = IntersectMenuQuad(rayOrigin, rayForward,
        1.2f, 1.1f, 0.825f, -0.08f);
    Check(hit.hit && hit.u == 0.5f, "Forward controller ray hits the menu center column");
    const float rayAway[3] = { 0.0f, 0.0f, 1.0f };
    Check(!IntersectMenuQuad(rayOrigin, rayAway, 1.2f, 1.1f, 0.825f, -0.08f).hit,
        "Controller rays pointing away from the menu miss");
    Check(BlendXInputMotors(0, 0) == 0.0f, "Zero XInput rumble stops haptics");
    Check(BlendXInputMotors(65535, 65535) == 1.0f,
        "Both full XInput motors produce full portable haptics");
    const float lowOnly = BlendXInputMotors(65535, 0);
    const float highOnly = BlendXInputMotors(0, 65535);
    Check(lowOnly > highOnly && highOnly > 0.0f,
        "Both motor bands contribute to the blended haptic amplitude");

    Check(NormalizeVirtualXInputSetStateResult(
              ERROR_DEVICE_NOT_CONNECTED, 0, true) == ERROR_SUCCESS,
        "Virtual slot 0 stays connected when title policy suppresses haptics");
    Check(NormalizeVirtualXInputSetStateResult(
              ERROR_DEVICE_NOT_CONNECTED, 1, true) ==
              ERROR_DEVICE_NOT_CONNECTED,
        "Foreign XInput slots preserve the underlying SetState result");

    // Peak-hold haptics: a gunfire pulse that rose and fell between two VR
    // frame samples must still fire once, a sustained rumble must persist,
    // and out-of-range inputs must clamp.
    const HapticPeakSample onePulse = SampleHapticPeak(0.8f, 0.0f);
    Check(onePulse.apply == 0.8f,
        "A rumble pulse that already returned to zero still applies its peak");
    Check(onePulse.carry == 0.0f,
        "A one-shot rumble pulse does not persist after it is applied");
    const HapticPeakSample sustained = SampleHapticPeak(0.3f, 0.6f);
    Check(sustained.apply == 0.6f && sustained.carry == 0.6f,
        "A sustained rumble above the stale peak applies and carries forward");
    const HapticPeakSample clamped = SampleHapticPeak(1.5f, -0.5f);
    Check(clamped.apply == 1.0f && clamped.carry == 0.0f,
        "Peak-hold haptic samples clamp to the [0,1] amplitude range");
    Check(NormalizeVirtualXInputSetStateResult(
              ERROR_DEVICE_NOT_CONNECTED, 0, false) ==
              ERROR_DEVICE_NOT_CONNECTED,
        "An invalid vibration request preserves the underlying SetState result");

    Check(!IsMaterialFramePeriodTransition(8333333, 8333334),
        "OpenXR nanosecond period jitter does not trigger a pacing capture");
    Check(IsMaterialFramePeriodTransition(8333333, 16666667),
        "A 120-to-60 runtime cadence change triggers a pacing capture");
    Check(IsMaterialFramePeriodTransition(11111111, 22222222),
        "A 90-to-45 runtime cadence change triggers a pacing capture");
    Check(IsMaterialFramePeriodTransition(6944444, 13888889),
        "A 144-to-72 runtime cadence change triggers a pacing capture");
    Check(!IsMaterialFramePeriodTransition(8333333, 8403361),
        "A small runtime-period adjustment does not masquerade as a cadence flip");
    Check(ShouldReleaseFrameWaitWorkerBeforeBegin(true, true),
        "A claimed worker packet releases the next wait before xrBeginFrame");
    Check(!ShouldReleaseFrameWaitWorkerBeforeBegin(true, false),
        "A missing worker packet never releases another wait before xrBeginFrame");
    Check(!ShouldReleaseFrameWaitWorkerBeforeBegin(false, true),
        "No wait-worker release occurs when the worker is unavailable");
    Check(ClassifyFrameWaitPermit(7, 6) == FrameWaitPermit::Park,
        "The worker remains parked until its exact packet sequence is released");
    Check(ClassifyFrameWaitPermit(7, 7) == FrameWaitPermit::StartNextWait,
        "An exact sequence acknowledgement permits one subsequent wait");
    Check(ClassifyFrameWaitPermit(7, 8) == FrameWaitPermit::Fault &&
          ClassifyFrameWaitPermit(0, 0) == FrameWaitPermit::Fault,
        "Skipped or invalid wait generations fault instead of issuing a wait");
    Check(IsExpectedNextFrameWaitDispatch(7, 8),
        "Begin admission observes the exact subsequent worker dispatch");
    Check(!IsExpectedNextFrameWaitDispatch(7, 7) &&
          !IsExpectedNextFrameWaitDispatch(7, 9) &&
          !IsExpectedNextFrameWaitDispatch(0, 1) &&
          !IsExpectedNextFrameWaitDispatch(UINT64_MAX, 0),
        "Begin admission rejects stale, skipped, invalid, or wrapped dispatches");

    // Reach observer-camera head-lock: the call-site classifier decides which
    // consumers get taken off the hand. Getting the world-render index wrong
    // would double-apply head-look to the accepted Reach 3D path.
    Check(ReachClassifyObserverCameraReturn(0x0026C2DE) ==
              kReachObserverCameraWorldSite,
        "The world render camera return address maps to the world site");
    Check(ReachClassifyObserverCameraReturn(0x002E1525) ==
              kReachObserverCameraChudSite,
        "The CHUD projection return address maps to the CHUD site");
    Check(kReachObserverCameraWorldSite != kReachObserverCameraChudSite,
        "The world and CHUD sites are distinct");
    Check(ReachClassifyObserverCameraReturn(0x0025B404) == 0 &&
          ReachClassifyObserverCameraReturn(0x0025D406) == 1 &&
          ReachClassifyObserverCameraReturn(0x0026FA47) == 3 &&
          ReachClassifyObserverCameraReturn(0x0026FB0E + 5) == 4,
        "Every measured observer-camera call site maps to its own index");
    Check(ReachClassifyObserverCameraReturn(0x0026C2DD) < 0 &&
          ReachClassifyObserverCameraReturn(0x0026C2DF) < 0 &&
          ReachClassifyObserverCameraReturn(0) < 0,
        "An off-by-one or unknown return address is never classified as a site");
    {
        bool collision = false;
        for (int a = 0; a < 6; ++a)
            for (int b = a + 1; b < 6; ++b)
                if (kReachObserverCameraReturnRvas[a] ==
                    kReachObserverCameraReturnRvas[b])
                    collision = true;
        Check(!collision,
            "The six observer-camera return addresses are all distinct");
    }

    // Reach second muzzle flash: only a WORLD location on an effect that
    // declares a first-person weapon user is redirected onto the gun. Getting
    // this wrong would relocate damage or object-spawn points.
    {
        // fp mask 1, output user 0 -> a real first-person weapon effect.
        const ReachEffectFpDecision world =
            ReachDecideEffectLocation(0x01, 0x0007);
        Check(world.redirect && world.userIndex == 0 &&
              world.markerIndex == 0x0007,
            "A world location on a first-person weapon effect is redirected");
        const ReachEffectFpDecision alreadyFp =
            ReachDecideEffectLocation(0x01, 0x8007);
        Check(!alreadyFp.redirect,
            "A location already flagged first-person is left to the engine");
        Check(!ReachDecideEffectLocation(0x00, 0x0007).redirect,
            "An effect with no first-person weapon user is never redirected");
        Check(!ReachDecideEffectLocation(0xF1, 0x0007).redirect,
            "An effect whose first-person output user is 'none' is not "
            "redirected");
        Check(!ReachDecideEffectLocation(0x01, 0xFFFE).redirect &&
              !ReachDecideEffectLocation(0x01, 0xFFFF).redirect,
            "The engine's own -2/-1 designator cases are left untouched");
        const ReachEffectFpDecision user2 =
            ReachDecideEffectLocation(0x21, 0x0123);
        Check(user2.redirect && user2.userIndex == 2 &&
              user2.markerIndex == 0x0123,
            "The first-person output user index is decoded from the high nibble");
    }

    // Reach muzzle retarget: every first-person system off the majority
    // marker is moved onto it; ties break toward the lower location index
    // (primary_trigger is index 0 in all six affected weapons).
    {
        // AR: round@0, smoke@0, long_brake@2, glow@0, all mode 1 -> one move.
        const unsigned short arM[] = {1, 1, 1, 1};
        const unsigned short arL[] = {0, 0, 2, 0};
        const ReachMuzzleRetargetPlan ar = ReachDecideMuzzleRetarget(arM, arL, 4);
        Check(ar.count == 1 && ar.elements[0] == 2 && ar.newLocation == 0,
            "The AR's odd first-person system is retargeted onto its siblings'");
        // DMR: two odd systems at location 2 -> both move.
        const unsigned short dmrM[] = {1, 1, 1, 1, 1};
        const unsigned short dmrL[] = {2, 2, 0, 0, 0};
        const ReachMuzzleRetargetPlan dmr =
            ReachDecideMuzzleRetarget(dmrM, dmrL, 5);
        Check(dmr.count == 2 && dmr.newLocation == 0 &&
              dmr.elements[0] == 0 && dmr.elements[1] == 1,
            "Both DMR odd systems are retargeted");
        // Sniper: 4 at 0 vs 4 at 1 - tie breaks to the lower index.
        const unsigned short snM[] = {1, 1, 1, 1, 1, 1, 1, 1};
        const unsigned short snL[] = {0, 0, 1, 0, 1, 0, 1, 1};
        const ReachMuzzleRetargetPlan sn = ReachDecideMuzzleRetarget(snM, snL, 8);
        Check(sn.count == 4 && sn.newLocation == 0,
            "A tie between locations resolves to the lower index");
        // Uniform event: nothing to do.
        const unsigned short uniM[] = {1, 1, 1, 1, 1};
        const unsigned short uniL[] = {2, 2, 2, 2, 2};
        Check(ReachDecideMuzzleRetarget(uniM, uniL, 5).count == 0,
            "An event whose systems share one location is never touched");
        // Mode-0 systems at other markers never make a weapon eligible.
        const unsigned short othM[] = {0, 0, 1, 1, 1};
        const unsigned short othL[] = {1, 5, 0, 0, 0};
        Check(ReachDecideMuzzleRetarget(othM, othL, 5).count == 0,
            "Mode-0 systems at other markers never make a weapon eligible");
        // No agreeing pair -> ambiguous -> untouched.
        const unsigned short thinM[] = {1, 1, 0};
        const unsigned short thinL[] = {0, 2, 0};
        Check(ReachDecideMuzzleRetarget(thinM, thinL, 3).count == 0,
            "A single sibling is not a majority; nothing is moved");
    }

    if (g_failures == 0)
        std::cout << "HaloMCCVR core tests passed\n";
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
