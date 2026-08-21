#pragma once

// Simple thread-safe file logger. Everything the mod does gets written to
// halo3xr.log next to the DLL so users can send us the file when something
// goes wrong on a machine we can't see.

void LogInit(const wchar_t* filePath);
// Directory of the log file (trailing separator included), for evidence
// files written next to it. Empty until LogInit ran.
const wchar_t* LogDirectory();
void Logf(const char* fmt, ...);

#define LOG(...) Logf(__VA_ARGS__)
