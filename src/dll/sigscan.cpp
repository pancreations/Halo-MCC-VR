#include <windows.h>
#include <psapi.h>
#include <vector>
#include <cstring>
#include "sigscan.h"

namespace
{
    // Parse "48 8B ?? C0" into parallel arrays: byte values, and a mask that
    // says which positions are wildcards.
    bool ParsePattern(const char* pattern, std::vector<uint8_t>& bytes, std::vector<bool>& wild)
    {
        for (const char* p = pattern; *p;)
        {
            if (*p == ' ')
            {
                p++;
                continue;
            }
            if (p[0] == '?')
            {
                bytes.push_back(0);
                wild.push_back(true);
                p++;
                if (*p == '?')
                    p++;
            }
            else
            {
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int hi = hex(p[0]);
                const int lo = hex(p[1]);
                if (hi < 0 || lo < 0)
                    return false;
                bytes.push_back((uint8_t)((hi << 4) | lo));
                wild.push_back(false);
                p += 2;
            }
        }
        return !bytes.empty();
    }
}

namespace sig
{
    uintptr_t Find(uintptr_t base, size_t size, const char* pattern)
    {
        std::vector<uint8_t> bytes;
        std::vector<bool> wild;
        if (!ParsePattern(pattern, bytes, wild))
            return 0;
        const size_t n = bytes.size();
        if (size < n)
            return 0;

        const uint8_t* data = reinterpret_cast<const uint8_t*>(base);
        const size_t last = size - n;
        for (size_t i = 0; i <= last; i++)
        {
            size_t j = 0;
            for (; j < n; j++)
                if (!wild[j] && data[i + j] != bytes[j])
                    break;
            if (j == n)
                return base + i;
        }
        return 0;
    }

    bool ModuleRange(const wchar_t* moduleName, uintptr_t& base, size_t& size)
    {
        HMODULE mod = GetModuleHandleW(moduleName);
        if (!mod)
            return false;
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
            return false;
        base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        size = mi.SizeOfImage;
        return true;
    }

    uintptr_t FindInModule(const wchar_t* moduleName, const char* pattern)
    {
        uintptr_t base = 0;
        size_t size = 0;
        if (!ModuleRange(moduleName, base, size))
            return 0;
        return Find(base, size, pattern);
    }

    uintptr_t RipTarget(uintptr_t dispAddr, uintptr_t nextInstr)
    {
        const int32_t disp = *reinterpret_cast<const int32_t*>(dispAddr);
        return nextInstr + disp;
    }
}
