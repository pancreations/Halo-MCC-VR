#include "reach_render_candidate.h"

#include <windows.h>

#include "../common/log.h"
#include "reach_render_preflight.h"

#ifndef HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
#define HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE 0
#endif

static_assert(HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE == 1,
    "reach_render_candidate.cpp must only compile in the selected scaffold preset");
static_assert(kReachMainRenderViewAob.size() == 32);
static_assert(kReachPlayerViewRenderAob.size() == 69);
static_assert(kReachFrustumHelperAob.size() == 25);

namespace
{
    ReachPreflightPublication g_preflightPublication;

    // Written only by Game's 50 ms title worker.
    ReachModuleEpoch g_attemptedEpoch{};
    bool g_attempted = false;
}

bool ReachRenderCandidate_Compiled() noexcept
{
    return true;
}

bool ReachRenderCandidate_RuntimeHooksEnabled() noexcept
{
    // This milestone deliberately has no MinHook installation path. Flipping
    // the compile option cannot make a Reach callback reachable.
    return false;
}

void ReachRenderCandidate_ColdPoll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool soleReachTitle) noexcept
{
    const ReachModuleEpoch epoch{moduleBase, generation};
    if (!soleReachTitle || !ReachModuleEpochValid(epoch) ||
        moduleSize != kReachRetailImageSize)
    {
        const bool hadAttempt = g_attempted;
        const bool hadComplete = g_preflightPublication.HasCurrent();
        g_attempted = false;
        g_attemptedEpoch = {};
        g_preflightPublication.Invalidate();
        if (hadAttempt || hadComplete)
            LOG("Reach render cold proof invalidated; stock Reach remains active");
        return;
    }

    if (g_attempted && ReachSameModuleEpoch(g_attemptedEpoch, epoch))
        return;
    g_attempted = true;
    g_attemptedEpoch = epoch;
    g_preflightPublication.Invalidate();

    ReachLoadedImagePreflight result{};
    ReachLoadedImageModulePin modulePin;
    bool complete = false;
    try
    {
        complete = ReachRender_RunLoadedImagePreflight(
            moduleBase, moduleSize, result, modulePin);
    }
    catch (...)
    {
        result.failure = ReachLoadedImageFailure::InvalidInput;
        complete = false;
    }

    // Generation is worker-owned and cannot change until the next
    // TitleAdapter_PollLoaded call on this thread. The non-owning HMODULE
    // identity remains in scope through this final mapping check/publication.
    if (complete && !modulePin.IsCurrent(moduleBase))
    {
        complete = false;
        result.failure = ReachLoadedImageFailure::MappingChanged;
    }

    if (complete &&
        !g_preflightPublication.Publish(epoch, result.proof))
    {
        complete = false;
        result.failure = ReachLoadedImageFailure::Publication;
    }
    if (!complete)
        g_preflightPublication.Invalidate();
    if (complete)
    {
        LOG("Reach render cold preflight PASS: exact retail image, unique "
            "executable signatures, exact camera and FP wrapper bodies, "
            "caller edges, and fixed ranges; runtime hooks remain disabled");
    }
    else
    {
        // Name the digest that was rejected. "backing-file-identity" alone does
        // not say whether the file was unreadable or simply a build this table
        // does not describe, and that ambiguity cost real diagnosis time.
        const char* observed = ReachRender_LastBackingFileSha256();
        LOG("Reach render cold preflight FAIL (%s): main=%u inner=%u "
            "frustum=%u; module sha256 %s; stock Reach remains active",
            ReachRender_LoadedImageFailureName(result.failure),
            result.proof.mainRenderViewMatchCount,
            result.proof.playerViewRenderMatchCount,
            result.proof.frustumHelperMatchCount,
            (observed && observed[0]) ? observed : "(not read)");
    }
}

ReachPreflightToken ReachRenderCandidate_GetPreflight(
    const ReachModuleEpoch& epoch) noexcept
{
    return g_preflightPublication.Get(epoch);
}

bool ReachRenderCandidate_IsPreflightCurrent(
    const ReachPreflightToken& token) noexcept
{
    return g_preflightPublication.IsCurrent(token);
}

ReachRenderAction ReachRenderCandidate_SelectAction(
    const ReachPreflightToken& preflight,
    const ReachRenderOwnerGate& owner,
    const ReachRenderOwnerToken& token,
    const ReachDirectCopyGate& directCopyGate,
    const ReachInnerRenderInput& input) noexcept
{
    return SelectReachRenderAction(
        ReachRenderCandidate_RuntimeHooksEnabled(),
        preflight, owner, token, directCopyGate, input);
}
