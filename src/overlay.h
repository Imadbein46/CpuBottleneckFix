// Overlay: a small click-through, always-on-top, semi-transparent window that sits over the game window.
// Because it is an ordinary window (not drawn through the game's graphics API) it works the same for
// Direct3D 8/9/10/11/12, OpenGL and Vulkan. Limitation: it cannot appear over true exclusive fullscreen.
// All methods must be called from the thread that created it (it needs a message pump on that thread).
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "config.h"

namespace cbf {

struct OverlayLine {
    std::wstring text;
    COLORREF color = RGB(255, 255, 255);
    bool operator==(const OverlayLine& o) const { return color == o.color && text == o.text; }
};

class Overlay {
public:
    Overlay() = default;
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool Create(HINSTANCE module, int opacity, int fontPt);
    void Destroy();

    // Shows `lines` over `gameWindow` in the given corner, or hides the overlay when there is nothing to show,
    // the game window is minimised, or another application has the focus.
    void Update(const std::vector<OverlayLine>& lines, HWND gameWindow, Corner corner);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC dc);
    void Measure();
    void Hide();

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HINSTANCE module_ = nullptr;
    std::vector<OverlayLine> lines_;
    int width_ = 0, height_ = 0, lineHeight_ = 16, pad_ = 6;
    int x_ = 0, y_ = 0;
    bool visible_ = false;
    bool classRegistered_ = false;
};

}  // namespace cbf
