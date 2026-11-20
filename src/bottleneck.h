// Heuristic "what is limiting my frame rate" classifier. Pure function, unit-tested.
#pragma once
#include "frame_clock.h"

namespace cbf {

enum class Bound { Unknown, GpuOrVsync, Cpu, Neither };

// presentShare = how much of each frame is spent blocked inside Present. A GPU-bound or vsync-bound game
// sits in Present waiting; a CPU-bound game never waits there, and its busiest thread is close to 100% of a core.
inline Bound ClassifyBottleneck(const FrameStats& s, bool threadLoadKnown, double hottestThreadLoad) {
    if (!s.valid || s.frameMsAvg <= 0.0) return Bound::Unknown;
    const double presentShare = s.presentMsAvg / s.frameMsAvg;
    if (presentShare >= 0.6) return Bound::GpuOrVsync;
    if (threadLoadKnown && hottestThreadLoad >= 0.85) return Bound::Cpu;
    return Bound::Neither;
}

}  // namespace cbf
