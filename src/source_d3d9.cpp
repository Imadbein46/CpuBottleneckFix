// Adapter: Direct3D 9 / 9Ex. Throwaway devices on a hidden window give us the method addresses (all devices
// of a class share the code), which are then inline-hooked. Vtable slots: IDirect3DDevice9::Present = 17,
// IDirect3DSwapChain9::Present = 3, IDirect3DDevice9Ex::PresentEx = 121.
#include <d3d9.h>

#include "log.h"
#include "sources.h"

namespace cbf {
namespace sources {
namespace {

using DevicePresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SwapPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using DevicePresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

// index 0 = plain device classes, 1 = Ex device classes (they can be different code)
void* g_devicePresent[2] = {};
void* g_swapPresent[2] = {};
void* g_devicePresentEx = nullptr;

template <int N>
HRESULT STDMETHODCALLTYPE DevicePresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
    PresentScope scope(reinterpret_cast<uintptr_t>(self), "D3D9");
    return reinterpret_cast<DevicePresentFn>(g_devicePresent[N])(self, src, dst, wnd, dirty);
}

template <int N>
HRESULT STDMETHODCALLTYPE SwapChainPresent(IDirect3DSwapChain9* self, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty, DWORD flags) {
    PresentScope scope(reinterpret_cast<uintptr_t>(self), "D3D9");
    return reinterpret_cast<SwapPresentFn>(g_swapPresent[N])(self, src, dst, wnd, dirty, flags);
}

HRESULT STDMETHODCALLTYPE DevicePresentEx(IDirect3DDevice9Ex* self, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty, DWORD flags) {
    PresentScope scope(reinterpret_cast<uintptr_t>(self), "D3D9");
    return reinterpret_cast<DevicePresentExFn>(g_devicePresentEx)(self, src, dst, wnd, dirty, flags);
}

bool Loaded() { return GetModuleHandleW(L"d3d9.dll") != nullptr; }

D3DPRESENT_PARAMETERS DummyParams(HWND window) {
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    return pp;
}

// FPU_PRESERVE matters: without it D3D9 lowers the x87 precision of the creating thread (ours) to single float.
constexpr DWORD kDummyFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT | D3DCREATE_FPU_PRESERVE;

HWND MakeWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CpuBottleneckFixDummyD3D9";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
}

bool Attach() {
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) return false;
    HWND window = MakeWindow();
    if (!window) return false;
    bool any = false;

    using Create9 = IDirect3D9*(WINAPI*)(UINT);
    using Create9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    auto create9 = reinterpret_cast<Create9>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9")));
    auto create9Ex = reinterpret_cast<Create9Ex>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9Ex")));

    if (create9) {
        if (IDirect3D9* d3d = create9(D3D_SDK_VERSION)) {
            D3DPRESENT_PARAMETERS pp = DummyParams(window);
            IDirect3DDevice9* dev = nullptr;
            if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, kDummyFlags, &pp, &dev)) && dev) {
                any |= InstallHook(Method(dev, 17), reinterpret_cast<void*>(&DevicePresent<0>), &g_devicePresent[0], "IDirect3DDevice9::Present");
                IDirect3DSwapChain9* chain = nullptr;
                if (SUCCEEDED(dev->GetSwapChain(0, &chain)) && chain) {
                    any |= InstallHook(Method(chain, 3), reinterpret_cast<void*>(&SwapChainPresent<0>), &g_swapPresent[0], "IDirect3DSwapChain9::Present");
                    chain->Release();
                }
                dev->Release();
            }
            d3d->Release();
        }
    }
    if (create9Ex) {
        IDirect3D9Ex* d3dEx = nullptr;
        if (SUCCEEDED(create9Ex(D3D_SDK_VERSION, &d3dEx)) && d3dEx) {
            D3DPRESENT_PARAMETERS pp = DummyParams(window);
            IDirect3DDevice9Ex* dev = nullptr;
            if (SUCCEEDED(d3dEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, kDummyFlags, &pp, nullptr, &dev)) && dev) {
                // a game may call plain Present on an Ex device: cover that too (skipped if the code is shared)
                any |= InstallHook(Method(dev, 17), reinterpret_cast<void*>(&DevicePresent<1>), &g_devicePresent[1], "IDirect3DDevice9Ex::Present");
                any |= InstallHook(Method(dev, 121), reinterpret_cast<void*>(&DevicePresentEx), &g_devicePresentEx, "IDirect3DDevice9Ex::PresentEx");
                IDirect3DSwapChain9* chain = nullptr;
                if (SUCCEEDED(dev->GetSwapChain(0, &chain)) && chain) {
                    any |= InstallHook(Method(chain, 3), reinterpret_cast<void*>(&SwapChainPresent<1>), &g_swapPresent[1], "IDirect3DSwapChain9Ex::Present");
                    chain->Release();
                }
                dev->Release();
            }
            d3dEx->Release();
        }
    }
    DestroyWindow(window);
    UnregisterClassW(L"CpuBottleneckFixDummyD3D9", GetModuleHandleW(nullptr));
    return any;
}

}  // namespace

const Adapter kD3D9 = {"D3D9", &Loaded, &Attach, nullptr};

}  // namespace sources
}  // namespace cbf
