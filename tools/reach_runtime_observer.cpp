// Standalone Halo: Reach runtime evidence observer.
//
// This executable is intentionally separate from halo3xr.dll. It requests only
// process-query and process-memory-read rights, never injects code, never
// installs a hook, and never writes to MCC. Run it only against an
// anti-cheat-disabled MCC session.

#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "../src/common/reach_observer_logic.h"

#ifndef HALOMCCVR_BUILD_COMMIT
#define HALOMCCVR_BUILD_COMMIT "unknown"
#endif

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
    constexpr wchar_t kMccExecutable[] = L"MCC-Win64-Shipping.exe";
    constexpr wchar_t kReachModule[] = L"haloreach.dll";
    // The mod DLL was renamed to HaloMCCVR.dll; the contamination guard must
    // keep refusing under either name or it silently stops guarding.
    constexpr wchar_t kInjectedModModule[] = L"HaloMCCVR.dll";
    constexpr wchar_t kLegacyInjectedModModule[] = L"halo3xr.dll";
    constexpr DWORD kProcessAccess =
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
    static_assert(
        (kProcessAccess &
         (PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD |
          PROCESS_SUSPEND_RESUME)) == 0);

    constexpr char kExpectedRetailSha256[] =
        "738DD2D24EA3AEA12E1EE9AA4A61094BF116027D42004C35A19E5048608B0894";
    constexpr uint32_t kExpectedTimestamp = 0x68A0EFE1u;
    constexpr uint32_t kExpectedImageSize = 0x04EDA000u;

    constexpr uint32_t kMainRenderViewRva = 0x000C31F4u;
    constexpr uint32_t kMainRenderViewSize = 515u;
    constexpr char kMainRenderViewSha256[] =
        "95DF3EFFF9AC6EE29887D1272CCA8D7BF3E58F87041BAD8032107825B733FE89";
    constexpr std::array<uint8_t, 32> kMainRenderViewEntry = {
        0x40, 0x53, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x80,
        0x00, 0x00, 0x00, 0x0F, 0x29, 0x74, 0x24, 0x70,
        0x48, 0x8B, 0x05, 0x05, 0x6E, 0xA3, 0x00, 0x48,
        0x33, 0xC4, 0x48, 0x89, 0x44, 0x24, 0x68, 0x41,
    };
    constexpr uint32_t kFrustumBoundsRva = 0x00287F58u;
    constexpr std::array<uint8_t, 25> kFrustumBoundsEntry = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x44, 0x0F,
        0xBF, 0x49, 0x62, 0x4C, 0x8B, 0xD9, 0x4C, 0x8B,
        0x41, 0x38, 0x48, 0x8B, 0xDA, 0x0F, 0xBF, 0x51,
        0x50,
    };

    constexpr uint32_t kPlayerViewArrayRva = 0x029F2B90u;
    constexpr size_t kPlayerViewStride = 0xA40;
    constexpr size_t kPlayerViewCount = 4;
    constexpr uint32_t kRasterizerWorkspaceRva = 0x00C9FAE0u;
    constexpr size_t kRasterizerWorkspaceSize = 0x2A8;
    constexpr size_t kCompactCameraSize = 0x90;
    constexpr size_t kSecondaryCompactOffset = 0x154;
    constexpr uint32_t kActiveViewGlobalRva = 0x04E389A8u;

    constexpr uint64_t kModulePollMs = 100;
    constexpr uint64_t kSummaryMs = 10000;

    struct ExpectedCall
    {
        uint32_t siteRva;
        uint32_t targetRva;
        const char* name;
    };

    constexpr ExpectedCall kExpectedCalls[] = {
        { 0x000C36D6u, 0x0026C204u, "normal setup" },
        { 0x000C3730u, 0x000C31F4u, "normal main_render_view" },
        { 0x001D3864u, 0x000C31F4u, "alternate main_render_view" },
        { 0x000C33C4u, 0x0026C6DCu, "player_view_render" },
        { 0x000C2FAAu, 0x000C33F8u, "outer main render" },
        { 0x000C3000u, 0x0025113Cu, "stock Present wrapper" },
    };

    constexpr std::wstring_view kTitleModules[] = {
        L"halo3.dll", L"halo3odst.dll", L"haloreach.dll",
        L"halo4.dll", L"halo1.dll", L"halo2.dll",
    };

    std::atomic<bool> g_stopRequested{ false };

    BOOL WINAPI ConsoleControlHandler(DWORD control)
    {
        if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT ||
            control == CTRL_CLOSE_EVENT)
        {
            g_stopRequested.store(true, std::memory_order_release);
            return TRUE;
        }
        return FALSE;
    }

    class Reporter
    {
    public:
        ~Reporter()
        {
            if (m_file)
                std::fclose(m_file);
        }

        bool OpenRelative(
            HANDLE directory, const std::wstring& leaf)
        {
            if (!directory || directory == INVALID_HANDLE_VALUE ||
                leaf.empty() ||
                leaf.size() >
                    std::numeric_limits<USHORT>::max() /
                        sizeof(wchar_t))
                return false;

            UNICODE_STRING name{};
            name.Length = static_cast<USHORT>(
                leaf.size() * sizeof(wchar_t));
            name.MaximumLength = name.Length;
            name.Buffer = const_cast<PWSTR>(leaf.c_str());
            OBJECT_ATTRIBUTES attributes{};
            InitializeObjectAttributes(
                &attributes, &name, OBJ_CASE_INSENSITIVE,
                directory, nullptr);
            IO_STATUS_BLOCK statusBlock{};
            HANDLE file = nullptr;
            const NTSTATUS status = NtCreateFile(
                &file, GENERIC_WRITE | SYNCHRONIZE, &attributes,
                &statusBlock, nullptr, FILE_ATTRIBUTE_NORMAL,
                FILE_SHARE_READ, FILE_CREATE,
                FILE_NON_DIRECTORY_FILE |
                    FILE_SYNCHRONOUS_IO_NONALERT |
                    FILE_OPEN_REPARSE_POINT,
                nullptr, 0);
            if (status < 0 || !file)
                return false;
            const int descriptor = _open_osfhandle(
                reinterpret_cast<intptr_t>(file),
                _O_WRONLY | _O_TEXT);
            if (descriptor == -1)
            {
                CloseHandle(file);
                return false;
            }
            m_file = _wfdopen(descriptor, L"wt");
            if (!m_file)
            {
                _close(descriptor);
                return false;
            }
            return true;
        }

        void Line(const char* format, ...)
        {
            char message[2048]{};
            va_list args;
            va_start(args, format);
            vsnprintf_s(
                message, sizeof(message), _TRUNCATE, format, args);
            va_end(args);

            SYSTEMTIME time{};
            GetLocalTime(&time);
            char line[2304]{};
            snprintf(
                line, sizeof(line), "[%02u:%02u:%02u.%03u] %s\n",
                time.wHour, time.wMinute, time.wSecond,
                time.wMilliseconds, message);
            std::fputs(line, stdout);
            std::fflush(stdout);
            if (m_file)
            {
                std::fputs(line, m_file);
                std::fflush(m_file);
            }
        }

    private:
        FILE* m_file = nullptr;
    };

    class MillisecondWaiter
    {
    public:
        MillisecondWaiter()
        {
            m_timer = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
            if (!m_timer)
                m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }

        ~MillisecondWaiter()
        {
            if (m_timer)
                CloseHandle(m_timer);
        }

        void Wait(DWORD milliseconds)
        {
            if (m_timer)
            {
                LARGE_INTEGER due{};
                due.QuadPart =
                    -static_cast<LONGLONG>(milliseconds) * 10000;
                if (SetWaitableTimer(
                        m_timer, &due, 0, nullptr, nullptr, FALSE))
                {
                    WaitForSingleObject(m_timer, INFINITE);
                    return;
                }
            }
            Sleep(milliseconds);
        }

    private:
        HANDLE m_timer = nullptr;
    };

    bool AddOffset(uintptr_t base, size_t offset, uintptr_t& result)
    {
        if (offset > std::numeric_limits<uintptr_t>::max() - base)
            return false;
        result = base + offset;
        return true;
    }

    bool ReadRemote(
        HANDLE process, uintptr_t address, void* destination, size_t size)
    {
        if (!destination || !size ||
            size > std::numeric_limits<uintptr_t>::max() - address)
            return false;
        SIZE_T received = 0;
        return ReadProcessMemory(
                   process, reinterpret_cast<const void*>(address),
                   destination, size, &received) &&
            received == size;
    }

    bool IsReadableProtection(DWORD protection)
    {
        if (protection & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        switch (protection & 0xFFu)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    bool IsRemoteImageRange(
        HANDLE process, uintptr_t address, size_t size, uintptr_t moduleBase)
    {
        if (!size ||
            size > std::numeric_limits<uintptr_t>::max() - address)
            return false;
        const uintptr_t end = address + size;
        uintptr_t cursor = address;
        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQueryEx(
                    process, reinterpret_cast<const void*>(cursor),
                    &info, sizeof(info)) != sizeof(info))
                return false;
            const uintptr_t regionBase =
                reinterpret_cast<uintptr_t>(info.BaseAddress);
            if (!info.RegionSize ||
                info.RegionSize >
                    std::numeric_limits<uintptr_t>::max() - regionBase)
                return false;
            const uintptr_t regionEnd = regionBase + info.RegionSize;
            if (cursor < regionBase || cursor >= regionEnd ||
                info.State != MEM_COMMIT || info.Type != MEM_IMAGE ||
                reinterpret_cast<uintptr_t>(info.AllocationBase) !=
                    moduleBase ||
                !IsReadableProtection(info.Protect))
                return false;
            cursor = std::min(end, regionEnd);
        }
        return true;
    }

    class Sha256
    {
    public:
        ~Sha256()
        {
            if (m_hash)
                BCryptDestroyHash(m_hash);
            if (m_algorithm)
                BCryptCloseAlgorithmProvider(m_algorithm, 0);
        }

        bool Initialize()
        {
            if (BCryptOpenAlgorithmProvider(
                    &m_algorithm, BCRYPT_SHA256_ALGORITHM,
                    nullptr, 0) < 0)
                return false;
            DWORD objectSize = 0;
            DWORD received = 0;
            if (BCryptGetProperty(
                    m_algorithm, BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&objectSize),
                    sizeof(objectSize), &received, 0) < 0 ||
                received != sizeof(objectSize) || !objectSize)
                return false;
            m_object.resize(objectSize);
            return BCryptCreateHash(
                       m_algorithm, &m_hash, m_object.data(),
                       static_cast<ULONG>(m_object.size()),
                       nullptr, 0, 0) >= 0;
        }

        bool Add(const void* bytes, size_t size)
        {
            const auto* cursor = static_cast<const uint8_t*>(bytes);
            while (size)
            {
                const ULONG part = static_cast<ULONG>(
                    std::min<size_t>(size, 1024 * 1024));
                if (BCryptHashData(
                        m_hash, const_cast<PUCHAR>(cursor), part, 0) < 0)
                    return false;
                cursor += part;
                size -= part;
            }
            return true;
        }

        bool Finish(std::array<uint8_t, 32>& output)
        {
            return BCryptFinishHash(
                       m_hash, output.data(),
                       static_cast<ULONG>(output.size()), 0) >= 0;
        }

    private:
        BCRYPT_ALG_HANDLE m_algorithm = nullptr;
        BCRYPT_HASH_HANDLE m_hash = nullptr;
        std::vector<uint8_t> m_object;
    };

    std::string Hex(const std::array<uint8_t, 32>& bytes)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string result(bytes.size() * 2, '0');
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            result[i * 2] = digits[bytes[i] >> 4];
            result[i * 2 + 1] = digits[bytes[i] & 0x0F];
        }
        return result;
    }

    bool HashBytes(const void* bytes, size_t size, std::string& output)
    {
        Sha256 hash;
        std::array<uint8_t, 32> digest{};
        if (!hash.Initialize() || !hash.Add(bytes, size) ||
            !hash.Finish(digest))
            return false;
        output = Hex(digest);
        return true;
    }

    bool HashOpenFile(HANDLE file, std::string& output)
    {
        LARGE_INTEGER beginning{};
        if (!file || file == INVALID_HANDLE_VALUE ||
            !SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN))
            return false;
        Sha256 hash;
        bool ok = hash.Initialize();
        std::vector<uint8_t> buffer(1024 * 1024);
        while (ok)
        {
            DWORD received = 0;
            if (!ReadFile(
                    file, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &received, nullptr))
            {
                ok = false;
                break;
            }
            if (!received)
                break;
            ok = hash.Add(buffer.data(), received);
        }
        std::array<uint8_t, 32> digest{};
        if (!ok || !hash.Finish(digest))
            return false;
        output = Hex(digest);
        return true;
    }

    bool HashFile(const std::wstring& path, std::string& output)
    {
        HANDLE file = CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        const bool ok = HashOpenFile(file, output);
        CloseHandle(file);
        return ok;
    }

    std::wstring BaseName(std::wstring_view path)
    {
        const size_t slash = path.find_last_of(L"/\\");
        return std::wstring(
            slash == std::wstring_view::npos
                ? path
                : path.substr(slash + 1));
    }

    struct RemoteModule
    {
        std::wstring name;
        std::wstring path;
        uintptr_t base = 0;
        size_t size = 0;
    };

    bool SnapshotModules(
        DWORD pid, std::vector<RemoteModule>& modules, DWORD& error)
    {
        modules.clear();
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            HANDLE snapshot = CreateToolhelp32Snapshot(
                TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snapshot == INVALID_HANDLE_VALUE)
            {
                error = GetLastError();
                if (error == ERROR_BAD_LENGTH)
                {
                    Sleep(5);
                    continue;
                }
                return false;
            }

            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (!Module32FirstW(snapshot, &entry))
            {
                error = GetLastError();
                CloseHandle(snapshot);
                if (error == ERROR_BAD_LENGTH)
                {
                    Sleep(5);
                    continue;
                }
                return false;
            }
            bool complete = false;
            for (;;)
            {
                modules.push_back({
                    entry.szModule,
                    entry.szExePath,
                    reinterpret_cast<uintptr_t>(entry.modBaseAddr),
                    entry.modBaseSize,
                });
                if (Module32NextW(snapshot, &entry))
                    continue;
                error = GetLastError();
                complete = error == ERROR_NO_MORE_FILES;
                break;
            }
            CloseHandle(snapshot);
            if (complete)
            {
                error = ERROR_SUCCESS;
                return true;
            }
            modules.clear();
            if (error == ERROR_BAD_LENGTH)
            {
                Sleep(5);
                continue;
            }
            return false;
        }
        return false;
    }

    const RemoteModule* FindModule(
        const std::vector<RemoteModule>& modules, const wchar_t* name)
    {
        for (const auto& module : modules)
        {
            if (_wcsicmp(module.name.c_str(), name) == 0)
                return &module;
        }
        return nullptr;
    }

    bool IsTitleModule(std::wstring_view name)
    {
        const std::wstring owned(name);
        for (const auto candidate : kTitleModules)
        {
            if (_wcsicmp(owned.c_str(), std::wstring(candidate).c_str()) == 0)
                return true;
        }
        return false;
    }

    std::wstring TitleModuleList(
        const std::vector<RemoteModule>& modules)
    {
        std::wstring result;
        for (const auto& module : modules)
        {
            if (!IsTitleModule(module.name))
                continue;
            if (!result.empty())
                result += L",";
            result += module.name;
        }
        return result.empty() ? L"none" : result;
    }

    std::vector<DWORD> FindMccProcesses()
    {
        std::vector<DWORD> result;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            HANDLE snapshot =
                CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return {};
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (!Process32FirstW(snapshot, &entry))
            {
                const DWORD error = GetLastError();
                CloseHandle(snapshot);
                if (error == ERROR_BAD_LENGTH)
                {
                    Sleep(5);
                    continue;
                }
                return {};
            }
            bool complete = false;
            for (;;)
            {
                if (_wcsicmp(entry.szExeFile, kMccExecutable) == 0)
                    result.push_back(entry.th32ProcessID);
                if (Process32NextW(snapshot, &entry))
                    continue;
                const DWORD error = GetLastError();
                complete = error == ERROR_NO_MORE_FILES;
                if (!complete && error == ERROR_BAD_LENGTH)
                    result.clear();
                break;
            }
            CloseHandle(snapshot);
            if (complete)
                return result;
            if (!result.empty())
                return {};
            Sleep(5);
        }
        return {};
    }

    struct RemoteSection
    {
        std::string name;
        uint32_t rva = 0;
        uint32_t size = 0;
        bool executable = false;
    };

    struct RemotePe
    {
        uint16_t machine = 0;
        uint32_t timestamp = 0;
        uint32_t imageSize = 0;
        std::vector<RemoteSection> sections;
    };

    bool ReadRemotePe(
        HANDLE process, const RemoteModule& module,
        RemotePe& output, std::string& error)
    {
        IMAGE_DOS_HEADER dos{};
        if (!ReadRemote(process, module.base, &dos, sizeof(dos)) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE)
        {
            error = "invalid or unreadable DOS header";
            return false;
        }
        if (dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
            static_cast<uint64_t>(dos.e_lfanew) + sizeof(DWORD) +
                    sizeof(IMAGE_FILE_HEADER) >
                module.size)
        {
            error = "out-of-range PE header offset";
            return false;
        }

        uintptr_t ntAddress = 0;
        if (!AddOffset(
                module.base, static_cast<size_t>(dos.e_lfanew),
                ntAddress))
        {
            error = "PE header address overflow";
            return false;
        }
        DWORD signature = 0;
        IMAGE_FILE_HEADER fileHeader{};
        if (!ReadRemote(
                process, ntAddress, &signature, sizeof(signature)) ||
            signature != IMAGE_NT_SIGNATURE ||
            !ReadRemote(
                process, ntAddress + sizeof(signature),
                &fileHeader, sizeof(fileHeader)))
        {
            error = "invalid or unreadable PE signature/header";
            return false;
        }
        if (fileHeader.NumberOfSections == 0 ||
            fileHeader.NumberOfSections > 96 ||
            fileHeader.SizeOfOptionalHeader <
                sizeof(IMAGE_OPTIONAL_HEADER64))
        {
            error = "invalid PE section/optional-header metadata";
            return false;
        }

        IMAGE_OPTIONAL_HEADER64 optionalHeader{};
        const uintptr_t optionalAddress =
            ntAddress + sizeof(signature) + sizeof(fileHeader);
        if (!ReadRemote(
                process, optionalAddress,
                &optionalHeader, sizeof(optionalHeader)) ||
            optionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            error = "module is not a readable PE32+ image";
            return false;
        }

        const uint64_t sectionTableOffset =
            static_cast<uint64_t>(dos.e_lfanew) + sizeof(signature) +
            sizeof(fileHeader) + fileHeader.SizeOfOptionalHeader;
        const uint64_t sectionBytes =
            static_cast<uint64_t>(fileHeader.NumberOfSections) *
            sizeof(IMAGE_SECTION_HEADER);
        if (sectionTableOffset > module.size ||
            sectionBytes > module.size - sectionTableOffset)
        {
            error = "section table exceeds the mapped image";
            return false;
        }

        std::vector<IMAGE_SECTION_HEADER> rawSections(
            fileHeader.NumberOfSections);
        uintptr_t sectionAddress = 0;
        if (!AddOffset(
                module.base, static_cast<size_t>(sectionTableOffset),
                sectionAddress) ||
            !ReadRemote(
                process, sectionAddress, rawSections.data(),
                rawSections.size() * sizeof(rawSections[0])))
        {
            error = "section table is unreadable";
            return false;
        }

        output = {};
        output.machine = fileHeader.Machine;
        output.timestamp = fileHeader.TimeDateStamp;
        output.imageSize = optionalHeader.SizeOfImage;
        for (const auto& raw : rawSections)
        {
            RemoteSection section{};
            char name[9]{};
            std::memcpy(name, raw.Name, 8);
            section.name = name;
            section.rva = raw.VirtualAddress;
            section.size = std::max(
                raw.Misc.VirtualSize, raw.SizeOfRawData);
            section.executable =
                (raw.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            if (section.rva >= output.imageSize ||
                section.size > output.imageSize - section.rva)
            {
                error = "section range exceeds SizeOfImage";
                return false;
            }
            output.sections.push_back(std::move(section));
        }
        return true;
    }

    bool IsExecutableRva(
        const RemotePe& image, uint32_t rva, size_t size)
    {
        if (!size ||
            size > std::numeric_limits<uint32_t>::max() - rva)
            return false;
        const uint32_t end = rva + static_cast<uint32_t>(size);
        for (const auto& section : image.sections)
        {
            if (!section.executable)
                continue;
            const uint64_t sectionEnd =
                static_cast<uint64_t>(section.rva) + section.size;
            if (rva >= section.rva && end <= sectionEnd)
                return true;
        }
        return false;
    }

    struct PatternScan
    {
        bool complete = false;
        size_t count = 0;
        uintptr_t first = 0;
    };

    PatternScan ScanExecutableSections(
        HANDLE process, const RemoteModule& module,
        const RemotePe& image, const uint8_t* pattern,
        size_t patternSize)
    {
        PatternScan result{};
        if (!pattern || !patternSize)
            return result;
        constexpr size_t chunkSize = 1024 * 1024;
        std::vector<uint8_t> buffer(chunkSize + patternSize - 1);

        for (const auto& section : image.sections)
        {
            if (!section.executable || section.size < patternSize)
                continue;
            uintptr_t sectionAddress = 0;
            if (!AddOffset(module.base, section.rva, sectionAddress) ||
                !IsRemoteImageRange(
                    process, sectionAddress, section.size, module.base))
                return result;

            size_t offset = 0;
            size_t carry = 0;
            while (offset < section.size)
            {
                const size_t part =
                    std::min(chunkSize, section.size - offset);
                if (!ReadRemote(
                        process, sectionAddress + offset,
                        buffer.data() + carry, part))
                    return result;
                const size_t available = carry + part;
                const size_t matches = CountReachExactPattern(
                    buffer.data(), available, pattern, patternSize);
                if (matches)
                {
                    if (!result.count)
                    {
                        for (size_t i = 0;
                             i + patternSize <= available; ++i)
                        {
                            if (std::memcmp(
                                    buffer.data() + i, pattern,
                                    patternSize) == 0)
                            {
                                result.first = sectionAddress + offset -
                                    carry + i;
                                break;
                            }
                        }
                    }
                    result.count += matches;
                }
                carry = std::min(patternSize - 1, available);
                if (carry)
                    std::memmove(
                        buffer.data(),
                        buffer.data() + available - carry, carry);
                offset += part;
            }
        }
        result.complete = true;
        return result;
    }

    bool CheckRel32Call(
        HANDLE process, const RemoteModule& module,
        const ExpectedCall& expected)
    {
        std::array<uint8_t, 5> bytes{};
        uintptr_t instruction = 0;
        if (!AddOffset(module.base, expected.siteRva, instruction) ||
            !ReadRemote(
                process, instruction, bytes.data(), bytes.size()))
            return false;
        uintptr_t target = 0;
        uintptr_t expectedTarget = 0;
        return ResolveReachRel32Call(
                   instruction, bytes.data(), bytes.size(), target) &&
            AddOffset(
                module.base, expected.targetRva, expectedTarget) &&
            target == expectedTarget;
    }

    bool SameModule(
        const RemoteModule& left, const RemoteModule& right)
    {
        return left.base == right.base && left.size == right.size &&
            _wcsicmp(left.path.c_str(), right.path.c_str()) == 0;
    }

    bool RunLoadedImagePreflight(
        HANDLE process, DWORD pid, const RemoteModule& module,
        Reporter& report)
    {
        report.Line(
            "REACH OBSERVER preflight: module=%ls base=0x%llX "
            "mappedSize=0x%zX path=%ls",
            module.name.c_str(),
            static_cast<unsigned long long>(module.base),
            module.size, module.path.c_str());

        std::string fileHash;
        if (!HashFile(module.path, fileHash) ||
            fileHash != kExpectedRetailSha256)
        {
            report.Line(
                "REACH OBSERVER preflight FAIL: backing SHA-256=%s "
                "(expected %s)",
                fileHash.empty() ? "unavailable" : fileHash.c_str(),
                kExpectedRetailSha256);
            return false;
        }

        RemotePe image{};
        std::string peError;
        if (!ReadRemotePe(process, module, image, peError))
        {
            report.Line(
                "REACH OBSERVER preflight FAIL: %s", peError.c_str());
            return false;
        }
        if (image.machine != IMAGE_FILE_MACHINE_AMD64 ||
            image.timestamp != kExpectedTimestamp ||
            image.imageSize != kExpectedImageSize ||
            module.size != kExpectedImageSize)
        {
            report.Line(
                "REACH OBSERVER preflight FAIL: machine=0x%04X "
                "timestamp=0x%08X SizeOfImage=0x%08X mapped=0x%zX",
                image.machine, image.timestamp, image.imageSize,
                module.size);
            return false;
        }
        if (!IsExecutableRva(
                image, kMainRenderViewRva, kMainRenderViewSize) ||
            !IsExecutableRva(
                image, kFrustumBoundsRva,
                kFrustumBoundsEntry.size()))
        {
            report.Line(
                "REACH OBSERVER preflight FAIL: proven function range "
                "is not executable");
            return false;
        }

        const PatternScan ownerScan = ScanExecutableSections(
            process, module, image, kMainRenderViewEntry.data(),
            kMainRenderViewEntry.size());
        const PatternScan frustumScan = ScanExecutableSections(
            process, module, image, kFrustumBoundsEntry.data(),
            kFrustumBoundsEntry.size());
        if (!ownerScan.complete || ownerScan.count != 1 ||
            ownerScan.first != module.base + kMainRenderViewRva ||
            !frustumScan.complete || frustumScan.count != 1 ||
            frustumScan.first != module.base + kFrustumBoundsRva)
        {
            report.Line(
                "REACH OBSERVER preflight FAIL: executable AOBs "
                "owner(count=%zu first=0x%llX) "
                "frustum(count=%zu first=0x%llX)",
                ownerScan.count,
                static_cast<unsigned long long>(ownerScan.first),
                frustumScan.count,
                static_cast<unsigned long long>(frustumScan.first));
            return false;
        }

        std::array<uint8_t, kMainRenderViewSize> ownerBody{};
        if (!ReadRemote(
                process, module.base + kMainRenderViewRva,
                ownerBody.data(), ownerBody.size()))
        {
            report.Line(
                "REACH OBSERVER preflight FAIL: main_render_view body "
                "read was partial");
            return false;
        }
        std::string ownerHash;
        if (!HashBytes(
                ownerBody.data(), ownerBody.size(), ownerHash) ||
            ownerHash != kMainRenderViewSha256)
        {
            report.Line(
                "REACH OBSERVER preflight FAIL: main_render_view "
                "body SHA-256=%s",
                ownerHash.empty() ? "unavailable" : ownerHash.c_str());
            return false;
        }

        for (const auto& expected : kExpectedCalls)
        {
            if (!IsExecutableRva(image, expected.siteRva, 5) ||
                !CheckRel32Call(process, module, expected))
            {
                report.Line(
                    "REACH OBSERVER preflight FAIL: rel32 edge '%s' "
                    "does not match",
                    expected.name);
                return false;
            }
        }

        uintptr_t playerViews = 0;
        uintptr_t workspace = 0;
        uintptr_t activeView = 0;
        if (!AddOffset(
                module.base, kPlayerViewArrayRva, playerViews) ||
            !AddOffset(
                module.base, kRasterizerWorkspaceRva, workspace) ||
            !AddOffset(
                module.base, kActiveViewGlobalRva, activeView) ||
            !IsRemoteImageRange(
                process, playerViews,
                kPlayerViewCount * kPlayerViewStride, module.base) ||
            !IsRemoteImageRange(
                process, workspace,
                kRasterizerWorkspaceSize, module.base) ||
            !IsRemoteImageRange(
                process, activeView, sizeof(uintptr_t), module.base))
        {
            report.Line(
                "REACH OBSERVER preflight FAIL: fixed evidence range "
                "is not a committed MEM_IMAGE mapping");
            return false;
        }

        std::vector<RemoteModule> finalModules;
        DWORD snapshotError = ERROR_SUCCESS;
        if (!SnapshotModules(pid, finalModules, snapshotError))
        {
            report.Line(
                "REACH OBSERVER preflight FAIL: final module snapshot "
                "failed (%lu)", snapshotError);
            return false;
        }
        const RemoteModule* finalReach =
            FindModule(finalModules, kReachModule);
        size_t finalTitleCount = 0;
        for (const auto& finalModule : finalModules)
        {
            if (IsTitleModule(finalModule.name))
                ++finalTitleCount;
        }
        if (!finalReach || !SameModule(module, *finalReach) ||
            finalTitleCount != 1 ||
            FindModule(finalModules, kInjectedModModule) ||
            FindModule(finalModules, kLegacyInjectedModModule))
        {
            report.Line(
                "REACH OBSERVER preflight FAIL: mapping changed or "
                "sole-title/mod-DLL state changed during validation");
            return false;
        }

        report.Line(
            "REACH OBSERVER preflight PASS: backing SHA-256=%s; "
            "AMD64 timestamp=0x%08X image=0x%08X; "
            "ownerAOB=1@0x%08X bodySHA=%s; "
            "frustumAOB=1@0x%08X; rel32Edges=%zu; "
            "hooks=0 processWrites=0",
            fileHash.c_str(), image.timestamp, image.imageSize,
            kMainRenderViewRva, ownerHash.c_str(),
            kFrustumBoundsRva, std::size(kExpectedCalls));
        return true;
    }

    enum class ReachPresence : uint8_t
    {
        Absent = 0,
        Unambiguous,
        Ambiguous,
    };

    const char* PresenceName(ReachPresence presence)
    {
        switch (presence)
        {
        case ReachPresence::Absent: return "reach-absent";
        case ReachPresence::Unambiguous: return "unambiguous-reach";
        case ReachPresence::Ambiguous: return "ambiguous-resident";
        default: return "unknown";
        }
    }

    ReachPresence DeterminePresence(
        const std::vector<RemoteModule>& modules)
    {
        size_t titleCount = 0;
        bool reach = false;
        for (const auto& module : modules)
        {
            if (!IsTitleModule(module.name))
                continue;
            ++titleCount;
            if (_wcsicmp(module.name.c_str(), kReachModule) == 0)
                reach = true;
        }
        if (!reach)
            return ReachPresence::Absent;
        return titleCount == 1
            ? ReachPresence::Unambiguous
            : ReachPresence::Ambiguous;
    }

    uint64_t Hash64(const uint8_t* bytes, size_t size)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    struct SessionStats
    {
        uint64_t startedMs = 0;
        uint64_t pointerReads = 0;
        uint64_t readFailures = 0;
        uint64_t activeSamples = 0;
        uint64_t transactions = 0;
        uint64_t normalTransactions = 0;
        uint64_t outsideArray = 0;
        uint64_t validCameras = 0;
        uint64_t invalidCameras = 0;
        uint64_t tornSnapshots = 0;
        uint64_t cameraChanges = 0;
        uint64_t primarySecondaryEqual = 0;
        uint64_t primarySecondaryDifferent = 0;
        uint64_t multipleFreshOwners = 0;
        uint64_t stableWindows = 0;
        uint64_t longestFreshSpanMs = 0;
        std::array<uint64_t, kPlayerViewCount> slotTransactions{};
        uint32_t slotMask = 0;
    };

    struct ObserverSession
    {
        RemoteModule module;
        uint64_t generation = 0;
        bool preflightPassed = false;
        ReachObserverTransactionGate transactionGate;
        bool loggedReadFailure = false;
        bool loggedOutsideArray = false;
        bool loggedTornSnapshot = false;
        bool loggedInvalidCamera = false;
        bool loggedFirstCamera = false;
        int stableOwnerSlot = kReachNoFreshOwner;
        bool hasCameraHash = false;
        uint64_t cameraHash = 0;
        std::array<ReachObserverFreshnessWindow, kPlayerViewCount> freshness{};
        SessionStats stats;
    };

    struct RunStats
    {
        uint64_t preflightPasses = 0;
        uint64_t moduleUnloads = 0;
        uint64_t ambiguityEntries = 0;
        uint64_t snapshotFailures = 0;
        uint64_t transactions = 0;
        uint64_t validCameras = 0;
        uint64_t multipleFreshOwners = 0;
        uint64_t stableWindows = 0;
    };

    uint64_t FreshTransactionCount(const ObserverSession& session)
    {
        uint64_t count = 0;
        for (const auto& freshness : session.freshness)
            count += freshness.TransactionCount();
        return count;
    }

    void ResetFreshness(ObserverSession& session)
    {
        for (auto& freshness : session.freshness)
            freshness.Reset();
        session.stableOwnerSlot = kReachNoFreshOwner;
    }

    void ResetObservationContinuity(
        ObserverSession& session, uint64_t now, Reporter& report,
        const char* reason)
    {
        const bool hadEvidence =
            FreshTransactionCount(session) != 0 ||
            session.transactionGate.HasLatchedValue();
        if (hadEvidence && reason)
        {
            report.Line(
                "REACH OBSERVER continuity reset: generation=%llu "
                "reason=%s at=%llums",
                static_cast<unsigned long long>(session.generation),
                reason, static_cast<unsigned long long>(now));
        }
        ResetFreshness(session);
        session.transactionGate.RequireClear();
    }

    void LogSessionSummary(
        const ObserverSession& session, uint64_t now, Reporter& report,
        const char* reason)
    {
        const uint64_t elapsed =
            now >= session.stats.startedMs
                ? now - session.stats.startedMs
                : 0;
        report.Line(
            "REACH OBSERVER summary: generation=%llu reason=%s "
            "elapsed=%llums reads=%llu readFailures=%llu "
            "activeSamples=%llu transactions=%llu normal=%llu "
            "outsideArray=%llu validCamera=%llu invalidCamera=%llu "
            "torn=%llu cameraChanges=%llu primarySecondary=%llu/%llu "
            "multiOwner=%llu stableWindows=%llu longestFresh=%llums slotMask=0x%X "
            "slots=[%llu,%llu,%llu,%llu]",
            static_cast<unsigned long long>(session.generation),
            reason ? reason : "periodic",
            static_cast<unsigned long long>(elapsed),
            static_cast<unsigned long long>(session.stats.pointerReads),
            static_cast<unsigned long long>(session.stats.readFailures),
            static_cast<unsigned long long>(session.stats.activeSamples),
            static_cast<unsigned long long>(session.stats.transactions),
            static_cast<unsigned long long>(
                session.stats.normalTransactions),
            static_cast<unsigned long long>(session.stats.outsideArray),
            static_cast<unsigned long long>(session.stats.validCameras),
            static_cast<unsigned long long>(session.stats.invalidCameras),
            static_cast<unsigned long long>(session.stats.tornSnapshots),
            static_cast<unsigned long long>(session.stats.cameraChanges),
            static_cast<unsigned long long>(
                session.stats.primarySecondaryEqual),
            static_cast<unsigned long long>(
                session.stats.primarySecondaryDifferent),
            static_cast<unsigned long long>(
                session.stats.multipleFreshOwners),
            static_cast<unsigned long long>(session.stats.stableWindows),
            static_cast<unsigned long long>(
                session.stats.longestFreshSpanMs),
            session.stats.slotMask,
            static_cast<unsigned long long>(
                session.stats.slotTransactions[0]),
            static_cast<unsigned long long>(
                session.stats.slotTransactions[1]),
            static_cast<unsigned long long>(
                session.stats.slotTransactions[2]),
            static_cast<unsigned long long>(
                session.stats.slotTransactions[3]));
    }

    void TickFreshness(
        ObserverSession& session, uint64_t now, Reporter& report)
    {
        const bool hadWindow =
            FreshTransactionCount(session) != 0;
        for (auto& freshness : session.freshness)
            freshness.Tick(now);
        if (hadWindow && FreshTransactionCount(session) == 0)
        {
            report.Line(
                "REACH OBSERVER freshness LOST: generation=%llu "
                "no newly observed set/use/clear pulse within %llums; "
                "missed pulses remain possible",
                static_cast<unsigned long long>(session.generation),
                static_cast<unsigned long long>(kReachObserverFreshMs));
            session.stableOwnerSlot = kReachNoFreshOwner;
        }
    }

    void SampleActiveView(
        HANDLE process, ObserverSession& session, uint64_t now,
        Reporter& report, RunStats& totals)
    {
        uintptr_t activeGlobal = 0;
        uintptr_t playerViewArray = 0;
        uintptr_t workspaceAddress = 0;
        if (!AddOffset(
                session.module.base, kActiveViewGlobalRva,
                activeGlobal) ||
            !AddOffset(
                session.module.base, kPlayerViewArrayRva,
                playerViewArray) ||
            !AddOffset(
                session.module.base, kRasterizerWorkspaceRva,
                workspaceAddress))
        {
            ++session.stats.readFailures;
            ResetObservationContinuity(
                session, now, report, "fixed-address overflow");
            return;
        }

        uintptr_t pointerBefore = 0;
        ++session.stats.pointerReads;
        if (!ReadRemote(
                process, activeGlobal, &pointerBefore,
                sizeof(pointerBefore)))
        {
            ++session.stats.readFailures;
            if (!session.loggedReadFailure)
            {
                session.loggedReadFailure = true;
                report.Line(
                    "REACH OBSERVER sample INCONCLUSIVE: active-view "
                    "read was partial");
            }
            ResetObservationContinuity(
                session, now, report, "active-view read failure");
            return;
        }

        if (!pointerBefore)
        {
            session.transactionGate.Observe(0);
            TickFreshness(session, now, report);
            return;
        }

        ++session.stats.activeSamples;
        if (!session.transactionGate.Observe(pointerBefore))
        {
            TickFreshness(session, now, report);
            return;
        }
        ++session.stats.transactions;
        ++totals.transactions;

        const ReachObservedView observed =
            ClassifyReachObservedView(
                pointerBefore, playerViewArray, kPlayerViewStride,
                kPlayerViewCount);
        if (observed.kind !=
            ReachObservedViewKind::NormalPlayerSlot)
        {
            ++session.stats.outsideArray;
            if (!session.loggedOutsideArray)
            {
                session.loggedOutsideArray = true;
                report.Line(
                    "REACH OBSERVER transaction OUTSIDE normal array: "
                    "pointer=0x%llX expectedBase=0x%llX stride=0x%zX "
                    "count=%zu; not treated as player-view evidence",
                    static_cast<unsigned long long>(pointerBefore),
                    static_cast<unsigned long long>(playerViewArray),
                    kPlayerViewStride, kPlayerViewCount);
            }
            ResetFreshness(session);
            return;
        }

        ++session.stats.normalTransactions;
        ++session.stats.slotTransactions[observed.slot];
        session.stats.slotMask |= 1u << observed.slot;
        if (!IsRemoteImageRange(
                process, pointerBefore, kPlayerViewStride,
                session.module.base) ||
            !IsRemoteImageRange(
                process, workspaceAddress,
                kRasterizerWorkspaceSize, session.module.base))
        {
            ++session.stats.readFailures;
            ResetObservationContinuity(
                session, now, report,
                "transaction range left Reach MEM_IMAGE");
            return;
        }

        std::array<uint8_t, kRasterizerWorkspaceSize> workspace{};
        uintptr_t pointerAfter = 0;
        if (!ReadRemote(
                process, workspaceAddress, workspace.data(),
                workspace.size()) ||
            !ReadRemote(
                process, activeGlobal, &pointerAfter,
                sizeof(pointerAfter)))
        {
            ++session.stats.readFailures;
            if (!session.loggedReadFailure)
            {
                session.loggedReadFailure = true;
                report.Line(
                    "REACH OBSERVER snapshot INCONCLUSIVE: workspace "
                    "or closing active-view read was partial");
            }
            ResetObservationContinuity(
                session, now, report, "snapshot read failure");
            return;
        }
        if (pointerAfter != pointerBefore)
        {
            ++session.stats.tornSnapshots;
            if (!session.loggedTornSnapshot)
            {
                session.loggedTornSnapshot = true;
                report.Line(
                    "REACH OBSERVER snapshot discarded: active-view "
                    "changed during pointer/workspace/pointer read "
                    "(0x%llX -> 0x%llX)",
                    static_cast<unsigned long long>(pointerBefore),
                    static_cast<unsigned long long>(pointerAfter));
            }
            ResetObservationContinuity(
                session, now, report, "torn active-view bracket");
            return;
        }

        ReachCompactCameraObservation primary{};
        ReachCompactCameraObservation secondary{};
        const bool primaryValid = ValidateReachCompactCamera(
            workspace.data(), kCompactCameraSize, primary);
        const bool secondaryValid = ValidateReachCompactCamera(
            workspace.data() + kSecondaryCompactOffset,
            kCompactCameraSize, secondary);
        if (!primaryValid || !secondaryValid)
        {
            ++session.stats.invalidCameras;
            if (!session.loggedInvalidCamera)
            {
                session.loggedInvalidCamera = true;
                report.Line(
                    "REACH OBSERVER camera INVALID: generation=%llu "
                    "slot=%u primary=%s secondary=%s; finite/axis/FOV/"
                    "bounds guards rejected the sample",
                    static_cast<unsigned long long>(session.generation),
                    observed.slot, primaryValid ? "valid" : "invalid",
                    secondaryValid ? "valid" : "invalid");
            }
            ResetFreshness(session);
            return;
        }

        ++session.stats.validCameras;
        ++totals.validCameras;
        const bool compactEqual = std::memcmp(
            workspace.data(),
            workspace.data() + kSecondaryCompactOffset,
            kCompactCameraSize) == 0;
        if (compactEqual)
            ++session.stats.primarySecondaryEqual;
        else
            ++session.stats.primarySecondaryDifferent;

        const uint64_t cameraHash =
            Hash64(workspace.data(), kCompactCameraSize);
        if (session.hasCameraHash &&
            session.cameraHash != cameraHash)
            ++session.stats.cameraChanges;
        session.hasCameraHash = true;
        session.cameraHash = cameraHash;

        if (!session.loggedFirstCamera)
        {
            session.loggedFirstCamera = true;
            report.Line(
                "REACH OBSERVER first valid transaction: "
                "generation=%llu slot=%u pointer=0x%llX "
                "position=(%.6f,%.6f,%.6f) "
                "forward=(%.6f,%.6f,%.6f) "
                "up=(%.6f,%.6f,%.6f) vfov=%.6f "
                "client=[%d,%d,%d,%d] secondaryCopy=%s",
                static_cast<unsigned long long>(session.generation),
                observed.slot,
                static_cast<unsigned long long>(pointerBefore),
                primary.position[0], primary.position[1],
                primary.position[2], primary.forward[0],
                primary.forward[1], primary.forward[2],
                primary.up[0], primary.up[1], primary.up[2],
                primary.verticalFov, primary.clientBounds.y0,
                primary.clientBounds.x0, primary.clientBounds.y1,
                primary.clientBounds.x1,
                compactEqual ? "equal" : "different");
        }

        TickFreshness(session, now, report);
        ReachObserverFreshnessWindow& slotFreshness =
            session.freshness[observed.slot];
        slotFreshness.ObserveTransaction(now);
        const int freshOwner = ReachObserverUniqueFreshOwner(
            session.freshness.data(), session.freshness.size(), now);
        if (freshOwner == kReachMultipleFreshOwners)
        {
            ++session.stats.multipleFreshOwners;
            ++totals.multipleFreshOwners;
            report.Line(
                "REACH OBSERVER freshness REFUSED: generation=%llu "
                "multiple player slots were fresh; all ownership windows reset",
                static_cast<unsigned long long>(session.generation));
            ResetFreshness(session);
            return;
        }
        const bool stable = freshOwner == static_cast<int>(observed.slot) &&
            slotFreshness.IsStable(now);
        if (!stable && session.stableOwnerSlot != freshOwner)
            session.stableOwnerSlot = kReachNoFreshOwner;
        session.stats.longestFreshSpanMs = std::max(
            session.stats.longestFreshSpanMs,
            slotFreshness.CurrentSpanMs());
        if (stable && session.stableOwnerSlot != freshOwner)
        {
            session.stableOwnerSlot = freshOwner;
            ++session.stats.stableWindows;
            ++totals.stableWindows;
            report.Line(
                "REACH OBSERVER one-second gate OBSERVED: "
                "generation=%llu slot=%u transactions=%llu span=%llums; "
                "observer only, no VR armed",
                static_cast<unsigned long long>(session.generation),
                observed.slot,
                static_cast<unsigned long long>(
                    slotFreshness.TransactionCount()),
                static_cast<unsigned long long>(
                    slotFreshness.CurrentSpanMs()));
        }
    }

    struct Options
    {
        DWORD pid = 0;
        uint64_t durationMs = 0;
        std::wstring outputPath;
        bool help = false;
    };

    void PrintUsage()
    {
        std::fputs(
            "Usage: reach-runtime-observer.exe "
            "[--pid <decimal>] [--duration <seconds>] "
            "[--output <log-path>]\n"
            "\n"
            "Run only with stock, anti-cheat-disabled MCC. The observer "
            "requests PROCESS_QUERY_INFORMATION | PROCESS_VM_READ only.\n",
            stdout);
    }

    bool ParseUnsigned(
        const wchar_t* text, uint64_t maximum, uint64_t& value)
    {
        if (!text || !*text || *text == L'-')
            return false;
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long parsed =
            std::wcstoull(text, &end, 10);
        if (errno == ERANGE || !end || *end != L'\0' ||
            parsed > maximum)
            return false;
        value = static_cast<uint64_t>(parsed);
        return true;
    }

    bool ParseOptions(
        int argc, wchar_t** argv, Options& options,
        std::wstring& error)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view argument(argv[i]);
            if (argument == L"--help" || argument == L"-h")
            {
                options.help = true;
                continue;
            }
            if (argument == L"--pid")
            {
                uint64_t parsed = 0;
                if (++i >= argc ||
                    !ParseUnsigned(
                        argv[i],
                        std::numeric_limits<DWORD>::max(), parsed) ||
                    parsed == 0)
                {
                    error = L"--pid requires a nonzero decimal PID";
                    return false;
                }
                options.pid = static_cast<DWORD>(parsed);
                continue;
            }
            if (argument == L"--duration")
            {
                uint64_t seconds = 0;
                constexpr uint64_t maxSeconds =
                    std::numeric_limits<uint64_t>::max() / 1000;
                if (++i >= argc ||
                    !ParseUnsigned(argv[i], maxSeconds, seconds) ||
                    seconds == 0)
                {
                    error =
                        L"--duration requires positive integer seconds";
                    return false;
                }
                options.durationMs = seconds * 1000;
                continue;
            }
            if (argument == L"--output")
            {
                if (++i >= argc || !argv[i][0])
                {
                    error = L"--output requires a path";
                    return false;
                }
                options.outputPath = argv[i];
                continue;
            }
            error = L"unknown argument: ";
            error += argument;
            return false;
        }
        return true;
    }

    bool QueryProcessPath(
        HANDLE process, std::wstring& output)
    {
        std::vector<wchar_t> buffer(32768);
        DWORD length = static_cast<DWORD>(buffer.size());
        if (!QueryFullProcessImageNameW(
                process, 0, buffer.data(), &length) ||
            !length)
            return false;
        output.assign(buffer.data(), length);
        return true;
    }

    size_t LastPathSeparator(std::wstring_view path)
    {
        const size_t forward = path.find_last_of(L'/');
        const size_t backward =
            path.find_last_of(static_cast<wchar_t>(92));
        if (forward == std::wstring_view::npos)
            return backward;
        if (backward == std::wstring_view::npos)
            return forward;
        return std::max(forward, backward);
    }

    std::wstring DirectoryName(std::wstring_view path)
    {
        const size_t separator = LastPathSeparator(path);
        if (separator == std::wstring_view::npos)
            return {};
        if (separator == 2 && path.size() >= 3 && path[1] == L':')
            return std::wstring(path.substr(0, 3));
        return std::wstring(path.substr(0, separator));
    }

    std::wstring ExecutablePath()
    {
        std::vector<wchar_t> buffer(32768);
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (!length || length >= buffer.size())
            return {};
        return std::wstring(buffer.data(), length);
    }

    bool NormalizeFullPath(
        const std::wstring& input, std::wstring& output)
    {
        std::vector<wchar_t> buffer(32768);
        const DWORD length = GetFullPathNameW(
            input.c_str(), static_cast<DWORD>(buffer.size()),
            buffer.data(), nullptr);
        if (!length || length >= buffer.size())
            return false;
        output.assign(buffer.data(), length);
        return true;
    }

    bool CanonicalPathFromHandle(
        HANDLE handle, std::wstring& output)
    {
        std::vector<wchar_t> buffer(32768);
        const DWORD flags =
            FILE_NAME_NORMALIZED | VOLUME_NAME_GUID;
        const DWORD length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            flags);
        if (!length || length >= buffer.size())
            return false;
        output.assign(buffer.data(), length);
        return true;
    }

    bool CanonicalPathForExisting(
        const std::wstring& input, bool directory,
        std::wstring& output)
    {
        HANDLE handle = CreateFileW(
            input.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            directory ? FILE_FLAG_BACKUP_SEMANTICS
                      : FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        const bool ok = CanonicalPathFromHandle(handle, output);
        CloseHandle(handle);
        return ok;
    }

    bool DeriveMccInstallRoot(
        const std::wstring& processPath, std::wstring& output)
    {
        const std::wstring win64 = DirectoryName(processPath);
        const std::wstring binaries = DirectoryName(win64);
        const std::wstring mcc = DirectoryName(binaries);
        const std::wstring root = DirectoryName(mcc);
        if (win64.empty() || binaries.empty() || mcc.empty() ||
            root.empty() ||
            _wcsicmp(BaseName(win64).c_str(), L"Win64") != 0 ||
            _wcsicmp(BaseName(binaries).c_str(), L"Binaries") != 0 ||
            _wcsicmp(BaseName(mcc).c_str(), L"MCC") != 0)
            return false;
        output = root;
        return true;
    }

    bool IsPathWithin(
        std::wstring_view path, std::wstring_view directory)
    {
        while (directory.size() > 3 &&
               (directory.back() == L'/' ||
                directory.back() == static_cast<wchar_t>(92)))
            directory.remove_suffix(1);
        if (path.size() < directory.size() ||
            _wcsnicmp(
                path.data(), directory.data(), directory.size()) != 0)
            return false;
        if (path.size() == directory.size())
            return true;
        return path[directory.size()] == L'/' ||
            path[directory.size()] == static_cast<wchar_t>(92);
    }

    bool ResolveCanonicalNewFilePath(
        const std::wstring& requested, std::wstring& output,
        std::wstring& outputLeaf, HANDLE& parentGuard)
    {
        parentGuard = INVALID_HANDLE_VALUE;
        std::wstring normalized;
        if (!NormalizeFullPath(requested, normalized))
            return false;
        const std::wstring leaf = BaseName(normalized);
        if (leaf.empty() || leaf == L"." || leaf == L".." ||
            leaf.find(L':') != std::wstring::npos)
            return false;

        const std::wstring parent = DirectoryName(normalized);
        std::wstring canonicalParent;
        if (parent.empty())
            return false;
        HANDLE parentHandle = CreateFileW(
            parent.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (parentHandle == INVALID_HANDLE_VALUE)
            return false;
        if (!CanonicalPathFromHandle(
                parentHandle, canonicalParent))
        {
            CloseHandle(parentHandle);
            return false;
        }
        output = std::move(canonicalParent);
        if (!output.empty() && output.back() != L'/' &&
            output.back() != static_cast<wchar_t>(92))
            output += static_cast<wchar_t>(92);
        output += leaf;
        outputLeaf = leaf;
        parentGuard = parentHandle;
        return true;
    }

    std::wstring DefaultLogPath(
        DWORD pid, const std::wstring& executablePath)
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t name[160]{};
        swprintf_s(
            name,
            L"reach-runtime-observer-%04u%02u%02u-%02u%02u%02u-"
            L"pid%lu.log",
            time.wYear, time.wMonth, time.wDay, time.wHour,
            time.wMinute, time.wSecond, pid);
        std::wstring path = DirectoryName(executablePath);
        if (path.empty())
            return {};
        if (!path.empty() && path.back() != L'\\' &&
            path.back() != L'/')
            path += L'\\';
        path += name;
        return path;
    }
}

int wmain(int argc, wchar_t** argv)
{
    Options options{};
    std::wstring optionError;
    if (!ParseOptions(argc, argv, options, optionError))
    {
        std::fwprintf(stderr, L"Error: %ls\n", optionError.c_str());
        PrintUsage();
        return 2;
    }
    if (options.help)
    {
        PrintUsage();
        return 0;
    }

    if (!options.pid)
    {
        const std::vector<DWORD> processes = FindMccProcesses();
        if (processes.size() != 1)
        {
            std::fprintf(
                stderr,
                "Expected exactly one %ls process; found %zu. "
                "Open stock anti-cheat-disabled MCC first, or use --pid "
                "when exactly identifying one process.\n",
                kMccExecutable, processes.size());
            return 3;
        }
        options.pid = processes[0];
    }

    HANDLE process = OpenProcess(
        kProcessAccess, FALSE, options.pid);
    if (!process)
    {
        std::fprintf(
            stderr,
            "OpenProcess(query/read only) failed for PID %lu "
            "(error %lu).\n",
            options.pid, GetLastError());
        return 4;
    }

    std::wstring processPath;
    if (!QueryProcessPath(process, processPath) ||
        _wcsicmp(
            BaseName(processPath).c_str(),
            kMccExecutable) != 0)
    {
        std::fprintf(
            stderr,
            "PID %lu is not a verifiable %ls process.\n",
            options.pid, kMccExecutable);
        CloseHandle(process);
        return 4;
    }

    const std::wstring reportedObserverPath = ExecutablePath();
    HANDLE observerFile = INVALID_HANDLE_VALUE;
    if (!reportedObserverPath.empty())
    {
        observerFile = CreateFileW(
            reportedObserverPath.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    std::wstring observerPath;
    std::wstring canonicalProcessPath;
    std::wstring derivedMccInstallRoot;
    std::wstring mccInstallRoot;
    if (observerFile == INVALID_HANDLE_VALUE ||
        !CanonicalPathFromHandle(
            observerFile, observerPath) ||
        !CanonicalPathForExisting(
            processPath, false, canonicalProcessPath) ||
        !DeriveMccInstallRoot(
            canonicalProcessPath, derivedMccInstallRoot) ||
        !CanonicalPathForExisting(
            derivedMccInstallRoot, true, mccInstallRoot))
    {
        std::fprintf(
            stderr,
            "Cannot prove the pinned Steam MCC installation boundary; "
            "refusing to create a log.\n");
        if (observerFile != INVALID_HANDLE_VALUE)
            CloseHandle(observerFile);
        CloseHandle(process);
        return 4;
    }
    if (IsPathWithin(observerPath, mccInstallRoot))
    {
        std::fwprintf(
            stderr,
            L"Refusing to run an observer stored inside the MCC "
            L"installation: %ls\n",
            observerPath.c_str());
        CloseHandle(observerFile);
        CloseHandle(process);
        return 2;
    }

    if (options.outputPath.empty())
        options.outputPath = DefaultLogPath(
            options.pid, observerPath);
    std::wstring canonicalOutput;
    std::wstring outputLeaf;
    HANDLE outputParentGuard = INVALID_HANDLE_VALUE;
    if (options.outputPath.empty() ||
        !ResolveCanonicalNewFilePath(
            options.outputPath, canonicalOutput,
            outputLeaf, outputParentGuard))
    {
        std::fwprintf(
            stderr, L"Cannot resolve output path: %ls\n",
            options.outputPath.c_str());
        CloseHandle(observerFile);
        CloseHandle(process);
        return 2;
    }
    options.outputPath = std::move(canonicalOutput);
    if (IsPathWithin(options.outputPath, mccInstallRoot))
    {
        std::fwprintf(
            stderr,
            L"Refusing an output path inside the MCC installation: "
            L"%ls\n",
            options.outputPath.c_str());
        CloseHandle(outputParentGuard);
        CloseHandle(observerFile);
        CloseHandle(process);
        return 2;
    }
    if (GetFileAttributesW(options.outputPath.c_str()) !=
        INVALID_FILE_ATTRIBUTES)
    {
        std::fwprintf(
            stderr,
            L"Refusing to overwrite existing output: %ls\n",
            options.outputPath.c_str());
        CloseHandle(outputParentGuard);
        CloseHandle(observerFile);
        CloseHandle(process);
        return 2;
    }

    std::string observerHash;
    if (!HashOpenFile(observerFile, observerHash))
    {
        std::fprintf(
            stderr, "Cannot hash the running observer executable.\n");
        CloseHandle(outputParentGuard);
        CloseHandle(observerFile);
        CloseHandle(process);
        return 2;
    }

    Reporter report;
    if (!report.OpenRelative(
            outputParentGuard, outputLeaf))
    {
        std::fwprintf(
            stderr, L"Cannot create output log: %ls\n",
            options.outputPath.c_str());
        CloseHandle(outputParentGuard);
        CloseHandle(observerFile);
        CloseHandle(process);
        return 2;
    }
    CloseHandle(outputParentGuard);

    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    report.Line(
        "REACH OBSERVER start: source=%s observerSHA256=%s "
        "pid=%lu process=%ls "
        "access=PROCESS_QUERY_INFORMATION|PROCESS_VM_READ "
        "hooks=0 processWrites=0 injection=0 debugAttach=0 "
        "output=%ls",
        HALOMCCVR_BUILD_COMMIT, observerHash.c_str(), options.pid,
        processPath.c_str(), options.outputPath.c_str());
    CloseHandle(observerFile);
    report.Line(
        "REACH OBSERVER contract: stock anti-cheat-disabled MCC only; "
        "a loaded mod DLL or ambiguous title residency causes refusal; "
        "missed short pulses are inconclusive, never negative "
        "proof");

    const uint64_t runStarted = GetTickCount64();
    uint64_t nextModulePoll = 0;
    uint64_t nextSummary = runStarted + kSummaryMs;
    uint64_t generation = 0;
    ReachPresence lastPresence = ReachPresence::Absent;
    bool havePresence = false;
    bool snapshotFailureLatched = false;
    bool fatalValidation = false;
    bool contaminated = false;
    bool sawReach = false;
    bool haveSession = false;
    ObserverSession session{};
    RunStats totals{};
    MillisecondWaiter waiter;

    while (!g_stopRequested.load(std::memory_order_acquire))
    {
        const uint64_t now = GetTickCount64();
        if (options.durationMs &&
            now - runStarted >= options.durationMs)
        {
            report.Line("REACH OBSERVER duration reached");
            break;
        }

        if (now >= nextModulePoll)
        {
            nextModulePoll = now + kModulePollMs;
            std::vector<RemoteModule> modules;
            DWORD snapshotError = ERROR_SUCCESS;
            if (!SnapshotModules(
                    options.pid, modules, snapshotError))
            {
                ++totals.snapshotFailures;
                if (haveSession)
                    ResetObservationContinuity(
                        session, now, report,
                        "module snapshot failure");
                if (!snapshotFailureLatched)
                {
                    snapshotFailureLatched = true;
                    report.Line(
                        "REACH OBSERVER module snapshot unavailable "
                        "(error %lu); sampling paused",
                        snapshotError);
                }
                DWORD exitCode = STILL_ACTIVE;
                if (!GetExitCodeProcess(process, &exitCode) ||
                    exitCode != STILL_ACTIVE)
                {
                    report.Line(
                        "REACH OBSERVER target process exited "
                        "(code=%lu)", exitCode);
                    break;
                }
                waiter.Wait(1);
                continue;
            }
            if (snapshotFailureLatched)
            {
                snapshotFailureLatched = false;
                report.Line(
                    "REACH OBSERVER module snapshots resumed; "
                    "loaded-image state will be revalidated");
                if (haveSession)
                {
                    LogSessionSummary(
                        session, now, report,
                        "snapshot discontinuity");
                    haveSession = false;
                }
            }

            if (FindModule(modules, kInjectedModModule) ||
                FindModule(modules, kLegacyInjectedModModule))
            {
                report.Line(
                    "REACH OBSERVER REFUSED: the mod DLL is loaded; "
                    "stock-process evidence is contaminated");
                contaminated = true;
                break;
            }

            const ReachPresence presence =
                DeterminePresence(modules);
            if (!havePresence || presence != lastPresence)
            {
                const std::wstring titles =
                    TitleModuleList(modules);
                report.Line(
                    "REACH OBSERVER title state: %s modules=%ls",
                    PresenceName(presence), titles.c_str());
                if (presence == ReachPresence::Ambiguous)
                    ++totals.ambiguityEntries;
                havePresence = true;
                lastPresence = presence;
            }

            const RemoteModule* reach =
                FindModule(modules, kReachModule);
            if (presence != ReachPresence::Unambiguous ||
                !reach)
            {
                if (haveSession)
                {
                    LogSessionSummary(
                        session, now, report,
                        presence == ReachPresence::Absent
                            ? "Reach unload/title exit"
                            : "ambiguous title residency");
                    if (presence == ReachPresence::Absent)
                        ++totals.moduleUnloads;
                    haveSession = false;
                }
            }
            else
            {
                sawReach = true;
                if (!haveSession ||
                    !SameModule(session.module, *reach))
                {
                    if (haveSession)
                    {
                        LogSessionSummary(
                            session, now, report,
                            "Reach mapping changed");
                    }
                    session = {};
                    session.module = *reach;
                    session.generation = ++generation;
                    session.stats.startedMs = now;
                    haveSession = true;
                    if (!RunLoadedImagePreflight(
                            process, options.pid,
                            session.module, report))
                    {
                        fatalValidation = true;
                        break;
                    }
                    session.preflightPassed = true;
                    ++totals.preflightPasses;
                    report.Line(
                        "REACH OBSERVER sampling armed: "
                        "generation=%llu polling=1ms; observer-only",
                        static_cast<unsigned long long>(
                            session.generation));
                }
            }
        }

        if (haveSession && session.preflightPassed &&
            lastPresence == ReachPresence::Unambiguous)
        {
            SampleActiveView(
                process, session, now, report, totals);
            if (now >= nextSummary)
            {
                LogSessionSummary(
                    session, now, report, "periodic");
                nextSummary = now + kSummaryMs;
            }
        }
        waiter.Wait(1);
    }

    const uint64_t finished = GetTickCount64();
    if (haveSession)
        LogSessionSummary(
            session, finished, report, "observer stop");
    report.Line(
        "REACH OBSERVER final: elapsed=%llums sawReach=%s "
        "preflightPasses=%llu unloads=%llu ambiguityEntries=%llu "
        "snapshotFailures=%llu observedTransactions=%llu "
        "validCameras=%llu multiOwner=%llu stableWindows=%llu result=%s",
        static_cast<unsigned long long>(finished - runStarted),
        sawReach ? "yes" : "no",
        static_cast<unsigned long long>(totals.preflightPasses),
        static_cast<unsigned long long>(totals.moduleUnloads),
        static_cast<unsigned long long>(totals.ambiguityEntries),
        static_cast<unsigned long long>(totals.snapshotFailures),
        static_cast<unsigned long long>(totals.transactions),
        static_cast<unsigned long long>(totals.validCameras),
        static_cast<unsigned long long>(totals.multipleFreshOwners),
        static_cast<unsigned long long>(totals.stableWindows),
        contaminated ? "REFUSED_CONTAMINATED" :
        fatalValidation ? "FAILED_PREFLIGHT" :
        totals.validCameras ? "OBSERVATIONS_RECORDED_UNASSESSED" :
        totals.preflightPasses ? "PREFLIGHT_ONLY_INCONCLUSIVE" :
        "NO_REACH_PREFLIGHT");

    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    CloseHandle(process);
    if (contaminated)
        return 5;
    if (fatalValidation)
        return 6;
    if (!totals.preflightPasses)
        return 7;
    return totals.validCameras ? 0 : 8;
}
