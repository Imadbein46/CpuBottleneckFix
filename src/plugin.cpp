// CpuBottleneckFix - ASI plugin for Ultimate ASI Loader.
//
// Entry points: InitializeASI() (called by the loader, outside the loader lock) and DllMain (which only starts
// the same initialisation from a thread, for ASI loaders that merely LoadLibrary the file).
//
// Threads: the game's render thread only runs the tiny hooks in sources.*. One worker thread of ours does
// everything else (hotkeys, overlay window, CPU sampling, hook attachment), so the game's threads never wait
// for us.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

#include "bottleneck.h"
#include "config.h"
#include "cpu_tuner.h"
#include "frame_clock.h"
#include "log.h"
#include "overlay.h"
#include "sources.h"

#ifndef CBF_VERSION
#define CBF_VERSION "1.0.0"
#endif

namespace cbf {
namespace {

HMODULE g_module = nullptr;
Config g_cfg;
FrameClock* g_clock = nullptr;  // created once, intentionally never freed (hooks may still run)
CpuTuner g_tuner;
Overlay g_overlay;

std::atomic<DWORD> g_renderTid{0};
std::atomic<const char*> g_api{nullptr};
volatile LONG g_initState = 0;
uint64_t g_qpcFreq = 1;

// MMCSS ("Games" task) for the thread that presents. Loaded dynamically; absent on very old Windows.
DWORD g_tlsMmcss = TLS_OUT_OF_INDEXES;
HANDLE(WINAPI* g_avSet)(LPCWSTR, LPDWORD) = nullptr;
BOOL(WINAPI* g_avRevert)(HANDLE) = nullptr;
const HANDLE kMmcssTried = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-1));

uint64_t Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}
uint64_t NowMs() { return Qpc() * 1000ull / g_qpcFreq; }

// Runs on the render thread, once per Present: registers / unregisters the thread with MMCSS to follow the
// state of the CPU fix. Registration and revert happen on the same thread, as the API requires.
void UpdateMmcssForThisThread() {
    if (!g_avSet || !g_avRevert || !g_cfg.cpu.useMmcss || g_tlsMmcss == TLS_OUT_OF_INDEXES) return;
    const bool want = g_tuner.Enabled();
    HANDLE cur = TlsGetValue(g_tlsMmcss);
    if (want && cur == nullptr) {
        DWORD index = 0;
        HANDLE h = g_avSet(L"Games", &index);
        TlsSetValue(g_tlsMmcss, h ? h : kMmcssTried);
    } else if (!want && cur != nullptr) {
        if (cur != kMmcssTried) g_avRevert(cur);
        TlsSetValue(g_tlsMmcss, nullptr);
    }
}

// ---------------------------------------------------------------- overlay text

std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t buf[200];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf(buf, 199, f, ap);
    va_end(ap);
    buf[199] = 0;
    return buf;
}

std::wstring Widen(const char* s) {
    std::wstring w;
    for (; s && *s; ++s) w.push_back(static_cast<unsigned char>(*s));
    return w;
}

const COLORREF kWhite = RGB(232, 232, 232), kGreen = RGB(110, 230, 110), kRed = RGB(240, 110, 110), kYellow = RGB(240, 210, 90), kGrey = RGB(150, 150, 150);

std::vector<OverlayLine> BuildLines(bool statsOn, const std::wstring& toast) {
    std::vector<OverlayLine> lines;
    const OverlayConfig& o = g_cfg.overlay;

    if (statsOn) {
        const FrameStats s = g_clock->Snapshot(Qpc());
        const CpuTuner::Metrics m = g_tuner.GetMetrics();
        if (!s.valid) {
            lines.push_back({L"Waiting for frames...", kGrey});
            const std::string hooked = sources::AttachedNames();
            lines.push_back({hooked.empty() ? L"No graphics API detected yet" : Fmt(L"Hooked: %ls", Widen(hooked.c_str()).c_str()), kGrey});
        } else if (s.msSinceLastFrame > 2000.0) {
            lines.push_back({Fmt(L"No frames for %.1f s", s.msSinceLastFrame / 1000.0), kYellow});
        } else {
            if (o.fps) {
                std::wstring t = Fmt(L"FPS: %.0f", s.fps);
                if (o.low1pct && s.low1pctFps > 0) t += Fmt(L"   1%% low: %.0f", s.low1pctFps);
                lines.push_back({t, s.fps >= 55 ? kGreen : (s.fps >= 30 ? kYellow : kRed)});
            }
            if (o.latency) lines.push_back({Fmt(L"Latency: %.2f ms (max %.1f)", s.frameMsAvg, s.frameMsMax), kWhite});
            if (o.presentWait) lines.push_back({Fmt(L"Present wait: %.2f ms", s.presentMsAvg), kWhite});
            if (o.cpuLoad && m.valid) lines.push_back({Fmt(L"CPU: %.0f%%  busiest thread: %.0f%%", m.processLoad * 100.0, m.hottestThreadLoad * 100.0), kWhite});
            if (o.busiestThread && m.valid && m.hottestThreadLoad >= 0.30 && !m.hottestLabel.empty())
                lines.push_back({Fmt(L"  %ls  kernel %.0f%%", Widen(m.hottestLabel.c_str()).c_str(), m.hottestKernelShare * 100.0), kGrey});
            if (o.bottleneck) {
                switch (ClassifyBottleneck(s, m.valid, m.hottestThreadLoad)) {
                    case Bound::Cpu: lines.push_back({L"Limit: CPU (one thread maxed)", kRed}); break;
                    case Bound::GpuOrVsync: lines.push_back({L"Limit: GPU or VSync", kYellow}); break;
                    case Bound::Neither: lines.push_back({L"Limit: neither (frame cap?)", kGreen}); break;
                    default: break;
                }
            }
            if (const char* api = g_api.load(std::memory_order_relaxed)) lines.push_back({Fmt(L"API: %ls", Widen(api).c_str()), kGrey});
        }
        if (o.cpuFixState) {
            const bool on = g_tuner.Enabled();
            std::wstring t = Fmt(L"CPU fix: %ls", on ? L"ON" : L"OFF");
            if (on && m.valid) t += Fmt(L"  (%d threads tuned)", m.tunedThreads);
            lines.push_back({t, on ? kGreen : kRed});
        }
    }
    if (!toast.empty()) lines.push_back({toast, kYellow});
    return lines;
}

// ---------------------------------------------------------------- game window

struct FindCtx {
    DWORD pid;
    HWND best;
    long long area;
};

BOOL CALLBACK EnumWindowsProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;  // also skips our own overlay
    RECT rc;
    if (!GetClientRect(h, &rc)) return TRUE;
    const long long area = static_cast<long long>(rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > c->area) {
        c->area = area;
        c->best = h;
    }
    return TRUE;
}

HWND FindGameWindow() {
    FindCtx c{GetCurrentProcessId(), nullptr, 100 * 100};  // ignore anything smaller than 100x100
    EnumWindows(&EnumWindowsProc, reinterpret_cast<LPARAM>(&c));
    return c.best;
}

bool OurProcessHasForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Edge-triggered, exact-modifier hotkey check. Only active while the game (our process) has the focus.
bool HotkeyPressed(const Hotkey& hk, bool& wasDown, bool focused) {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool now = focused && hk.bound() && down(hk.vk) && down(VK_CONTROL) == hk.ctrl && down(VK_MENU) == hk.alt && down(VK_SHIFT) == hk.shift;
    const bool fire = now && !wasDown;
    wasDown = now;
    return fire;
}

// ---------------------------------------------------------------- worker thread

DWORD WINAPI WorkerMain(LPVOID) {
    if (!g_overlay.Create(g_module, g_cfg.overlay.opacity, g_cfg.overlay.fontPt)) log::Write("overlay unavailable, continuing without it");
    g_tuner.SetEnabled(g_cfg.cpu.enabledAtStart);

    bool overlayOn = g_cfg.overlay.showAtStart;
    bool keyOverlayDown = false, keyCpuDown = false;
    std::wstring toast;
    uint64_t toastUntil = 0, nextPoll = 0, nextRefresh = 0, nextWindowSearch = 0, nextStatsLog = 5000, nextLimitLog = 0, nextHitchLog = 0, nextDiag = 0;
    uint32_t lastSeq = 0;
    int sustainedLimit = 0;
    HWND game = nullptr;

    for (;;) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const uint64_t now = NowMs();
        const bool focused = OurProcessHasForeground();

        if (HotkeyPressed(g_cfg.keyOverlay, keyOverlayDown, focused)) {
            overlayOn = !overlayOn;
            nextRefresh = 0;
            log::Write("overlay %s", overlayOn ? "shown" : "hidden");
        }
        if (HotkeyPressed(g_cfg.keyCpuFix, keyCpuDown, focused)) {
            g_tuner.SetEnabled(!g_tuner.Enabled());
            toast = g_tuner.Enabled() ? L"CPU fix: ON" : L"CPU fix: OFF";
            toastUntil = now + 2500;
            nextRefresh = 0;
        }

        if (now >= nextPoll) {  // attach hooks for graphics APIs that appeared since the last look
            sources::Poll();
            nextPoll = now + 250;
        }

        g_tuner.Tick(g_renderTid.load(std::memory_order_relaxed), GetCurrentThreadId(), overlayOn || g_cfg.log);

        if (g_cfg.log && now >= nextDiag) {  // diagnostics, written to the log file only, checked 4x per second
            nextDiag = now + 250;
            const FrameStats fs = g_clock->Snapshot(Qpc());
            const CpuTuner::Metrics m = g_tuner.GetMetrics();
            const bool frozen = !fs.valid || fs.msSinceLastFrame > 2000.0;

            if (now >= nextStatsLog) {  // one line every 5 s
                nextStatsLog = now + 5000;
                if (!fs.valid) {
                    log::Write("stats: no frames yet (hooked: %s)", sources::AttachedNames().c_str());
                } else if (frozen) {
                    log::Write("stats: no frames for %.0f s (paused, minimised or loading)", fs.msSinceLastFrame / 1000.0);
                } else {
                    const char* api = g_api.load(std::memory_order_relaxed);
                    log::Write("stats: %.1f fps, frame %.2f ms (max %.1f), 1%% low %.1f fps, present wait %.2f ms, cpu %.0f%%, busiest thread %.0f%%, tuned %d, api %s",
                               fs.fps, fs.frameMsAvg, fs.frameMsMax, fs.low1pctFps, fs.presentMsAvg, m.processLoad * 100.0, m.hottestThreadLoad * 100.0,
                               m.tunedThreads, api ? api : "?");
                }
            }

            // A thread pegged near 100% for 3 samples in a row is the CPU bottleneck: say which one it is.
            if (m.valid && m.sampleSeq != lastSeq) {
                lastSeq = m.sampleSeq;
                sustainedLimit = (!frozen && m.hottestThreadLoad >= 0.90) ? sustainedLimit + 1 : 0;
                if (sustainedLimit >= 3 && now >= nextLimitLog) {
                    nextLimitLog = now + 20000;
                    log::Write("LIMIT: a thread has been at %.0f%% of a core for %d s while the whole process uses %.0f%% of the CPU | %s", m.hottestThreadLoad * 100.0,
                               sustainedLimit, m.processLoad * 100.0, g_tuner.TopThreadsReport(4).c_str());
                }
            }

            // A very long frame: record which threads were busy at that moment (shader compile? streaming? driver?).
            if (!frozen && fs.frameMsMax >= 100.0 && now >= nextHitchLog) {
                nextHitchLog = now + 3000;
                log::Write("HITCH: worst frame %.0f ms in the last second | %s", fs.frameMsMax, g_tuner.TopThreadsReport(4).c_str());
            }
        }

        if (now >= nextRefresh) {
            nextRefresh = now + static_cast<uint64_t>(g_cfg.overlay.refreshMs);
            if (now >= nextWindowSearch || !game || !IsWindow(game)) {
                game = FindGameWindow();
                nextWindowSearch = now + 1000;
            }
            if (!toast.empty() && now >= toastUntil) toast.clear();
            g_overlay.Update(BuildLines(overlayOn, toast), game, g_cfg.overlay.corner);
        }
    }
}

// ---------------------------------------------------------------- initialisation

void Init() {
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(g_module, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return;
    std::wstring base(path, n);
    const size_t dot = base.find_last_of(L'.');
    const size_t slash = base.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) base.resize(dot);

    LoadConfig((base + L".ini").c_str(), g_cfg);
    if (!g_cfg.enabled) return;
    log::Init((base + L".log").c_str(), g_cfg.log);
    log::Write("CpuBottleneckFix " CBF_VERSION " (%s) starting", sizeof(void*) == 8 ? "x64" : "x86");

    LARGE_INTEGER f;
    g_qpcFreq = (QueryPerformanceFrequency(&f) && f.QuadPart > 0) ? static_cast<uint64_t>(f.QuadPart) : 1;
    g_clock = new (std::nothrow) FrameClock(g_qpcFreq);
    if (!g_clock) return;

    // Never let the DLL be unloaded: hooks and the worker thread point into it.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Init), &pinned);

    g_tlsMmcss = TlsAlloc();
    if (HMODULE av = LoadLibraryW(L"avrt.dll")) {
        g_avSet = reinterpret_cast<decltype(g_avSet)>(reinterpret_cast<void*>(GetProcAddress(av, "AvSetMmThreadCharacteristicsW")));
        g_avRevert = reinterpret_cast<decltype(g_avRevert)>(reinterpret_cast<void*>(GetProcAddress(av, "AvRevertMmThreadCharacteristics")));
    }

    g_tuner.Init(g_cfg.cpu);
    if (!sources::Init()) log::Write("frame sources unavailable: FPS display disabled, CPU fix still works");

    HANDLE worker = CreateThread(nullptr, 0, &WorkerMain, nullptr, 0, nullptr);
    if (worker) CloseHandle(worker);
    else log::Write("could not start worker thread");
}

void InitOnce() {
    if (InterlockedCompareExchange(&g_initState, 1, 0) != 0) return;  // already running or done
    Init();
}

DWORD WINAPI InitThread(LPVOID) {
    InitOnce();
    return 0;
}

// Both entry points go through here, so the real work always runs on a fresh thread, after any loader lock
// held by the caller is released, and exactly one thread is ever started.
volatile LONG g_threadStarted = 0;
void StartInitThreadOnce() {
    if (InterlockedCompareExchange(&g_threadStarted, 1, 0) != 0) return;
    if (HANDLE t = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    else InitOnce();  // cannot create a thread: nothing better to do than initialise inline
}

}  // namespace

// ---- called by the sources (declared in sources.h) ----

FrameClock::Ticket OnPresentBegin(uintptr_t key, const char* api) {
    FrameClock::Ticket t;
    if (!g_clock) return t;
    UpdateMmcssForThisThread();
    t = g_clock->Begin(key, Qpc());
    if (t.counted) {
        g_renderTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_api.store(api, std::memory_order_relaxed);
    }
    return t;
}

void OnPresentEnd(const FrameClock::Ticket& ticket) {
    if (g_clock) g_clock->End(ticket, Qpc());
}

}  // namespace cbf

// Called by Ultimate ASI Loader right after it loaded this plugin, outside the loader lock.
extern "C" __declspec(dllexport) void InitializeASI() { cbf::StartInitThreadOnce(); }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        cbf::g_module = module;
        DisableThreadLibraryCalls(module);
        // ASI loaders that only call LoadLibrary never call InitializeASI, so start from here as well; the
        // guard makes whichever of the two arrives second a no-op.
        cbf::StartInitThreadOnce();
    }
    return TRUE;
}
