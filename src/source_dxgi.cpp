// Adapter: Direct3D 10 / 11 / 12 (all present through IDXGISwapChain, implemented once in dxgi.dll).
// Vtable slots: IDXGISwapChain::Present = 8, IDXGISwapChain1::Present1 = 22.
#include <d3d11.h>
#include <dxgi1_2.h>

#include "log.h"
#include "sources.h"

namespace cbf {
namespace sources {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

void* g_present = nullptr;
void* g_present1 = nullptr;

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return reinterpret_cast<PresentFn>(g_present)(self, sync, flags);  // shows nothing
    PresentScope scope(reinterpret_cast<uintptr_t>(self), "DXGI (D3D10/11/12)");
    return reinterpret_cast<PresentFn>(g_present)(self, sync, flags);
}

HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    if (flags & DXGI_PRESENT_TEST) return reinterpret_cast<Present1Fn>(g_present1)(self, sync, flags, params);
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
        IDXGISwapChain1* chain1 = nullptr;
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1))) && chain1) {
            any |= InstallHook(Method(chain1, 22), reinterpret_cast<void*>(&Present1), &g_present1, "IDXGISwapChain1::Present1");
            chain1->Release();
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

const Adapter kDXGI = {"DXGI", &Loaded, &Attach, nullptr};

}  // namespace sources
}  // namespace cbf
