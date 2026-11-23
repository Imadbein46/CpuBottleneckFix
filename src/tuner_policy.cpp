#include "tuner_policy.h"

#include <algorithm>

namespace cbf {

std::vector<Target> ChooseTargets(const std::vector<ThreadView>& threads, uint32_t renderTid, uint32_t mainTid, uint32_t selfTid, int workerThreads) {
    std::vector<Target> out;
    auto known = [&threads](uint32_t tid) {
        return std::any_of(threads.begin(), threads.end(), [tid](const ThreadView& t) { return t.tid == tid; });
    };
    auto chosen = [&out](uint32_t tid) {
        return std::any_of(out.begin(), out.end(), [tid](const Target& t) { return t.tid == tid; });
    };
    auto addCritical = [&](uint32_t tid) {
        if (tid != 0 && tid != selfTid && known(tid) && !chosen(tid)) out.push_back({tid, true});
    };
    addCritical(renderTid);
    addCritical(mainTid);

    if (workerThreads > 0) {
        std::vector<ThreadView> ranked;
        for (const ThreadView& t : threads) {
            if (t.tid == selfTid || chosen(t.tid)) continue;
            if (t.load >= (t.tuned ? kWorkerStayLoad : kWorkerEnterLoad)) ranked.push_back(t);
        }
        std::sort(ranked.begin(), ranked.end(), [](const ThreadView& a, const ThreadView& b) {
            if (a.load != b.load) return a.load > b.load;
            return a.tid < b.tid;
        });
        for (const ThreadView& t : ranked) {
            if (workerThreads <= 0) break;
            out.push_back({t.tid, false});
            --workerThreads;
        }
    }
    return out;
}

bool ShouldWidenAffinity(uint64_t threadMask, uint64_t targetMask) {
    if (threadMask == 0 || targetMask == 0 || threadMask == targetMask) return false;
    return (threadMask & ~targetMask) == 0;  // proper subset of the target
}

int CountCpus(uint64_t mask) {
    int n = 0;
    for (; mask; mask &= (mask - 1)) ++n;
    return n;
}

}  // namespace cbf
