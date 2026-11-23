// FrameClock: turns "a Present call started / ended" events into FPS, frame time, 1% low and Present-wait
// statistics. Portable (no Windows dependency) so it can be unit-tested natively. Time is passed in by the
// caller (ticks of any monotonic clock), which keeps the module deterministic.
//
// Threading: Begin/End are called from the render thread, Snapshot from another thread. All shared state is
// lock-free; a reader can at worst see one frame that is half-written, which is harmless for statistics.
#pragma once
#include <atomic>
#include <cstdint>

namespace cbf {

struct FrameStats {
    bool valid = false;            // at least one measured frame exists
    uint64_t totalFrames = 0;      // measured frames since start
    double fps = 0;                // average over the last ~1 s
    double frameMsAvg = 0;         // average time between two Present calls (last ~1 s)
    double frameMsMax = 0;         // worst frame time in the last ~1 s
    double low1pctFps = 0;         // 1000 / 99th-percentile frame time over the last ~5 s; 0 if < 100 frames
    double presentMsAvg = 0;       // average time spent *inside* Present (waiting for GPU / vsync / queue)
    double msSinceLastFrame = 0;   // how long ago the last Present started
};

// Raw event counters, to find out WHY no frames are being measured (e.g. Present is called but every call is
// ignored, or the clock is not moving forward).
struct FrameCounters {
    uint64_t begins = 0;            // Present calls offered to the clock
    uint64_t ignoredSecondary = 0;  // calls ignored because another surface is the measured one
    uint64_t primarySwitches = 0;   // times the measured surface changed
    uint64_t longGaps = 0;          // gaps over 5 s between two calls of the measured surface
    uint64_t clockNotForward = 0;   // calls whose timestamp was not later than the previous one
    uint64_t entries = 0;           // frames recorded
};

class FrameClock {
public:
    struct Ticket {
        bool counted = false;      // this call belongs to the surface we measure
        uint64_t begin = 0;        // tick when Present started
        uint32_t intervalUs = 0;   // time since the previous counted Present; 0 = none (first frame / after a pause)
    };

    explicit FrameClock(uint64_t ticksPerSecond);

    // Call right before the real Present. `key` identifies the presenting surface (swap chain, device, HDC,
    // queue...). Only one surface is measured at a time (the first one seen; another one takes over if the
    // current one stays silent for 1 s), so games with several windows are not counted twice.
    Ticket Begin(uintptr_t key, uint64_t nowTicks);

    // Call right after the real Present returned.
    void End(const Ticket& ticket, uint64_t nowTicks);

    FrameStats Snapshot(uint64_t nowTicks) const;
    FrameCounters Counters() const;

    static constexpr uint32_t kRing = 8192;  // power of two

private:
    uint64_t ticksPerSecond_;
    std::atomic<uintptr_t> primary_{0};
    std::atomic<uint64_t> lastBegin_{0};
    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> begins_{0}, ignored_{0}, switches_{0}, longGaps_{0}, notForward_{0};
    std::atomic<uint32_t> intervalUs_[kRing];
    std::atomic<uint32_t> presentUs_[kRing];
};

}  // namespace cbf
