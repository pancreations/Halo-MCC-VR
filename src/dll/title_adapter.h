#pragma once

#include <cstdint>
#include "../common/title_registry.h"

// Per-title weapon-tag schema. Keeping these offsets behind the adapter makes
// it impossible to accidentally apply Halo 3's layout to ODST when that title
// is enabled later. Modded weapons work automatically because the values are
// read from the live held-weapon tag, not from a name table.
struct WeaponZoomTagLayout
{
    bool supported = false;
    uint32_t levelCountOffset = 0;
    uint32_t minimumZoomOffset = 0;
    uint32_t maximumZoomOffset = 0;
    int maximumLevels = 0;
};

const TitleDescriptor* TitleAdapter_PollLoaded();
const TitleDescriptor* TitleAdapter_GetActive();
WeaponZoomTagLayout TitleAdapter_GetWeaponZoomTagLayout();
void TitleAdapter_SetRuntimeMode(RuntimeMode mode);
RuntimeMode TitleAdapter_GetRuntimeMode();
