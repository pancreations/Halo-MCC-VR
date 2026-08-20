#pragma once

#include <cstddef>
#include <cstdint>

// Halo 2-only render evidence and cold-observation policy. This header is
// deliberately pure: no Windows APIs, logging, allocation, hooks, or engine
// writes. Every address and layout below is derived from the official H2EK and
// verified against both pinned retail halo2.dll images; see
// docs/HALO2-SIGNATURE-EVIDENCE.md.

inline constexpr size_t kHalo2RetailFileSize = 15807960;
inline constexpr size_t kHalo2RetailImageSize = 0x02A38000;
inline constexpr uint32_t kHalo2RetailPeTimestamp = 0x68A0F0F2;

inline constexpr const char* kHalo2RetailModuleSha256[] = {
    // Steam
    "DE65B4F4FDBF3F0A5EAB7431FE530DA17DD815599182DFD6AE9B7E21CF171946",
    // Microsoft Store / Xbox app (Game Pass)
    "81E5F41A7F8409D27A5454A28BFBECB8CD273E389366FB9865DD1D01E6BE689D",
};

inline constexpr char kHalo2KitBuildTag[] =
    "2023.06.20.176294.1-Release";
inline constexpr char kHalo2KitTagTestSha256[] =
    "D0B71186D3948C48DDD02E2CCB88FA13E77E25A3D8F7FA60922F23A2A0073E36";

// The engine stores one heap-allocated 0x2C-byte game_time_globals object
// behind this module pointer slot. The official H2EK establishes the layout;
// retail's unique incrementer and level initializer independently decode the
// same slot.
inline constexpr uint32_t kHalo2GameTimeSlotRva = 0x015FE008;
inline constexpr uint32_t kHalo2GameTimeGlobalsSize = 0x2C;
inline constexpr uint32_t kHalo2GameTimeInitializedOffset = 0x00;
inline constexpr uint32_t kHalo2GameTimeTickRateOffset = 0x02;
inline constexpr uint32_t kHalo2GameTimeSecondsPerTickOffset = 0x04;
inline constexpr uint32_t kHalo2GameTimeCurrentTickOffset = 0x08;

// Retail camera/window facts retained now because one of the cold anchors is
// the native asymmetric-frustum helper. C-H2-1 does not consume or write these
// fields; they define the evidence-backed route for the next stereo candidate.
inline constexpr uint32_t kHalo2RetailWindowStride = 0x120;
inline constexpr uint32_t kHalo2RenderCameraOffset = 0x0C;
inline constexpr uint32_t kHalo2RasterCameraOffset = 0x80;
inline constexpr uint32_t kHalo2CameraBytes = 0x74;
inline constexpr uint32_t kHalo2CameraAsymmetricEnableOffset = 0x58;
inline constexpr uint32_t kHalo2CameraFrustumCenterXOffset = 0x5C;
inline constexpr uint32_t kHalo2CameraFrustumCenterYOffset = 0x60;
inline constexpr uint32_t kHalo2CameraFrustumExtentScaleOffset = 0x64;
inline constexpr uint32_t kHalo2CameraPixelOffsetEnableOffset = 0x68;
inline constexpr uint32_t kHalo2CameraPixelOffsetXOffset = 0x6C;
inline constexpr uint32_t kHalo2CameraPixelOffsetYOffset = 0x70;

struct Halo2RetailAnchor
{
    const char* name;
    const char* pattern;
    uint32_t rva;
    uint8_t relativeDispOffset;
    uint32_t relativeTargetRva;
};

inline constexpr size_t kHalo2AnchorGameTimeIncrement = 0;
inline constexpr size_t kHalo2AnchorGameTimeInit = 1;
inline constexpr size_t kHalo2AnchorRenderFrame = 2;
inline constexpr size_t kHalo2AnchorPlayerWindow = 3;
inline constexpr size_t kHalo2AnchorRenderView = 4;
inline constexpr size_t kHalo2AnchorAsymmetricFrustum = 5;

// Every pattern matched exactly once over each complete mapped retail image.
// A nonzero relativeDispOffset names a disp32 inside the match; its instruction
// ends at offset+4, so the same decode covers RIP-relative data operands.
inline constexpr Halo2RetailAnchor kHalo2RetailAnchors[] = {
    { "game-time-increment",
      "48 8B 05 ?? ?? ?? ?? FF 40 08 C3",
      0x7067F0, 0x03, kHalo2GameTimeSlotRva },
    { "game-time-level-init",
      "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 33 C9 48 89 08 48 89 48 08 48 "
      "89 48 10 48 89 48 18 48 89 48 20 89 48 28 E8 ?? ?? ?? ?? 48 8B "
      "15 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 0F BF 48 08 66 89 4A 02 "
      "C7 42 0C 00 00 80 3F C6 02 01",
      0x706910, 0x07, kHalo2GameTimeSlotRva },
    { "render-frame",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
      "41 56 48 83 EC 40 41 8B F9 48 63 EA 41 8B F0 8B D9",
      0x7E1600, 0, 0 },
    { "render-player-window",
      "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 "
      "02 00 00",
      0x7E2130, 0, 0 },
    { "render-view",
      "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 44 88 4C 24 20 "
      "55 41 54 41 55 41 56 41 57 48 8D AC 24 B0 FD FF FF 48 81 EC 50 "
      "03 00 00",
      0x7E30D0, 0, 0 },
    { "asymmetric-frustum-helper",
      "48 89 5C 24 08 57 48 83 EC 40 0F BF 41 3A 48 8B F9 44 0F BF 49 "
      "3C 48 8B DA 0F BF 51 32 44 0F BF 41 3E",
      0x7DFCD0, 0, 0 },
};

inline constexpr size_t kHalo2RetailAnchorCount =
    sizeof(kHalo2RetailAnchors) / sizeof(kHalo2RetailAnchors[0]);

constexpr uint32_t Halo2RetailAnchorRelativeTargetCount()
{
    uint32_t count = 0;
    for (const Halo2RetailAnchor& anchor : kHalo2RetailAnchors)
        if (anchor.relativeDispOffset != 0)
            ++count;
    return count;
}

inline constexpr uint32_t kHalo2RetailAnchorRelativeTargets =
    Halo2RetailAnchorRelativeTargetCount();

struct Halo2ColdObservationResult
{
    bool moduleRangeValid = false;
    bool peIdentity = false;
    uint32_t anchorsMatchedOnce = 0;
    uint32_t anchorsAtPinnedRva = 0;
    uint32_t relativeTargetsAtPinnedRva = 0;
    bool postInitializationTickObserved = false;
    bool mappingStable = false;
};

constexpr bool Halo2ColdObservationPass(
    const Halo2ColdObservationResult& result)
{
    return result.moduleRangeValid && result.peIdentity &&
        result.anchorsMatchedOnce == kHalo2RetailAnchorCount &&
        result.anchorsAtPinnedRva == kHalo2RetailAnchorCount &&
        result.relativeTargetsAtPinnedRva ==
            kHalo2RetailAnchorRelativeTargets &&
        result.postInitializationTickObserved && result.mappingStable;
}

constexpr bool Halo2ColdObservationNeedsImageScan(
    bool completedForModuleInstance)
{
    return !completedForModuleInstance;
}

// Pure decision core for the read-only game-time liveness gate. The false->true
// initialized transition is only a new baseline: it is never mistaken for a
// game tick. A later different tick proves game_update reached the official
// active-update tail. `!=` is deliberate: the official code permits save-state
// restoration and uint32 wrap, neither of which invalidates the clock. If
// observation starts mid-level, six seconds of uninterrupted tick changes is
// the conservative already-running path used by the established title gates.
class Halo2GameTimeGateLogic
{
public:
    enum class Decision : uint8_t
    {
        Hold = 0,
        OpenAfterBoundaryThenTick,
        OpenAlreadyRunning,
    };

    // Six seconds at the title worker's 50 ms cadence. Kept equal to the
    // established gate, but implemented independently because H2's fast path
    // accepts ONLY the engine's explicit uninitialized lifecycle state; an
    // unchanged initialized clock is not allowed to manufacture that boundary.
    static constexpr uint32_t kAlreadyRunningSamples = 120;

    Decision Observe(bool initialized, uint32_t tick)
    {
        if (!initialized)
        {
            // halo2.dll normally remains resident while MCC leaves a level.
            // Process the engine's explicit level-dispose state even after a
            // prior open so liveness cannot remain falsely latched in menus or
            // carry across a later load.
            m_sawUninitialized = true;
            m_haveInitializedSample = false;
            m_changeRun = 0;
            ++m_stillRun;
            m_open = false;
            m_lastDecision = Decision::Hold;
            return Decision::Hold;
        }
        if (m_open)
            return m_lastDecision;
        if (!m_haveInitializedSample)
        {
            m_haveInitializedSample = true;
            m_tick = tick;
            return Decision::Hold;
        }
        const bool changed = tick != m_tick;
        m_tick = tick;
        if (!changed)
        {
            m_changeRun = 0;
            ++m_stillRun;
            return Decision::Hold;
        }
        m_stillRun = 0;
        if (m_sawUninitialized)
        {
            m_open = true;
            m_lastDecision = Decision::OpenAfterBoundaryThenTick;
            return m_lastDecision;
        }
        if (++m_changeRun >= kAlreadyRunningSamples)
        {
            m_open = true;
            m_lastDecision = Decision::OpenAlreadyRunning;
            return m_lastDecision;
        }
        return Decision::Hold;
    }

    // A null/unreadable/racy engine sample must not manufacture a still frame.
    // Preserve prior genuine frozen evidence, but force the next readable
    // initialized value to become a new baseline.
    void InvalidateSample()
    {
        m_haveInitializedSample = false;
        // The already-running proof is explicitly consecutive. A missing,
        // unreadable, or incoherent sample breaks that run even when a prior
        // real uninitialized lifecycle boundary remains valid evidence.
        m_changeRun = 0;
        m_stillRun = 0;
        m_open = false;
        m_lastDecision = Decision::Hold;
    }

    void Reset()
    {
        m_tick = 0;
        m_changeRun = 0;
        m_stillRun = 0;
        m_haveInitializedSample = false;
        m_sawUninitialized = false;
        m_open = false;
        m_lastDecision = Decision::Hold;
    }

    bool IsOpen() const { return m_open; }
    bool SawUninitialized() const { return m_sawUninitialized; }
    uint32_t ChangeRun() const { return m_changeRun; }
    uint32_t StillRun() const { return m_stillRun; }

private:
    uint32_t m_tick = 0;
    uint32_t m_changeRun = 0;
    uint32_t m_stillRun = 0;
    bool m_haveInitializedSample = false;
    bool m_sawUninitialized = false;
    bool m_open = false;
    Decision m_lastDecision = Decision::Hold;
};
