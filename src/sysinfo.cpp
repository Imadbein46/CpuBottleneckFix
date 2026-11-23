#include "sysinfo.h"

#include <windows.h>

#include <cpuid.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"
#include "power_guids.h"

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
    // Hardware-accelerated GPU scheduling: the GPU schedules its own work instead of the CPU doing it (2 = on, 1 = off)
    const long hags = ReadDword(regGet, HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode");
    log::Write("system: hardware GPU scheduling (HAGS) %s", hags == 2 ? "ON" : (hags == 1 ? "off" : "unknown/not set"));

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

        // The plan's CPU limits (plugged-in values). Below 100 % means the plan holds the CPU back.
        static_assert(sizeof(GuidBytes) == sizeof(GUID), "GuidBytes must have the layout of GUID");
        auto readAc = Resolve<DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, PDWORD)>(pp, "PowerReadACValueIndex");
        auto value = [&](const GuidBytes& setting) -> long {
            DWORD v = 0;
            if (!readAc || readAc(nullptr, scheme, reinterpret_cast<const GUID*>(&kGuidProcessorSubgroup), reinterpret_cast<const GUID*>(&setting), &v) != ERROR_SUCCESS) return -1;
            return static_cast<long>(v);
        };
        const long minState = value(kGuidMinProcessorState), maxState = value(kGuidMaxProcessorState), parkMin = value(kGuidCoreParkingMinCores), boost = value(kGuidPerfBoostMode);
        auto num = [](long v) { return v < 0 ? std::string("n/a") : std::to_string(v); };
        log::Write("system: power plan CPU limits (plugged in): min processor state %s%%, max processor state %s%%, core parking keeps at least %s%% of cores awake, boost mode %s (0 disabled, 1 enabled, 2 aggressive)",
                   num(minState).c_str(), num(maxState).c_str(), num(parkMin).c_str(), num(boost).c_str());
        if (maxState >= 0 && maxState < 100) log::Write("system: WARNING the power plan caps the CPU at %ld%% of its speed", maxState);
        if (parkMin >= 0 && parkMin < 100) log::Write("system: note: core parking may put up to %ld%% of the cores to sleep when they look idle", 100 - parkMin);
        if (boost == 0) log::Write("system: WARNING processor boost is disabled in the power plan");
        LocalFree(scheme);
    }
}

void LogLoadedModules() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enumModules = Resolve<BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD)>(k32, "K32EnumProcessModules");
    auto baseName = Resolve<DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD)>(k32, "K32GetModuleBaseNameW");
    if (!enumModules || !baseName) return;
    std::vector<HMODULE> mods(1024);
    DWORD needed = 0;
    if (!enumModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed)) return;
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), mods.size());
    std::string line = "modules:";
    for (size_t i = 0; i < count; ++i) {
        wchar_t name[128];
        if (!baseName(GetCurrentProcess(), mods[i], name, 128)) continue;
        std::string n;
        for (const wchar_t* p = name; *p && n.size() < 60; ++p) n.push_back((*p >= 32 && *p < 127) ? static_cast<char>(*p) : '?');
        if (line.size() + n.size() + 2 > 460) {
            log::Write("%s", line.c_str());
            line = "modules (cont.):";
        }
        line += " " + n;
    }
    log::Write("%s", line.c_str());
}

}  // namespace cbf
