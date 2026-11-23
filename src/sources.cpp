#include "sources.h"

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "MinHook.h"
#include "log.h"

namespace cbf {
namespace sources {

DWORD g_tlsDepth = TLS_OUT_OF_INDEXES;
std::atomic<uint32_t> g_rawPresentCalls{0};
std::atomic<uint32_t> g_outerPresentCalls{0};

namespace {
const Adapter* const kAdapters[] = {&kD3D9, &kDXGI, &kOpenGL, &kVulkan, &kD3D8};
constexpr int kCount = sizeof(kAdapters) / sizeof(kAdapters[0]);
constexpr int kMaxAttempts = 3;

bool g_attached[kCount] = {};
int g_attempts[kCount] = {};
bool g_inited = false;

// Remember the first bytes of every hooked function before and after patching, so that VerifyHooks() can tell
// whether our hook is still in place, was restored to the original, or had another hook installed on top.
struct HookRecord {
    void* target;
    unsigned char before[8];
    unsigned char after[8];
    const char* what;
    int state;  // 0 = intact, 1 = restored to original, 2 = changed by something else
};
HookRecord g_records[64];
int g_recordCount = 0;

bool ReadBytes(const void* p, unsigned char* out, SIZE_T n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}
}  // namespace

std::string ModuleOf(const void* address) {
    HMODULE m = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &m) || !m) return "?";
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "?";
    const wchar_t* b = path + n;
    while (b > path && b[-1] != L'\\' && b[-1] != L'/') --b;
    std::string out;
    for (; *b && out.size() < 48; ++b) out.push_back((*b >= 32 && *b < 127) ? static_cast<char>(*b) : '?');
    return out;
}

namespace {
bool StartsWithDir(const wchar_t* path, const wchar_t* dir) {
    const size_t n = std::wcslen(dir);
    return n > 0 && _wcsnicmp(path, dir, n) == 0 && (path[n] == L'\\' || path[n] == L'/');
}
bool ModulePath(const void* address, wchar_t* out, DWORD size) {
    HMODULE m = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &m) || !m) return false;
    const DWORD n = GetModuleFileNameW(m, out, size);
    return n > 0 && n < size;
}
}  // namespace

bool IsWindowsModule(const void* address) {
    wchar_t path[MAX_PATH], win[MAX_PATH];
    if (!ModulePath(address, path, MAX_PATH)) return false;
    const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    return n > 0 && n < MAX_PATH && StartsWithDir(path, win);
}

std::string ModuleKind(const void* address) {
    wchar_t path[MAX_PATH], other[MAX_PATH];
    if (!ModulePath(address, path, MAX_PATH)) return "unknown";
    const UINT w = GetWindowsDirectoryW(other, MAX_PATH);
    if (w > 0 && w < MAX_PATH && StartsWithDir(path, other)) return "Windows folder";
    wchar_t exe[MAX_PATH];
    const DWORD e = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (e > 0 && e < MAX_PATH) {
        wchar_t* slash = std::wcsrchr(exe, L'\\');
        if (slash) {
            *slash = 0;
            if (StartsWithDir(path, exe)) return "game folder";
        }
    }
    return "other folder";
}

void VerifyHooks() {
    for (int i = 0; i < g_recordCount; ++i) {
        HookRecord& r = g_records[i];
        if (r.state != 0) continue;
        unsigned char now[8] = {};
        if (!ReadBytes(r.target, now, 8)) continue;
        if (std::memcmp(now, r.after, 8) == 0) continue;
        if (std::memcmp(now, r.before, 8) == 0) {
            r.state = 1;
            log::Write("hook check: %s was RESTORED to its original bytes - something removed our hook", r.what);
        } else {
            r.state = 2;
            log::Write("hook check: %s first bytes changed - another hook was installed on top of ours (usually harmless)", r.what);
        }
    }
}

bool Init() {
    if (g_inited) return true;
    g_tlsDepth = TlsAlloc();
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        log::Write("MinHook initialisation failed: %s", MH_StatusToString(st));
        return false;
    }
    g_inited = true;
    return true;
}

bool InstallHook(void* target, void* detour, void** original, const char* what) {
    if (!target) return false;
    unsigned char before[8] = {};
    ReadBytes(target, before, 8);
    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st == MH_ERROR_ALREADY_CREATED) return true;  // another detour with the same signature already covers it
    if (st != MH_OK) {
        log::Write("hook %s: create failed: %s", what, MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK) {
        log::Write("hook %s: enable failed: %s", what, MH_StatusToString(st));
        MH_RemoveHook(target);
        return false;
    }
    log::Write("hooked %s (in %s, %s)", what, ModuleOf(target).c_str(), ModuleKind(target).c_str());
    if (!IsWindowsModule(target)) {
        // The function we hooked belongs to another injected tool: it sits between the game and Windows, and runs
        // on the same thread as the game's Present call.
        static char reported[8][48];
        static int reportedCount = 0;
        const std::string mod = ModuleOf(target);
        bool seen = false;
        for (int i = 0; i < reportedCount; ++i) seen = seen || mod == reported[i];
        if (!seen && reportedCount < 8) {
            std::snprintf(reported[reportedCount++], sizeof(reported[0]), "%s", mod.c_str());
            log::Write("note: %s is provided by %s (%s), not by Windows: another tool is in the swap-chain path and costs CPU time on the thread that presents frames",
                       what, mod.c_str(), ModuleKind(target).c_str());
        }
    }
    if (g_recordCount < static_cast<int>(sizeof(g_records) / sizeof(g_records[0]))) {
        HookRecord& r = g_records[g_recordCount++];
        r.target = target;
        r.what = what;
        r.state = 0;
        std::memcpy(r.before, before, 8);
        std::memset(r.after, 0, 8);
        ReadBytes(target, r.after, 8);
    }
    return true;
}

void Poll() {
    if (!g_inited) return;
    static int verifyCounter = 0;
    if (++verifyCounter >= 8) {  // every ~2 s
        verifyCounter = 0;
        VerifyHooks();
    }
    for (int i = 0; i < kCount; ++i) {
        const Adapter& a = *kAdapters[i];
        if (!g_attached[i] && g_attempts[i] < kMaxAttempts && a.apiLoaded()) {
            ++g_attempts[i];
            g_attached[i] = a.attach();
            log::Write("adapter %s: %s (attempt %d)", a.name, g_attached[i] ? "attached" : "FAILED", g_attempts[i]);
        }
        if (g_attached[i] && a.poll) a.poll();
    }
}

std::string AttachedNames() {
    std::string out;
    for (int i = 0; i < kCount; ++i) {
        if (!g_attached[i]) continue;
        if (!out.empty()) out += ", ";
        out += kAdapters[i]->name;
    }
    return out;
}

void Shutdown() {
    if (!g_inited) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_inited = false;
}

}  // namespace sources
}  // namespace cbf
