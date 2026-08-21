#pragma once

#include <cstddef>
#include <cstdint>

// D-H2-1. Read-only render-topology census for Halo 2.
//
// Static analysis established that halo2.dll contains two renderers and that
// the classic tree is skipped whole while the remastered Anniversary renderer
// owns the frame. It could NOT establish two things that per-eye stereo needs:
// which frame function actually executes in the mode the player uses, and which
// D3D11 resource holds the finished image when it returns.
//
// This census answers both from a live process. Every detour calls the engine's
// original first and only increments an atomic afterwards; no engine field is
// written, no render target is redirected, and no behavior changes. It is a
// diagnostic, never part of an accepted candidate.
//
// It is deliberately independent of OpenXR: it installs on the title worker
// after the C-H2-1 cold-observation proof and reports regardless of whether a
// VR session exists, so the same build can be read on a monitor or in a headset.
bool Halo2RenderProbe_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed) noexcept;

bool Halo2RenderProbe_Installed() noexcept;
