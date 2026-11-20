#include "sources.h"

#include "MinHook.h"
#include "log.h"

namespace cbf {
namespace sources {

DWORD g_tlsDepth = TLS_OUT_OF_INDEXES;

namespace {
const Adapter* const kAdapters[] = {&kD3D9, &kDXGI, &kOpenGL, &kVulkan, &kD3D8};
constexpr int kCount = sizeof(kAdapters) / sizeof(kAdapters[0]);
constexpr int kMaxAttempts = 3;

bool g_attached[kCount] = {};
int g_attempts[kCount] = {};
bool g_inited = false;
}  // namespace

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
    log::Write("hooked %s", what);
    return true;
}

void Poll() {
    if (!g_inited) return;
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
