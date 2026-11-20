#include "sysinfo.h"

#include <windows.h>

#include <cpuid.h>

#include <cstring>

#include "log.h"

namespace cbf {
namespace {

template <class T>
T Resolve(HMODULE m, const char* name) {
    return m ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

using RegGetValueWFn = LONG(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

// -1 = unknown / not present, otherwise the DWORD value
long ReadDword(RegGetValueWFn fn, HKEY root, const wchar_t* key, const wchar_t* value) {
    if (!fn) return -1;
    DWORD data = 0, size = sizeof(data);
    if (fn(root, key, value, 0x10 /* RRF_RT_REG_DWORD */, nullptr, &data, &size) != ERROR_SUCCESS) return -1;
    return static_cast<long>(data);
}

const char* OnOff(long v) { return v < 0 ? "unknown/not set" : (v ? "ON" : "off"); }

}  // namespace

void LogSystemInfo() {
    // CPU model
    char brand[49] = {};
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid_max(0x80000000, nullptr) >= 0x80000004) {
        for (unsigned i = 0; i < 3; ++i) {
            __cpuid(0x80000002 + i, a, b, c, d);
            std::memcpy(brand + i * 16 + 0, &a, 4);
            std::memcpy(brand + i * 16 + 4, &b, 4);
            std::memcpy(brand + i * 16 + 8, &c, 4);
            std::memcpy(brand + i * 16 + 12, &d, 4);
        }
    }
    const char* p = brand;
    while (*p == ' ') ++p;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    log::Write("system: CPU \"%s\", %lu logical processors in this group, RAM %lu MB", p, static_cast<unsigned long>(si.dwNumberOfProcessors),
               static_cast<unsigned long>(ms.ullTotalPhys / (1024ull * 1024ull)));

    // Windows version (RtlGetVersion is not subject to compatibility shims)
    struct OsVersion {
        ULONG size, major, minor, build, platform;
        WCHAR csd[128];
    } ov{};
    ov.size = sizeof(ov);
    auto rtlGetVersion = Resolve<LONG(WINAPI*)(void*)>(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtlGetVersion && rtlGetVersion(&ov) >= 0) log::Write("system: Windows %lu.%lu build %lu", ov.major, ov.minor, ov.build);

    // registry switches that influence CPU-bound games
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    auto regGet = Resolve<RegGetValueWFn>(adv, "RegGetValueW");
    const long hvci = ReadDword(regGet, HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", L"Enabled");
    const long vbs = ReadDword(regGet, HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", L"EnableVirtualizationBasedSecurity");
    const long gameMode = ReadDword(regGet, HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled");
    log::Write("system: memory integrity (HVCI) %s, virtualization-based security %s, Game Mode %s", OnOff(hvci), OnOff(vbs), OnOff(gameMode));

    // active power plan
    HMODULE pp = LoadLibraryW(L"powrprof.dll");
    auto getScheme = Resolve<DWORD(WINAPI*)(HKEY, GUID**)>(pp, "PowerGetActiveScheme");
    GUID* scheme = nullptr;
    if (getScheme && getScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
        static const GUID kBalanced = {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
        static const GUID kHigh = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
        static const GUID kSaver = {0xa1841308, 0x3541, 0x4fab, {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};
        static const GUID kUltimate = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
        const char* name = "custom";
        if (IsEqualGUID(*scheme, kBalanced)) name = "Balanced";
        else if (IsEqualGUID(*scheme, kHigh)) name = "High performance";
        else if (IsEqualGUID(*scheme, kSaver)) name = "Power saver";
        else if (IsEqualGUID(*scheme, kUltimate)) name = "Ultimate performance";
        log::Write("system: power plan %s", name);
        LocalFree(scheme);
    }
}

}  // namespace cbf
