#pragma once
#include <cstdint>
#include <cstddef>

// AOB (array-of-bytes) signature scanning. We locate game code/data by the
// bytes of the surrounding instructions rather than hardcoded addresses, so
// the mod keeps working when an MCC update shifts everything around. When a
// pattern stops matching after a patch, we detect it and warn instead of
// crashing (that's the whole reason we scan).

namespace sig
{
    // Pattern is space-separated hex bytes with "??" as a wildcard, e.g.
    //   "48 8B 05 ?? ?? ?? ?? 48 85 C0"
    // Returns the address of the first match inside [base, base+size), or 0.
    uintptr_t Find(uintptr_t base, size_t size, const char* pattern);

    // Same, but over a loaded module's image (by name, e.g. L"halo3.dll").
    // Returns 0 if the module isn't loaded or the pattern isn't found.
    uintptr_t FindInModule(const wchar_t* moduleName, const char* pattern);

    // Resolve a 32-bit RIP-relative reference (the addressing mode x64 uses for
    // things like `mov rax,[rip+disp32]`). dispAddr points at the 4-byte
    // displacement; nextInstr is the address of the instruction that follows
    // (RIP's value when the displacement is applied). Returns the target.
    uintptr_t RipTarget(uintptr_t dispAddr, uintptr_t nextInstr);

    // Base address and size of a loaded module's image, for bounding a scan.
    bool ModuleRange(const wchar_t* moduleName, uintptr_t& base, size_t& size);
}
