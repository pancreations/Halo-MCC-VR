#pragma once

#include "../common/title_registry.h"

const TitleDescriptor* TitleAdapter_PollLoaded();
const TitleDescriptor* TitleAdapter_GetActive();
GameTitle TitleAdapter_GetActiveTitle();
uint64_t TitleAdapter_GetActiveTitleEpochMs();
void TitleAdapter_SetRuntimeMode(RuntimeMode mode);
RuntimeMode TitleAdapter_GetRuntimeMode();
