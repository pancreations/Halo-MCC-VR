#pragma once

#include <cstddef>
#include <string_view>

#include "runtime_types.h"

struct TitleDescriptor
{
    GameTitle title;
    const wchar_t* moduleName;
    const char* displayName;
    bool runtimeSupported;
    uint32_t capabilities;
};

const TitleDescriptor* TitleRegistry_All(size_t& count);
const TitleDescriptor* TitleRegistry_Find(GameTitle title);
const TitleDescriptor* TitleRegistry_FromModuleName(std::wstring_view moduleName);
const char* RuntimeModeName(RuntimeMode mode);
