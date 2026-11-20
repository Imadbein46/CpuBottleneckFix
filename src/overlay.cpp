#include "overlay.h"

#include <algorithm>

#include "log.h"

namespace cbf {
namespace {
const wchar_t kClassName[] = L"CpuBottleneckFixOverlay";
constexpr int kMargin = 10;

bool OurProcessHasForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
}  // namespace

Overlay::~Overlay() { Destroy(); }

LRESULT CALLBACK Overlay::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(h, msg, wp, lp);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(h, GWLP_USERDATA))) self->Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        default: return DefWindowProcW(h, msg, wp, lp);
    }
}

bool Overlay::Create(HINSTANCE module, int opacity, int fontPt) {
    if (hwnd_) return true;
    module_ = module;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Overlay::WndProc;
    wc.hInstance = module;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) classRegistered_ = true;
    if (!classRegistered_) {
        log::Write("overlay: RegisterClassEx failed (%lu)", static_cast<unsigned long>(GetLastError()));
        return false;
    }

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP, 0, 0, 10, 10,
                            nullptr, nullptr, module, this);
    if (!hwnd_) {
        log::Write("overlay: CreateWindowEx failed (%lu)", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(std::min(255, std::max(0, opacity))), LWA_ALPHA);

    HDC dc = GetDC(hwnd_);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(hwnd_, dc);
    font_ = CreateFontW(-MulDiv(fontPt, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    pad_ = std::max(4, MulDiv(6, dpi, 96));
    return true;
}

void Overlay::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (classRegistered_) {
        UnregisterClassW(kClassName, module_);
        classRegistered_ = false;
    }
    visible_ = false;
}

void Overlay::Measure() {
    HDC dc = GetDC(hwnd_);
    if (!dc) return;
    HGDIOBJ old = SelectObject(dc, font_);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    lineHeight_ = std::max<int>(8, tm.tmHeight + 2);
    int maxW = 0;
    for (const auto& l : lines_) {
        SIZE sz{};
        if (GetTextExtentPoint32W(dc, l.text.c_str(), static_cast<int>(l.text.size()), &sz)) maxW = std::max<int>(maxW, sz.cx);
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    width_ = maxW + 2 * pad_;
    height_ = static_cast<int>(lines_.size()) * lineHeight_ + 2 * pad_;
}

void Overlay::Paint(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(14, 14, 14));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    HGDIOBJ old = SelectObject(dc, font_);
    SetBkMode(dc, TRANSPARENT);
    int y = pad_;
    for (const auto& l : lines_) {
        SetTextColor(dc, l.color);
        TextOutW(dc, pad_, y, l.text.c_str(), static_cast<int>(l.text.size()));
        y += lineHeight_;
    }
    SelectObject(dc, old);
}

void Overlay::Hide() {
    if (hwnd_ && visible_) {
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }
}

void Overlay::Update(const std::vector<OverlayLine>& lines, HWND game, Corner corner) {
    if (!hwnd_) return;
    const bool canShow = !lines.empty() && game && IsWindow(game) && IsWindowVisible(game) && !IsIconic(game) && OurProcessHasForeground();
    if (!canShow) {
        Hide();
        return;
    }

    const bool textChanged = !(lines == lines_);
    if (textChanged) {
        lines_ = lines;
        Measure();
    }

    RECT client{};
    POINT origin{0, 0};
    if (!GetClientRect(game, &client) || !ClientToScreen(game, &origin)) {
        Hide();
        return;
    }
    const int cw = client.right - client.left, ch = client.bottom - client.top;
    int x = origin.x + kMargin, y = origin.y + kMargin;
    if (corner == Corner::TopRight || corner == Corner::BottomRight) x = origin.x + cw - width_ - kMargin;
    if (corner == Corner::BottomLeft || corner == Corner::BottomRight) y = origin.y + ch - height_ - kMargin;

    const bool moved = x != x_ || y != y_;
    const bool wasVisible = visible_;
    UINT flags = SWP_NOACTIVATE;
    if (!wasVisible) flags |= SWP_SHOWWINDOW;
    // Re-asserting HWND_TOPMOST each refresh keeps us above games that re-order their own topmost window.
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, flags);
    x_ = x;
    y_ = y;
    visible_ = true;
    if (textChanged || moved || !wasVisible) InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace cbf
