#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include "log.h"

static FILE* g_file = nullptr;
static CRITICAL_SECTION g_cs;

void LogInit(const wchar_t* filePath)
{
    InitializeCriticalSection(&g_cs);
    // Keep the previous run's log as <name>.prev so a failed relaunch (which
    // truncates the log) doesn't erase the evidence from the run before it.
    std::wstring prev(filePath);
    prev += L".prev";
    MoveFileExW(filePath, prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    _wfopen_s(&g_file, filePath, L"wt");
}

void Logf(const char* fmt, ...)
{
    if (!g_file)
        return;
    EnterCriticalSection(&g_cs);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
    LeaveCriticalSection(&g_cs);
}
