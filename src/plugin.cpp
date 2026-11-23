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
#include <cstring>
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
#include "sysinfo.h"

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
std::atomic<uint64_t> g_lastCountedQpc{0};  // when a measured Present last started
std::atomic<const char*> g_api{nullptr};
std::atomic<bool> g_syncOn{false};          // frame-latency control is switched on
std::atomic<bool> g_frameGenLoaded{false};  // a frame-generation module is loaded: shown FPS may include generated frames
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
            if (o.cpuLoad && m.valid && (m.allowedCpus < m.systemCpus || m.restrictedThreads > 0 || m.widenedThreads > 0)) {
                std::wstring t = Fmt(L"Cores allowed: %d/%d", m.allowedCpus, m.systemCpus);
                if (m.restrictedThreads > 0) t += Fmt(L", %d thread(s) pinned to fewer", m.restrictedThreads);
                if (m.widenedThreads > 0) t += Fmt(L", %d widened", m.widenedThreads);
                lines.push_back({t, m.allowedCpus < m.systemCpus ? kYellow : kGrey});
            }
            if (o.bottleneck) {
                switch (ClassifyBottleneck(s, m.valid, m.hottestThreadLoad)) {
                    case Bound::Cpu: lines.push_back({L"Limit: CPU (one thread maxed)", kRed}); break;
                    case Bound::GpuOrVsync: lines.push_back({L"Limit: GPU or VSync", kYellow}); break;
                    case Bound::Neither: lines.push_back({L"Limit: neither (frame cap?)", kGreen}); break;
                    default: break;
                }
            }
            if (const char* api = g_api.load(std::memory_order_relaxed)) lines.push_back({Fmt(L"API: %ls", Widen(api).c_str()), kGrey});
            if (o.syncInfo) {
                const sources::SyncInfo si = sources::GetSyncInfo();
                if (si.known) {
                    const std::wstring vsync = si.presentInterval == 0 ? ((si.presentFlags & 0x200u) ? std::wstring(L"vsync off, tearing allowed") : std::wstring(L"vsync off"))
                                                                       : Fmt(L"vsync %d", si.presentInterval);
                    const std::wstring queue = si.frameLatency > 0 ? Fmt(L", queue %d", si.frameLatency) : std::wstring();
                    lines.push_back({Fmt(L"Sync: %ls, %ls, %lu buffers%ls", vsync.c_str(), si.swapEffect >= 3 ? L"flip" : L"blit", static_cast<unsigned long>(si.bufferCount), queue.c_str()), kGrey});
                }
                const bool syncOn = g_syncOn.load(std::memory_order_relaxed);
                lines.push_back({syncOn ? Fmt(L"Frame latency control: ON (max %d)", g_cfg.sync.maxFrameLatency) : std::wstring(L"Frame latency control: OFF"), syncOn ? kGreen : kGrey});
            }
            if (g_frameGenLoaded.load(std::memory_order_relaxed)) lines.push_back({L"Frame generation loaded: FPS may include generated frames", kGrey});
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
    uint64_t toastUntil = 0, nextPoll = 0, nextRefresh = 0, nextWindowSearch = 0, nextStatsLog = 0, nextLimitLog = 0, nextHitchLog = 0, nextDiag = 0;
    uint32_t lastSeq = 0;
    bool modulesLogged = false;
    int sustainedLimit = 0;
    DWORD seenRender = 0, stableRender = 0;
    uint64_t renderSince = 0, nextFgCheck = 0;
    HWND game = nullptr;
    const uint64_t startMs = NowMs();  // NowMs() counts from system boot, so "20 s after start" must be relative to this
    bool syncOn = g_cfg.sync.enabledAtStart;
    bool keySyncDown = false;
    sources::SetFrameLatency(syncOn ? g_cfg.sync.maxFrameLatency : 0);
    g_syncOn.store(syncOn, std::memory_order_relaxed);
    sources::SyncInfo lastSyncLogged;

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

        if (HotkeyPressed(g_cfg.keySync, keySyncDown, focused)) {
            syncOn = !syncOn;
            sources::SetFrameLatency(syncOn ? g_cfg.sync.maxFrameLatency : 0);
            g_syncOn.store(syncOn, std::memory_order_relaxed);
            toast = syncOn ? Fmt(L"Frame latency control: ON (max %d queued)", g_cfg.sync.maxFrameLatency) : L"Frame latency control: OFF";
            toastUntil = now + 2500;
            nextRefresh = 0;
            log::Write("frame latency control %s (max %d queued frames)", syncOn ? "ON" : "OFF", g_cfg.sync.maxFrameLatency);
        }

        if (now >= nextPoll) {  // attach hooks for graphics APIs that appeared since the last look
            sources::Poll();
            nextPoll = now + 250;
        }

        // Some engines (job systems such as Cyberpunk's redDispatcher, or frame generation) present from a different
        // thread every frame. Only a presenter that stayed the same for 5 s counts as "the render thread".
        // a presenter that stopped presenting (e.g. a loading screen thread) is not "the render thread" any more
        const uint64_t lastCounted = g_lastCountedQpc.load(std::memory_order_relaxed);
        const bool presenting = lastCounted != 0 && Qpc() - lastCounted < 2 * g_qpcFreq;
        const DWORD curRender = presenting ? g_renderTid.load(std::memory_order_relaxed) : 0;
        if (curRender != seenRender) {
            seenRender = curRender;
            renderSince = now;
            stableRender = 0;
        } else if (curRender != 0 && now - renderSince >= 5000) {
            stableRender = curRender;
        }
        if (now >= nextFgCheck) {
            nextFgCheck = now + 2000;
            g_frameGenLoaded.store(GetModuleHandleW(L"nvngx_dlssg.dll") || GetModuleHandleW(L"sl.dlss_g.dll") || GetModuleHandleW(L"amd_fidelityfx_framegeneration_dx12.dll"),
                                   std::memory_order_relaxed);
        }

        g_tuner.Tick(stableRender, GetCurrentThreadId(), overlayOn || g_cfg.log);

        if (g_cfg.log && now >= nextDiag) {  // diagnostics, written to the log file only, checked 4x per second
            nextDiag = now + 250;
            const FrameStats fs = g_clock->Snapshot(Qpc());
            const CpuTuner::Metrics m = g_tuner.GetMetrics();
            const bool frozen = !fs.valid || fs.msSinceLastFrame > 2000.0;

            if (now >= nextStatsLog) {  // one line every 5 s
                nextStatsLog = now + 5000;
                if (!fs.valid && now - startMs >= 20000 && !modulesLogged) {
                    modulesLogged = true;
                    LogLoadedModules();
                }
                const FrameCounters k = g_clock->Counters();
                char why[420];
                std::snprintf(why, sizeof(why), "present calls seen by hooks: %lu (outermost %lu); clock: offered %lu, ignored-other-surface %lu, surface switches %lu, gaps>5s %lu, clock-not-forward %lu, frames %lu",
                              static_cast<unsigned long>(sources::g_rawPresentCalls.load()), static_cast<unsigned long>(sources::g_outerPresentCalls.load()),
                              static_cast<unsigned long>(k.begins), static_cast<unsigned long>(k.ignoredSecondary), static_cast<unsigned long>(k.primarySwitches),
                              static_cast<unsigned long>(k.longGaps), static_cast<unsigned long>(k.clockNotForward), static_cast<unsigned long>(k.entries));
                if (m.valid) {  // is the game busy, or idle? (tells "not presenting" apart from "presenting but unseen")
                    const size_t len = std::strlen(why);
                    std::snprintf(why + len, sizeof(why) - len, "; process cpu %.0f%%, busiest thread %.0f%% %s", m.processLoad * 100.0, m.hottestThreadLoad * 100.0, m.hottestLabel.c_str());
                }
                if (!fs.valid) {
                    log::Write("stats: no frames yet (hooked: %s) | %s", sources::AttachedNames().c_str(), why);
                } else if (frozen) {
                    log::Write("stats: no frames for %.0f s (paused, minimised or loading) | %s", fs.msSinceLastFrame / 1000.0, why);
                } else {
                    const char* api = g_api.load(std::memory_order_relaxed);
                    log::Write("stats: %.1f fps, frame %.2f ms (max %.1f), 1%% low %.1f fps, present wait %.2f ms, cpu %.0f%%, busiest thread %.0f%%, tuned %d, api %s",
                               fs.fps, fs.frameMsAvg, fs.frameMsMax, fs.low1pctFps, fs.presentMsAvg, m.processLoad * 100.0, m.hottestThreadLoad * 100.0,
                               m.tunedThreads, api ? api : "?");
                }
            }

            {
                const sources::SyncInfo si = sources::GetSyncInfo();
                if (si.known && (si.presentInterval != lastSyncLogged.presentInterval || si.bufferCount != lastSyncLogged.bufferCount || si.swapEffect != lastSyncLogged.swapEffect ||
                                 si.frameLatency != lastSyncLogged.frameLatency || !lastSyncLogged.known)) {
                    lastSyncLogged = si;
                    log::Write("sync: present interval %d (0 = no vsync), flags 0x%lX, %lu buffers, swap effect %d (3-4 = flip), swap chain flags 0x%lX, frame latency %d (-1 = n/a)",
                               si.presentInterval, static_cast<unsigned long>(si.presentFlags), static_cast<unsigned long>(si.bufferCount), si.swapEffect,
                               static_cast<unsigned long>(si.swapChainFlags), si.frameLatency);
                }
            }

            // A thread pegged near 100% for 3 samples in a row is the CPU bottleneck: say which one it is.
            if (m.valid && m.sampleSeq != lastSeq) {
                lastSeq = m.sampleSeq;
                sustainedLimit = (!frozen && m.hottestThreadLoad >= 0.90) ? sustainedLimit + 1 : 0;
                if (sustainedLimit >= 3 && now >= nextLimitLog) {
                    nextLimitLog = now + 20000;
                    log::Write("LIMIT: a thread has been at %.0f%% of a core for about %d s while the whole process uses %.0f%% of the CPU | %s", m.hottestThreadLoad * 100.0,
                               sustainedLimit * g_cfg.cpu.sampleMs / 1000, m.processLoad * 100.0, g_tuner.TopThreadsReport(4).c_str());
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

    const std::wstring ini = base + L".ini";
    // The log must exist before the config is parsed, otherwise warnings about bad ini values are lost.
    log::Init((base + L".log").c_str(), GetPrivateProfileIntW(L"General", L"Log", 0, ini.c_str()) != 0);
    log::Write("CpuBottleneckFix " CBF_VERSION " (%s) starting", sizeof(void*) == 8 ? "x64" : "x86");
    LoadConfig(ini.c_str(), g_cfg);
    if (!g_cfg.enabled) {
        log::Write("disabled by Enabled=0 in the ini, doing nothing");
        return;
    }
    if (g_cfg.log) LogSystemInfo();

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

namespace {
uint64_t ticketBegin(const FrameClock::Ticket& t) { return t.begin; }
}  // namespace

FrameClock::Ticket OnPresentBegin(uintptr_t key, const char* api) {
    FrameClock::Ticket t;
    if (!g_clock) return t;
    UpdateMmcssForThisThread();
    t = g_clock->Begin(key, Qpc());
    if (t.counted) {
        g_renderTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_lastCountedQpc.store(ticketBegin(t), std::memory_order_relaxed);
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
