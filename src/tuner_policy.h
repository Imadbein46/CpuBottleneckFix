// Which threads should the CPU fix touch? Pure decision logic (no Windows calls) so it can be unit-tested.
#pragma once
#include <cstdint>
#include <vector>

namespace cbf {

struct ThreadView {
    uint32_t tid;
    double load;   // 0..1 of one core over the last sample
    bool tuned;    // currently tuned by us
};

struct Target {
    uint32_t tid;
    bool critical;  // critical = may get a higher priority and a core hint; otherwise a core hint only
};

constexpr double kWorkerEnterLoad = 0.60;  // an extra worker thread must use >= 60% of a core to be picked...
constexpr double kWorkerStayLoad = 0.30;   // ...and keeps its place while it stays above 30% (hysteresis)

// Critical threads first: the one that presents frames (renderTid, 0 = unknown) and the main thread. Then up to
// `workerThreads` of the busiest remaining threads. The calling thread (selfTid) is never chosen and ids that are
// not in `threads` are ignored. Ties are broken by thread id so the result is deterministic.
std::vector<Target> ChooseTargets(const std::vector<ThreadView>& threads, uint32_t renderTid, uint32_t mainTid, uint32_t selfTid, int workerThreads);

// Should a thread whose affinity mask is `threadMask` be widened to `targetMask` (the process's mask)?
// Only when it is narrower: a non-empty proper subset. Equal, empty, or masks with CPUs outside the target are left alone.
bool ShouldWidenAffinity(uint64_t threadMask, uint64_t targetMask);
int CountCpus(uint64_t mask);

}  // namespace cbf
