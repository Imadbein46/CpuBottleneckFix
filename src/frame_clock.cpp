#include "frame_clock.h"

#include <algorithm>
#include <functional>
#include <vector>

namespace cbf {
namespace {

constexpr double kShortWindowUs = 1000000.0;   // FPS / average window
constexpr double kLongWindowUs = 5000000.0;    // 1% low window
constexpr uint64_t kMaxGapSeconds = 5;         // a longer gap is a pause (alt-tab, loading), not a frame

uint32_t ClampU32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v); }

}  // namespace

FrameClock::FrameClock(uint64_t ticksPerSecond) : ticksPerSecond_(ticksPerSecond ? ticksPerSecond : 1) {
    for (uint32_t i = 0; i < kRing; ++i) {
        intervalUs_[i].store(0, std::memory_order_relaxed);
        presentUs_[i].store(0, std::memory_order_relaxed);
    }
}

FrameClock::Ticket FrameClock::Begin(uintptr_t key, uint64_t now) {
    Ticket t;
    t.begin = now;
    begins_.fetch_add(1, std::memory_order_relaxed);

    const uintptr_t primary = primary_.load(std::memory_order_relaxed);
    uint64_t last = lastBegin_.load(std::memory_order_relaxed);

    if (primary != key) {
        const bool primaryAlive = primary != 0 && last != 0 && now >= last && (now - last) < ticksPerSecond_;
        if (primaryAlive) {  // a secondary surface: ignore it
            ignored_.fetch_add(1, std::memory_order_relaxed);
            return t;
        }
        switches_.fetch_add(1, std::memory_order_relaxed);
        primary_.store(key, std::memory_order_relaxed);
        last = 0;  // new surface: start a fresh interval
    }

    t.counted = true;
    if (last != 0) {
        if (now <= last) {
            notForward_.fetch_add(1, std::memory_order_relaxed);
        } else if ((now - last) > ticksPerSecond_ * kMaxGapSeconds) {
            longGaps_.fetch_add(1, std::memory_order_relaxed);
        } else {
            const uint64_t us = (now - last) * 1000000ull / ticksPerSecond_;
            t.intervalUs = us == 0 ? 1u : ClampU32(us);
        }
    }
    lastBegin_.store(now, std::memory_order_relaxed);
    return t;
}

void FrameClock::End(const Ticket& t, uint64_t now) {
    if (!t.counted || t.intervalUs == 0) return;
    const uint64_t presentUs = now > t.begin ? (now - t.begin) * 1000000ull / ticksPerSecond_ : 0;
    const uint64_t slot = head_.fetch_add(1, std::memory_order_acq_rel);
    const uint32_t i = static_cast<uint32_t>(slot & (kRing - 1));
    intervalUs_[i].store(t.intervalUs, std::memory_order_relaxed);
    presentUs_[i].store(ClampU32(presentUs), std::memory_order_relaxed);
}

FrameCounters FrameClock::Counters() const {
    FrameCounters c;
    c.begins = begins_.load(std::memory_order_relaxed);
    c.ignoredSecondary = ignored_.load(std::memory_order_relaxed);
    c.primarySwitches = switches_.load(std::memory_order_relaxed);
    c.longGaps = longGaps_.load(std::memory_order_relaxed);
    c.clockNotForward = notForward_.load(std::memory_order_relaxed);
    c.entries = head_.load(std::memory_order_relaxed);
    return c;
}

FrameStats FrameClock::Snapshot(uint64_t now) const {
    FrameStats s;
    const uint64_t total = head_.load(std::memory_order_acquire);
    s.totalFrames = total;
    if (total == 0) return s;

    const uint64_t last = lastBegin_.load(std::memory_order_relaxed);
    s.msSinceLastFrame = (now > last) ? static_cast<double>(now - last) * 1000.0 / static_cast<double>(ticksPerSecond_) : 0.0;

    const uint64_t maxEntries = std::min<uint64_t>(total, kRing - 64);  // keep clear of the slot being written
    std::vector<uint32_t> longWindow;
    longWindow.reserve(1024);

    double accLong = 0, accShort = 0, sumPresent = 0, maxInterval = 0;
    uint32_t countShort = 0;

    for (uint64_t n = 0; n < maxEntries && accLong < kLongWindowUs; ++n) {
        const uint32_t i = static_cast<uint32_t>((total - 1 - n) & (kRing - 1));
        const uint32_t iv = intervalUs_[i].load(std::memory_order_relaxed);
        if (iv == 0) continue;  // slot reserved but not written yet
        const uint32_t pr = presentUs_[i].load(std::memory_order_relaxed);

        accLong += iv;
        longWindow.push_back(iv);

        if (accShort < kShortWindowUs) {
            accShort += iv;
            sumPresent += pr;
            maxInterval = std::max<double>(maxInterval, iv);
            ++countShort;
        }
    }
    if (countShort == 0 || accShort <= 0) return s;

    s.valid = true;
    s.fps = static_cast<double>(countShort) * 1000000.0 / accShort;
    s.frameMsAvg = accShort / countShort / 1000.0;
    s.frameMsMax = maxInterval / 1000.0;
    s.presentMsAvg = sumPresent / countShort / 1000.0;

    if (longWindow.size() >= 100) {
        // 99th percentile frame time = the value 1% of the way into the slowest-first ordering
        const size_t k = longWindow.size() / 100;
        std::nth_element(longWindow.begin(), longWindow.begin() + k, longWindow.end(), std::greater<uint32_t>());
        const double p99 = longWindow[k];
        if (p99 > 0) s.low1pctFps = 1000000.0 / p99;
    }
    return s;
}

}  // namespace cbf
