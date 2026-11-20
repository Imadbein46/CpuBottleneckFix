// Fake "game" for the Wine smoke test: loads the .asi the way Ultimate ASI Loader does, opens a window, then
// presents frames through OpenGL's SwapBuffers (and D3D9 if available) at a steady ~125 fps for a few seconds.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "gl";
    int seconds = argc > 2 ? atoi(argv[2]) : 8;

    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FakeGame";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"FakeGame", L"Fake Game", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);

    // the game loads its graphics DLL first, then the ASI loader runs plugins
    HMODULE gl = nullptr; IDirect3DDevice9* dev = nullptr; IDirect3D9* d3d = nullptr;
    if (mode[0] == 'g') gl = LoadLibraryW(L"opengl32.dll");
    if (mode[0] == 'd') {
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
    }

    HMODULE asi = LoadLibraryW(L"CpuBottleneckFix.asi");
    if (!asi) { std::printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }
    auto init = reinterpret_cast<void (*)()>(reinterpret_cast<void*>(GetProcAddress(asi, "InitializeASI")));
    if (!init) { std::printf("no InitializeASI export\n"); return 3; }
    init();
    std::printf("plugin loaded, InitializeASI called\n");

    if (mode[0] == 'd') {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd;
        HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
        std::printf("D3D9 CreateDevice hr=0x%lx\n", (unsigned long)hr);
        if (FAILED(hr)) return 4;
    }

    HDC dc = GetDC(hwnd);
    DWORD end = GetTickCount() + seconds * 1000;
    long frames = 0;
    MSG msg;
    while ((long)(GetTickCount() - end) < 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (mode[0] == 'g') SwapBuffers(dc);
        else dev->Present(nullptr, nullptr, nullptr, nullptr);
        ++frames;
        Sleep(8);
    }
    std::printf("harness presented %ld frames\n", frames);
    return 0;
}
