// Plugin settings, read once from CpuBottleneckFix.ini (next to the .asi).
#pragma once
#include "hotkey.h"

namespace cbf {

enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };
enum class ProcessPriority { Unchanged, Normal, AboveNormal, High };
enum class ThreadPriority { Unchanged, AboveNormal, Highest };

struct OverlayConfig {
    bool showAtStart = true;
    Corner corner = Corner::TopLeft;
    int refreshMs = 500;   // 100..5000
    int opacity = 215;     // 40..255
    int fontPt = 11;       // 8..32
    bool fps = true;
    bool latency = true;
    bool low1pct = true;
    bool presentWait = true;
    bool cpuLoad = true;
    bool busiestThread = true;
    bool bottleneck = true;
    bool cpuFixState = true;
};

struct CpuFixConfig {
    bool enabledAtStart = true;
    bool timerResolution = true;
    int timerResolutionUs = 500;  // 500..15600 microseconds
    bool disablePowerThrottling = true;
    ProcessPriority processPriority = ProcessPriority::AboveNormal;
    bool threadTuning = true;
    int workerThreads = 0;        // 0..6 extra busy background threads to give their own core (never a priority boost)
    ThreadPriority threadPriority = ThreadPriority::AboveNormal;
    bool preferPerformanceCores = true;
    bool avoidCpu0 = true;
    bool useMmcss = true;
    int sampleMs = 1000;          // 250..10000
};

struct Config {
    bool enabled = true;
    bool log = false;
    Hotkey keyOverlay;
    Hotkey keyCpuFix;
    OverlayConfig overlay;
    CpuFixConfig cpu;
};

// Fills `cfg` with defaults, then overrides from the ini file if it exists. Unreadable or invalid values keep
// their defaults. Never fails.
void LoadConfig(const wchar_t* iniPath, Config& cfg);

}  // namespace cbf
