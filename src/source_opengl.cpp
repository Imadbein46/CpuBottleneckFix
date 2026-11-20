// Adapter: OpenGL. Hooks wglSwapBuffers / wglSwapLayerBuffers (opengl32.dll) and SwapBuffers (gdi32).
// SwapBuffers usually calls into wglSwapBuffers; PresentScope counts only the outermost call.
#include "log.h"
#include "sources.h"

namespace cbf {
namespace sources {
namespace {

using SwapFn = BOOL(WINAPI*)(HDC);
using SwapLayerFn = BOOL(WINAPI*)(HDC, UINT);

void* g_wglSwap = nullptr;
void* g_gdiSwap = nullptr;
void* g_wglSwapLayer = nullptr;

BOOL WINAPI WglSwapBuffers(HDC dc) {
    PresentScope scope(reinterpret_cast<uintptr_t>(dc), "OpenGL");
    return reinterpret_cast<SwapFn>(g_wglSwap)(dc);
}
BOOL WINAPI GdiSwapBuffers(HDC dc) {
    PresentScope scope(reinterpret_cast<uintptr_t>(dc), "OpenGL");
    return reinterpret_cast<SwapFn>(g_gdiSwap)(dc);
}
BOOL WINAPI WglSwapLayerBuffers(HDC dc, UINT planes) {
    PresentScope scope(reinterpret_cast<uintptr_t>(dc), "OpenGL");
    return reinterpret_cast<SwapLayerFn>(g_wglSwapLayer)(dc, planes);
}

bool Loaded() { return GetModuleHandleW(L"opengl32.dll") != nullptr; }

void* Export(const wchar_t* module, const char* name) {
    HMODULE m = GetModuleHandleW(module);
    return m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
}

bool Attach() {
    bool any = false;
    any |= InstallHook(Export(L"opengl32.dll", "wglSwapBuffers"), reinterpret_cast<void*>(&WglSwapBuffers), &g_wglSwap, "wglSwapBuffers");
    any |= InstallHook(Export(L"opengl32.dll", "wglSwapLayerBuffers"), reinterpret_cast<void*>(&WglSwapLayerBuffers), &g_wglSwapLayer, "wglSwapLayerBuffers");
    any |= InstallHook(Export(L"gdi32.dll", "SwapBuffers"), reinterpret_cast<void*>(&GdiSwapBuffers), &g_gdiSwap, "gdi32!SwapBuffers");
    return any;
}

}  // namespace

const Adapter kOpenGL = {"OpenGL", &Loaded, &Attach, nullptr};

}  // namespace sources
}  // namespace cbf
