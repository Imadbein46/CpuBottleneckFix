// The "frame source" seam. Every graphics API is one adapter that hooks that API's present call and reports
// "a frame was presented" through PresentScope. Everything else in the plugin only sees frames, never APIs.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "frame_clock.h"

namespace cbf {

// Implemented by the plugin core (plugin.cpp); called from the render thread by PresentScope.
FrameClock::Ticket OnPresentBegin(uintptr_t key, const char* api);
void OnPresentEnd(const FrameClock::Ticket& ticket);

namespace sources {

// One graphics API.
struct Adapter {
    const char* name;
    bool (*apiLoaded)();   // is this API's DLL present in the process right now?
    bool (*attach)();      // install hooks; true if at least one hook is live
    void (*poll)();        // optional, called every Poll() once attached (late work); may be null
};

// What the swap chain and the Present calls say about how the CPU is synchronised with the GPU / display.
struct SyncInfo {
    bool known = false;
    int presentInterval = -1;       // 0 = no vsync, 1..4 = vsync every N refreshes
    uint32_t presentFlags = 0;      // DXGI_PRESENT_* of the last call (0x200 = allow tearing)
    uint32_t bufferCount = 0;
    int swapEffect = -1;            // DXGI_SWAP_EFFECT (3 and 4 are flip model)
    uint32_t swapChainFlags = 0;
    int frameLatency = -1;          // D3D11: how many frames the CPU may queue ahead; -1 = unknown / not applicable
};
SyncInfo GetSyncInfo();
// Limit how many frames the CPU may queue ahead of the GPU (Direct3D 11 / DXGI device). 0 = put the game's own
// value back. Applied from the presenting thread at the next Present; has no effect on Direct3D 12.
void SetFrameLatency(int frames);

bool Init();                    // initialises MinHook and the thread-local storage. Call once.
void Shutdown();                // removes every hook
void Poll();                    // attaches adapters whose API appeared; call every ~250 ms from ONE thread
std::string AttachedNames();    // e.g. "D3D9, DXGI" (for the log / overlay)

// ---- used by the adapters ----
extern DWORD g_tlsDepth;
extern std::atomic<uint32_t> g_rawPresentCalls;  // every call that reached a present hook (before nesting is resolved)
extern std::atomic<uint32_t> g_outerPresentCalls;
bool InstallHook(void* target, void* detour, void** original, const char* what);
std::string ModuleOf(const void* address);
std::string ModuleKind(const void* address);   // "Windows folder", "game folder", "other folder" or "unknown"
bool IsWindowsModule(const void* address);     // the code at `address` is inside the Windows folder   // base name of the module containing `address`, or "?"
void VerifyHooks();                          // logs (once per hook) if our patched bytes were restored or overwritten

// Brackets one call of the real present function. Present calls often nest (a device Present calls the swap
// chain's Present, SwapBuffers calls wglSwapBuffers...): only the outermost call is measured. The Windows
// last-error value is preserved so the game never notices the bookkeeping.
class PresentScope {
public:
    PresentScope(uintptr_t key, const char* api) {
        const DWORD saved = GetLastError();
        g_rawPresentCalls.fetch_add(1, std::memory_order_relaxed);
        if (g_tlsDepth != TLS_OUT_OF_INDEXES) {
            const uintptr_t depth = reinterpret_cast<uintptr_t>(TlsGetValue(g_tlsDepth));
            TlsSetValue(g_tlsDepth, reinterpret_cast<LPVOID>(depth + 1));
            tracked_ = true;
            outer_ = depth == 0;
        }
        if (outer_) {
            g_outerPresentCalls.fetch_add(1, std::memory_order_relaxed);
            ticket_ = OnPresentBegin(key, api);
        }
        SetLastError(saved);
    }
    ~PresentScope() {
        const DWORD callError = GetLastError();
        if (outer_) OnPresentEnd(ticket_);
        if (tracked_) {
            const uintptr_t depth = reinterpret_cast<uintptr_t>(TlsGetValue(g_tlsDepth));
            TlsSetValue(g_tlsDepth, reinterpret_cast<LPVOID>(depth ? depth - 1 : 0));
        }
        SetLastError(callError);
    }
    PresentScope(const PresentScope&) = delete;
    PresentScope& operator=(const PresentScope&) = delete;

private:
    bool outer_ = true;     // without TLS every call counts
    bool tracked_ = false;
    FrameClock::Ticket ticket_;
};

// The adapters (one .cpp each)
extern const Adapter kD3D8;
extern const Adapter kD3D9;
extern const Adapter kDXGI;
extern const Adapter kOpenGL;
extern const Adapter kVulkan;

// vtable entry `index` of a COM object
inline void* Method(void* object, int index) { return (*reinterpret_cast<void***>(object))[index]; }

}  // namespace sources
}  // namespace cbf
