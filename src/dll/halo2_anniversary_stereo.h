#pragma once

#include <cstddef>
#include <cstdint>

// C-H2-10. Per-eye stereo for Halo 2's remastered (Anniversary / Saber) renderer.
//
// The classic core cannot serve this mode: the runtime census proved that with
// Anniversary graphics selected, render_player_window is never called at all,
// while the Saber scene render 0x2DF190 runs once per frame.
//
// 0x2DF190 is the one entry that is safe to run twice. The retail engine
// already calls it once per player window, on the same thread, inside a single
// hold of its critical section, and it never blocks. 0x2DEC00 ends in a
// blocking wait, and 0x2DC3D0 is a frame boundary that bumps a global frame
// counter eleven other sites read - neither may be doubled.
//
// The camera the scene render actually consumes is NOT the Saber camera object.
// 0x2DDCB0 byte-copies that object once per frame into a per-view record and
// 0x1C6D80 bakes the derived matrices into the record; from then on the object
// is dead for rendering. Byte-verified at 0x2DF2C5:
//     record = *(halo2 + 0x1A250F8) + 0x150 + viewIndex * 0x758
//     embedded camera copy at record + 0x20
// So each eye is applied by writing that embedded camera and re-running the
// engine's own rebuild, never by inventing matrices.
bool Halo2AnniversaryStereo_Poll(
    uintptr_t moduleBase, size_t moduleSize, uint32_t generation,
    bool activeAndRange, bool levelRunning, bool coldPassed,
    bool remasteredRendererLive, uintptr_t observerResultArray) noexcept;

bool Halo2AnniversaryStereo_Installed() noexcept;
bool Halo2AnniversaryStereo_Armed() noexcept;
uint32_t Halo2AnniversaryStereo_Generation() noexcept;
void Halo2AnniversaryStereo_RequestRecenter() noexcept;
void Halo2AnniversaryStereo_ShutdownForVrFailure() noexcept;
