#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "log.h"

namespace cbf {
namespace {

std::string ReadString(const wchar_t* ini, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[128];
    GetPrivateProfileStringW(section, key, def, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), ini);
    std::string out;
    for (const wchar_t* p = buf; *p; ++p) out.push_back(*p < 128 ? static_cast<char>(*p) : '?');
    return out;
}

bool ReadBool(const wchar_t* ini, const wchar_t* section, const wchar_t* key, bool def) {
    return GetPrivateProfileIntW(section, key, def ? 1 : 0, ini) != 0;
}

int ReadInt(const wchar_t* ini, const wchar_t* section, const wchar_t* key, int def, int lo, int hi) {
    int v = static_cast<int>(GetPrivateProfileIntW(section, key, static_cast<UINT>(def), ini));
    return std::min(hi, std::max(lo, v));
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string t;
    for (char c : s) {
        if (c == ';') break;
        if (!std::isspace(static_cast<unsigned char>(c))) t.push_back(c);
    }
    return t;
}

void ReadHotkey(const wchar_t* ini, const wchar_t* key, const char* def, Hotkey& out, const char* name) {
    const std::string text = ReadString(ini, L"Hotkeys", key, L"");
    Hotkey parsed;
    if (text.empty() || !ParseHotkey(text, parsed)) {
        if (!text.empty()) log::Write("ini: cannot parse hotkey %s=\"%s\", using %s", name, text.c_str(), def);
        ParseHotkey(def, out);
        return;
    }
    out = parsed;
}

}  // namespace

void LoadConfig(const wchar_t* ini, Config& cfg) {
    cfg = Config{};
    ParseHotkey("CTRL+F9", cfg.keyOverlay);
    ParseHotkey("CTRL+F10", cfg.keyCpuFix);
    ParseHotkey("CTRL+F11", cfg.keySync);

    cfg.enabled = ReadBool(ini, L"General", L"Enabled", cfg.enabled);
    cfg.log = ReadBool(ini, L"General", L"Log", cfg.log);

    ReadHotkey(ini, L"ToggleOverlay", "CTRL+F9", cfg.keyOverlay, "ToggleOverlay");
    ReadHotkey(ini, L"ToggleCpuFix", "CTRL+F10", cfg.keyCpuFix, "ToggleCpuFix");
    ReadHotkey(ini, L"ToggleSync", "CTRL+F11", cfg.keySync, "ToggleSync");

    OverlayConfig& o = cfg.overlay;
    o.showAtStart = ReadBool(ini, L"Overlay", L"Show", o.showAtStart);
    {
        const std::string c = Lower(ReadString(ini, L"Overlay", L"Corner", L"TopLeft"));
        if (c == "topleft") o.corner = Corner::TopLeft;
        else if (c == "topright") o.corner = Corner::TopRight;
        else if (c == "bottomleft") o.corner = Corner::BottomLeft;
        else if (c == "bottomright") o.corner = Corner::BottomRight;
    }
    o.refreshMs = ReadInt(ini, L"Overlay", L"RefreshMs", o.refreshMs, 100, 5000);
    o.opacity = ReadInt(ini, L"Overlay", L"Opacity", o.opacity, 40, 255);
    o.fontPt = ReadInt(ini, L"Overlay", L"FontSize", o.fontPt, 8, 32);
    o.fps = ReadBool(ini, L"Overlay", L"ShowFps", o.fps);
    o.latency = ReadBool(ini, L"Overlay", L"ShowLatency", o.latency);
    o.low1pct = ReadBool(ini, L"Overlay", L"ShowOnePercentLow", o.low1pct);
    o.presentWait = ReadBool(ini, L"Overlay", L"ShowPresentWait", o.presentWait);
    o.cpuLoad = ReadBool(ini, L"Overlay", L"ShowCpuLoad", o.cpuLoad);
    o.busiestThread = ReadBool(ini, L"Overlay", L"ShowBusiestThread", o.busiestThread);
    o.bottleneck = ReadBool(ini, L"Overlay", L"ShowBottleneck", o.bottleneck);
    o.cpuFixState = ReadBool(ini, L"Overlay", L"ShowCpuFixState", o.cpuFixState);
    o.syncInfo = ReadBool(ini, L"Overlay", L"ShowSyncInfo", o.syncInfo);

    cfg.sync.enabledAtStart = ReadBool(ini, L"Sync", L"Enabled", cfg.sync.enabledAtStart);
    cfg.sync.maxFrameLatency = ReadInt(ini, L"Sync", L"MaxFrameLatency", cfg.sync.maxFrameLatency, 1, 8);

    CpuFixConfig& c = cfg.cpu;
    c.enabledAtStart = ReadBool(ini, L"CpuFix", L"Enabled", c.enabledAtStart);
    c.timerResolution = ReadBool(ini, L"CpuFix", L"TimerResolution", c.timerResolution);
    c.timerResolutionUs = ReadInt(ini, L"CpuFix", L"TimerResolutionUs", c.timerResolutionUs, 500, 15600);
    c.disablePowerThrottling = ReadBool(ini, L"CpuFix", L"DisablePowerThrottling", c.disablePowerThrottling);
    {
        const std::string p = Lower(ReadString(ini, L"CpuFix", L"ProcessPriority", L"AboveNormal"));
        if (p == "unchanged") c.processPriority = ProcessPriority::Unchanged;
        else if (p == "normal") c.processPriority = ProcessPriority::Normal;
        else if (p == "abovenormal") c.processPriority = ProcessPriority::AboveNormal;
        else if (p == "high") c.processPriority = ProcessPriority::High;
    }
    c.threadTuning = ReadBool(ini, L"CpuFix", L"ThreadTuning", c.threadTuning);
    c.workerThreads = ReadInt(ini, L"CpuFix", L"TuneWorkerThreads", c.workerThreads, 0, 6);
    {
        const std::string p = Lower(ReadString(ini, L"CpuFix", L"HotThreadPriority", L"AboveNormal"));
        if (p == "unchanged") c.threadPriority = ThreadPriority::Unchanged;
        else if (p == "abovenormal") c.threadPriority = ThreadPriority::AboveNormal;
        else if (p == "highest") c.threadPriority = ThreadPriority::Highest;
    }
    c.preferPerformanceCores = ReadBool(ini, L"CpuFix", L"PreferPerformanceCores", c.preferPerformanceCores);
    c.avoidCpu0 = ReadBool(ini, L"CpuFix", L"AvoidCpu0", c.avoidCpu0);
    c.useMmcss = ReadBool(ini, L"CpuFix", L"UseMmcss", c.useMmcss);
    c.useAllCpus = ReadBool(ini, L"CpuFix", L"UseAllCpus", c.useAllCpus);
    c.widenThreadAffinity = ReadBool(ini, L"CpuFix", L"WidenThreadAffinity", c.widenThreadAffinity);
    c.sampleMs = ReadInt(ini, L"CpuFix", L"SampleMs", c.sampleMs, 250, 10000);
}

}  // namespace cbf
