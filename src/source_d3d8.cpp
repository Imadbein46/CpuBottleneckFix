// Adapter: Direct3D 8 (old games: GTA III / Vice City, Max Payne, ...). IDirect3DDevice8::Present = vtable slot 15.
// With the loader's UseD3D8to9 option the D3D9 adapter sees the frames as well; PresentScope counts them once.
#include <d3d8.h>

#include "log.h"
#include "sources.h"

namespace cbf {
namespace sources {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);
void* g_present = nullptr;

HRESULT STDMETHODCALLTYPE Present(IDirect3DDevice8* self, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
    PresentScope scope(reinterpret_cast<uintptr_t>(self), "D3D8");
    return reinterpret_cast<PresentFn>(g_present)(self, src, dst, wnd, dirty);
}

bool Loaded() { return GetModuleHandleW(L"d3d8.dll") != nullptr; }

bool Attach() {
    HMODULE d3d8 = GetModuleHandleW(L"d3d8.dll");
    if (!d3d8) return false;
    using Create8 = IDirect3D8*(WINAPI*)(UINT);
    auto create8 = reinterpret_cast<Create8>(reinterpret_cast<void*>(GetProcAddress(d3d8, "Direct3DCreate8")));
    if (!create8) return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CpuBottleneckFixDummyD3D8";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return false;

    bool any = false;
    if (IDirect3D8* d3d = create8(D3D_SDK_VERSION)) {
        D3DDISPLAYMODE mode{};
        if (SUCCEEDED(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode))) {
            D3DPRESENT_PARAMETERS pp{};
            pp.Windowed = TRUE;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.BackBufferFormat = mode.Format;
            pp.hDeviceWindow = window;
            IDirect3DDevice8* dev = nullptr;
            if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev)) && dev) {
                any = InstallHook(Method(dev, 15), reinterpret_cast<void*>(&Present), &g_present, "IDirect3DDevice8::Present");
                dev->Release();
            }
        }
        d3d->Release();
    }
    DestroyWindow(window);
    UnregisterClassW(L"CpuBottleneckFixDummyD3D8", GetModuleHandleW(nullptr));
    return any;
}

}  // namespace

const Adapter kD3D8 = {"D3D8", &Loaded, &Attach, nullptr};

}  // namespace sources
}  // namespace cbf
