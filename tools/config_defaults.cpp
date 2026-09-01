#include <windows.h>

#include "../src/common/config.h"

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2 || !argv[1] || !argv[1][0])
        return 2;
    if (GetFileAttributesW(argv[1]) != INVALID_FILE_ATTRIBUTES)
        return 3;

    ConfigLoad(argv[1]);
    const DWORD attributes = GetFileAttributesW(argv[1]);
    return attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & FILE_ATTRIBUTE_DIRECTORY)
        ? 0
        : 4;
}
