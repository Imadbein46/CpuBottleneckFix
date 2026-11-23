// Adapter: Direct3D 10 / 11 / 12 (all present through IDXGISwapChain, implemented once in dxgi.dll).
// Vtable slots: IDXGISwapChain::Present = 8, IDXGISwapChain1::Present1 = 22.
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>

#include "log.h"
#include "sources.h"

namespace cbf {
namespace sources {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

void* g_present = nullptr;
void* g_present1 = nullptr;

// ---- how the CPU is synchronised with the GPU: what Present asks for, what the swap chain is, and an optional
// ---- cap on the number of frames the CPU may queue ahead (IDXGIDevice1::SetMaximumFrameLatency)
std::atomic<int> g_syncInterval{-1};
std::atomic<uint32_t> g_presentFlags{0};
std::atomic<void*> g_infoSc{nullptr};
std::atomic<uint32_t> g_buffers{0}, g_scFlags{0};
std::atomic<int> g_swapEffect{-1}, g_devLatency{-1};
std::atomic<bool> g_infoKnown{false};
std::atomic<int> g_wantLatency{0};
std::atomic<void*> g_latSc{nullptr};
std::atomic<int> g_latApplied{0};
std::atomic_flag g_applyLock = ATOMIC_FLAG_INIT;
UINT g_origLatency = 0;   // the game's own value, remembered while we override it (guarded by g_applyLock)
bool g_origKnown = false;

void CaptureSyncInfo(IDXGISwapChain* sc) {
    g_infoSc.store(sc, std::memory_order_relaxed);  // set first: a failure below must not make every frame retry
    bool ok = false;
    IDXGISwapChain1* s1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&s1))) && s1) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        if (SUCCEEDED(s1->GetDesc1(&d))) {
            g_buffers.store(d.BufferCount, std::memory_order_relaxed);
            g_swapEffect.store(static_cast<int>(d.SwapEffect), std::memory_order_relaxed);
            g_scFlags.store(d.Flags, std::memory_order_relaxed);
            ok = true;
        }
        s1->Release();
    }
    if (!ok) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) {
            g_buffers.store(d.BufferCount, std::memory_order_relaxed);
            g_swapEffect.store(static_cast<int>(d.SwapEffect), std::memory_order_relaxed);
            g_scFlags.store(d.Flags, std::memory_order_relaxed);
            ok = true;
        }
    }
    int latency = -1;
    IDXGIDevice1* dev = nullptr;  // only a Direct3D 10/11 swap chain has a DXGI device; D3D12 returns a command queue
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
        UINT l = 0;
        if (SUCCEEDED(dev->GetMaximumFrameLatency(&l))) latency = static_cast<int>(l);
        dev->Release();
    }
    g_devLatency.store(latency, std::memory_order_relaxed);
    if (ok) g_infoKnown.store(true, std::memory_order_relaxed);
}

void ApplyLatency(IDXGISwapChain* sc, int want) {
    if (g_applyLock.test_and_set(std::memory_order_acquire)) return;  // another presenting thread is doing it; retry next frame
    IDXGIDevice1* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
        UINT cur = 0;
        if (SUCCEEDED(dev->GetMaximumFrameLatency(&cur))) {
            if (want > 0) {
                if (!g_origKnown) {
                    g_origLatency = cur;
                    g_origKnown = true;
                }
                if (cur != static_cast<UINT>(want)) dev->SetMaximumFrameLatency(static_cast<UINT>(want));
            } else if (g_origKnown) {
                if (cur != g_origLatency) dev->SetMaximumFrameLatency(g_origLatency);
                g_origKnown = false;
            }
            g_devLatency.store(want > 0 ? want : static_cast<int>(g_origLatency ? g_origLatency : cur), std::memory_order_relaxed);
        }
        dev->Release();
    }
    g_latSc.store(sc, std::memory_order_relaxed);
    g_latApplied.store(want, std::memory_order_relaxed);
    g_applyLock.clear(std::memory_order_release);
}

// Runs on the presenting thread right before the real Present: two stores and two comparisons per frame.
inline void SyncHook(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    g_syncInterval.store(static_cast<int>(syncInterval), std::memory_order_relaxed);
    g_presentFlags.store(flags, std::memory_order_relaxed);
    if (sc != g_infoSc.load(std::memory_order_relaxed)) CaptureSyncInfo(sc);
    const int want = g_wantLatency.load(std::memory_order_relaxed);
    if (want != g_latApplied.load(std::memory_order_relaxed) || (want > 0 && sc != g_latSc.load(std::memory_order_relaxed))) ApplyLatency(sc, want);
}

// ---- the first few calls are logged in detail: which object, which thread, which code the vtable points to ----
std::atomic<uint32_t> g_callsLogged{0};
void LogFirstCalls(IDXGISwapChain* self, const char* which, void* hookedTarget) {
    if (g_callsLogged.fetch_add(1, std::memory_order_relaxed) >= 3) return;
    void* slot = Method(self, 8);
    if (slot == hookedTarget) {
        log::Write("DXGI %s called: swap chain %p, thread %lu, its Present code (%s) is the code that was hooked", which, static_cast<void*>(self),
                   static_cast<unsigned long>(GetCurrentThreadId()), ModuleOf(slot).c_str());
    } else {
        // not an error: another tool's function sits in the swap chain's vtable and calls through to the hooked code
        log::Write("DXGI %s called: swap chain %p, thread %lu, its Present entry is in %s (%s) and calls on into the hooked code in %s", which, static_cast<void*>(self),
                   static_cast<unsigned long>(GetCurrentThreadId()), ModuleOf(slot).c_str(), ModuleKind(slot).c_str(), ModuleOf(hookedTarget).c_str());
    }
}

// ---- swap chains the game really creates: learn their class, in case it differs from the dummy's ----
constexpr int kMaxDiscovered = 8;
std::atomic<void*> g_discPresent[kMaxDiscovered];
std::atomic<void*> g_discPresent1[kMaxDiscovered];
void* g_known[32];
int g_knownCount = 0;
bool Known(void* p) {
    for (int i = 0; i < g_knownCount; ++i) if (g_known[i] == p) return true;
    return false;
}
void MarkKnown(void* p) {
    if (p && !Known(p) && g_knownCount < 32) g_known[g_knownCount++] = p;
}
void Record(std::atomic<void*>* arr, void* p) {
    if (!p) return;
    for (int i = 0; i < kMaxDiscovered; ++i) {
        void* v = arr[i].load(std::memory_order_relaxed);
        if (v == p) return;
        void* expected = nullptr;
        if (v == nullptr && arr[i].compare_exchange_strong(expected, p)) return;
    }
}
// Runs inside the game's swap-chain creation call. Takes no lasting reference and only records two addresses.
void NoteSwapChain(IDXGISwapChain* sc) {
    if (!sc) return;
    Record(g_discPresent, Method(sc, 8));
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        Record(g_discPresent1, Method(sc1, 22));
        sc1->Release();
    }
}

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateForCoreFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateForCompFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
void* g_origCreate = nullptr;
void* g_origCreateHwnd = nullptr;
void* g_origCreateCore = nullptr;
void* g_origCreateComp = nullptr;

HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    const HRESULT hr = reinterpret_cast<CreateSwapChainFn>(g_origCreate)(self, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out) NoteSwapChain(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateForHwndHook(IDXGIFactory2* self, IUnknown* dev, HWND wnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    const HRESULT hr = reinterpret_cast<CreateForHwndFn>(g_origCreateHwnd)(self, dev, wnd, d, fs, o, out);
    if (SUCCEEDED(hr) && out && *out) NoteSwapChain(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateForCoreHook(IDXGIFactory2* self, IUnknown* dev, IUnknown* core, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** out) {
    const HRESULT hr = reinterpret_cast<CreateForCoreFn>(g_origCreateCore)(self, dev, core, d, o, out);
    if (SUCCEEDED(hr) && out && *out) NoteSwapChain(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateForCompHook(IDXGIFactory2* self, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** out) {
    const HRESULT hr = reinterpret_cast<CreateForCompFn>(g_origCreateComp)(self, dev, d, o, out);
    if (SUCCEEDED(hr) && out && *out) NoteSwapChain(*out);
    return hr;
}

// Extra Present hooks for classes found at creation time (a handful of slots is plenty).
void* g_extraPresentOrig[3] = {};
void* g_extraPresent1Orig[3] = {};
int g_extraPresentUsed = 0, g_extraPresent1Used = 0;
template <int N>
HRESULT STDMETHODCALLTYPE ExtraPresent(IDXGISwapChain* self, UINT sync, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return reinterpret_cast<PresentFn>(g_extraPresentOrig[N])(self, sync, flags);
    SyncHook(self, sync, flags);
    PresentScope scope(reinterpret_cast<uintptr_t>(self), "DXGI (D3D10/11/12)");
    return reinterpret_cast<PresentFn>(g_extraPresentOrig[N])(self, sync, flags);
}
template <int N>
HRESULT STDMETHODCALLTYPE ExtraPresent1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    if (flags & DXGI_PRESENT_TEST) return reinterpret_cast<Present1Fn>(g_extraPresent1Orig[N])(self, sync, flags, params);
    SyncHook(self, sync, flags);
    PresentScope scope(reinterpret_cast<uintptr_t>(self), "DXGI (D3D10/11/12)");
    return reinterpret_cast<Present1Fn>(g_extraPresent1Orig[N])(self, sync, flags, params);
}
void* const kExtraPresent[3] = {reinterpret_cast<void*>(&ExtraPresent<0>), reinterpret_cast<void*>(&ExtraPresent<1>), reinterpret_cast<void*>(&ExtraPresent<2>)};
void* const kExtraPresent1[3] = {reinterpret_cast<void*>(&ExtraPresent1<0>), reinterpret_cast<void*>(&ExtraPresent1<1>), reinterpret_cast<void*>(&ExtraPresent1<2>)};

void PollDiscovery() {
    for (int i = 0; i < kMaxDiscovered; ++i) {
        void* p = g_discPresent[i].load(std::memory_order_relaxed);
        if (p && !Known(p)) {
            MarkKnown(p);  // even on failure: never retry in a loop
            log::Write("a swap chain created by the game uses Present code that was not hooked yet (in %s)", ModuleOf(p).c_str());
            if (g_extraPresentUsed < 3) {
                const int k = g_extraPresentUsed++;
                InstallHook(p, kExtraPresent[k], &g_extraPresentOrig[k], "discovered IDXGISwapChain::Present");
            }
        }
        void* q = g_discPresent1[i].load(std::memory_order_relaxed);
        if (q && !Known(q)) {
            MarkKnown(q);
            log::Write("a swap chain created by the game uses Present1 code that was not hooked yet (in %s)", ModuleOf(q).c_str());
            if (g_extraPresent1Used < 3) {
                const int k = g_extraPresent1Used++;
                InstallHook(q, kExtraPresent1[k], &g_extraPresent1Orig[k], "discovered IDXGISwapChain1::Present1");
            }
        }
    }
}

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return reinterpret_cast<PresentFn>(g_present)(self, sync, flags);  // shows nothing
    LogFirstCalls(self, "Present", g_present);
    SyncHook(self, sync, flags);
    PresentScope scope(reinterpret_cast<uintptr_t>(self), "DXGI (D3D10/11/12)");
    return reinterpret_cast<PresentFn>(g_present)(self, sync, flags);
}

HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    if (flags & DXGI_PRESENT_TEST) return reinterpret_cast<Present1Fn>(g_present1)(self, sync, flags, params);
    LogFirstCalls(self, "Present1", g_present1);
    SyncHook(self, sync, flags);
    PresentScope scope(reinterpret_cast<uintptr_t>(self), "DXGI (D3D10/11/12)");
    return reinterpret_cast<Present1Fn>(g_present1)(self, sync, flags, params);
}

bool Loaded() { return GetModuleHandleW(L"dxgi.dll") != nullptr; }

bool Attach() {
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")))
                        : nullptr;
    if (!create) return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CpuBottleneckFixDummyDXGI";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;

    IDXGISwapChain* chain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    for (D3D_DRIVER_TYPE type : types) {
        if (SUCCEEDED(create(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &chain, &device, nullptr, &context)) && chain) break;
        chain = nullptr;
    }

    bool any = false;
    if (chain) {
        any |= InstallHook(Method(chain, 8), reinterpret_cast<void*>(&Present), &g_present, "IDXGISwapChain::Present");
        MarkKnown(Method(chain, 8));
        IDXGISwapChain1* chain1 = nullptr;
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1))) && chain1) {
            any |= InstallHook(Method(chain1, 22), reinterpret_cast<void*>(&Present1), &g_present1, "IDXGISwapChain1::Present1");
            MarkKnown(Method(chain1, 22));
            chain1->Release();
        }
        // watch the swap chains the game creates (vtable slots: IDXGIFactory::CreateSwapChain = 10,
        // IDXGIFactory2::CreateSwapChainForHwnd = 15, ForCoreWindow = 16, ForComposition = 24)
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(chain->GetParent(IID_PPV_ARGS(&factory))) && factory) {
            InstallHook(Method(factory, 10), reinterpret_cast<void*>(&CreateSwapChainHook), &g_origCreate, "IDXGIFactory::CreateSwapChain");
            IDXGIFactory2* factory2 = nullptr;
            if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))) && factory2) {
                InstallHook(Method(factory2, 15), reinterpret_cast<void*>(&CreateForHwndHook), &g_origCreateHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
                InstallHook(Method(factory2, 16), reinterpret_cast<void*>(&CreateForCoreHook), &g_origCreateCore, "IDXGIFactory2::CreateSwapChainForCoreWindow");
                InstallHook(Method(factory2, 24), reinterpret_cast<void*>(&CreateForCompHook), &g_origCreateComp, "IDXGIFactory2::CreateSwapChainForComposition");
                factory2->Release();
            }
            factory->Release();
        }
    }
    if (context) context->Release();
    if (device) device->Release();
    if (chain) chain->Release();
    DestroyWindow(window);
    UnregisterClassW(L"CpuBottleneckFixDummyDXGI", GetModuleHandleW(nullptr));
    return any;
}

}  // namespace

SyncInfo GetSyncInfo() {
    SyncInfo s;
    s.known = g_infoKnown.load(std::memory_order_relaxed);
    s.presentInterval = g_syncInterval.load(std::memory_order_relaxed);
    s.presentFlags = g_presentFlags.load(std::memory_order_relaxed);
    s.bufferCount = g_buffers.load(std::memory_order_relaxed);
    s.swapEffect = g_swapEffect.load(std::memory_order_relaxed);
    s.swapChainFlags = g_scFlags.load(std::memory_order_relaxed);
    s.frameLatency = g_devLatency.load(std::memory_order_relaxed);
    return s;
}

void SetFrameLatency(int frames) { g_wantLatency.store(frames < 0 ? 0 : (frames > 8 ? 8 : frames), std::memory_order_relaxed); }

const Adapter kDXGI = {"DXGI", &Loaded, &Attach, &PollDiscovery};

}  // namespace sources
}  // namespace cbf
