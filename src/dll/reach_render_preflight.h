#pragma once

#include <cstddef>
#include <cstdint>

#include "../common/reach_render_logic.h"

enum class ReachLoadedImageFailure : uint8_t
{
    None = 0,
    InvalidInput,
    ModuleReference,
    BackingFileIdentity,
    BackingFileUnreadable,
    PeIdentity,
    ExecutableSections,
    SignatureIdentity,
    BodyIdentity,
    CallerEdges,
    FixedRanges,
    MappingChanged,
    Publication,
};

struct ReachLoadedImagePreflight
{
    ReachRenderCandidateProof proof{};
    ReachLoadedImageFailure failure = ReachLoadedImageFailure::InvalidInput;
};

class ReachLoadedImageModulePin
{
public:
    ReachLoadedImageModulePin() noexcept = default;
    ~ReachLoadedImageModulePin();
    ReachLoadedImageModulePin(const ReachLoadedImageModulePin&) = delete;
    ReachLoadedImageModulePin& operator=(
        const ReachLoadedImageModulePin&) = delete;

    bool Valid() const noexcept;
    bool IsCurrent(uintptr_t expectedBase) const noexcept;

private:
    bool Acquire(uintptr_t expectedBase) noexcept;
    void Reset() noexcept;

    void* m_module = nullptr;

    friend bool ReachRender_RunLoadedImagePreflight(
        uintptr_t moduleBase, size_t moduleSize,
        ReachLoadedImagePreflight& result,
        ReachLoadedImageModulePin& pin);
};

// Worker-thread-only loaded-image proof. The historical Pin type now retains
// only a non-owning exact-base identity through the final mapping check and
// publication; MCC alone owns the module refcount. No write or hook occurs.
bool ReachRender_RunLoadedImagePreflight(
    uintptr_t moduleBase, size_t moduleSize,
    ReachLoadedImagePreflight& result,
    ReachLoadedImageModulePin& pin);

const char* ReachRender_LoadedImageFailureName(
    ReachLoadedImageFailure failure) noexcept;

// SHA-256 the last preflight actually read off disk, so a rejected build names
// itself in the log instead of only saying that it did not match. Empty when
// the file could not be read at all. Worker-thread-only, like the preflight.
const char* ReachRender_LastBackingFileSha256() noexcept;
