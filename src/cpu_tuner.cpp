#include "cpu_tuner.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>

#include "log.h"

namespace cbf {
namespace {

template <class T>
T Resolve(HMODULE m, const char* name) {
    return m ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

uint64_t Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

uint64_t FileTime100ns(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

int ProcessRank(DWORD cls) {
    switch (cls) {
        case IDLE_PRIORITY_CLASS: return 0;
        case BELOW_NORMAL_PRIORITY_CLASS: return 1;
        case NORMAL_PRIORITY_CLASS: return 2;
        case ABOVE_NORMAL_PRIORITY_CLASS: return 3;
        case HIGH_PRIORITY_CLASS: return 4;
        case REALTIME_PRIORITY_CLASS: return 5;
        default: return 2;
    }
}

// PROCESS_POWER_THROTTLING_STATE, declared here so older SDK headers do not matter.
struct PowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};
constexpr int kProcessPowerThrottling = 4;  // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
constexpr ULONG kThrottleCurrentVersion = 1;
constexpr ULONG kThrottleExecutionSpeed = 0x1;
constexpr ULONG kThrottleIgnoreTimerResolution = 0x4;

// Some mingw-w64 headers declare PROCESSOR_RELATIONSHIP as {Flags, Reserved[21], GroupCount, ...}; the Windows SDK
// calls the byte after Flags "EfficiencyClass". Both layouts put GroupCount at offset 22.
static_assert(offsetof(PROCESSOR_RELATIONSHIP, GroupCount) == 22, "unexpected PROCESSOR_RELATIONSHIP layout");

constexpr double kWorkerEnterLoad = 0.60;  // an extra worker thread must use >= 60% of a core to be picked...
constexpr double kWorkerStayLoad = 0.30;   // ...and keeps its place while it stays above 30%
constexpr int kMissedSamplesBeforeRestore = 3;
constexpr uint64_t kIdleTickMs = 5000;        // how often the critical threads are re-checked when nothing is displayed
constexpr uint64_t kIdleFullScanMs = 60000;   // and how often all threads are scanned (also frees handles of dead threads)

}  // namespace

CpuTuner::CpuTuner() = default;

CpuTuner::~CpuTuner() {
    for (auto& kv : threads_) {
        if (kv.second.handle) CloseHandle(kv.second.handle);
    }
    threads_.clear();
}

void CpuTuner::Init(const CpuFixConfig& cfg) {
    if (initialised_) return;
    cfg_ = cfg;

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    api_.NtSetTimerResolution = Resolve<decltype(api_.NtSetTimerResolution)>(nt, "NtSetTimerResolution");
    api_.NtQueryTimerResolution = Resolve<decltype(api_.NtQueryTimerResolution)>(nt, "NtQueryTimerResolution");
    api_.SetProcessInformation = Resolve<decltype(api_.SetProcessInformation)>(k32, "SetProcessInformation");
    api_.GetLogicalProcessorInformationEx = Resolve<decltype(api_.GetLogicalProcessorInformationEx)>(k32, "GetLogicalProcessorInformationEx");
    api_.SetThreadIdealProcessorEx = Resolve<decltype(api_.SetThreadIdealProcessorEx)>(k32, "SetThreadIdealProcessorEx");
    api_.GetThreadIdealProcessorEx = Resolve<decltype(api_.GetThreadIdealProcessorEx)>(k32, "GetThreadIdealProcessorEx");
    api_.GetProcessGroupAffinity = Resolve<decltype(api_.GetProcessGroupAffinity)>(k32, "GetProcessGroupAffinity");
    api_.NtQueryInformationThread = Resolve<decltype(api_.NtQueryInformationThread)>(nt, "NtQueryInformationThread");
    api_.GetThreadDescription = Resolve<decltype(api_.GetThreadDescription)>(k32, "GetThreadDescription");

    LARGE_INTEGER f;
    qpcFreq_ = (QueryPerformanceFrequency(&f) && f.QuadPart > 0) ? static_cast<uint64_t>(f.QuadPart) : 1;

    // denominator for the process-wide CPU load: logical CPUs this process may run on
    unsigned count = 0;
    DWORD_PTR procMask = 0, sysMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) {
        for (DWORD_PTR b = procMask; b; b &= (b - 1)) ++count;
    }
    if (count == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        count = si.dwNumberOfProcessors;
    }
    cpuCountForLoad_ = std::max(1u, count);

    BuildTopology();
    initialised_ = true;
    log::Write("CpuTuner ready: %u CPUs for this process, %u usable physical cores for placement", cpuCountForLoad_,
               static_cast<unsigned>(cores_.size()));
}

void CpuTuner::BuildTopology() {
    cores_.clear();
    slotUsed_.clear();
    if (!api_.GetLogicalProcessorInformationEx || !api_.SetThreadIdealProcessorEx || !api_.GetThreadIdealProcessorEx) {
        log::Write("topology: core placement APIs not available, only priorities will be tuned");
        return;
    }

    // Threads of a process that is not processor-group aware stay in one group. Only handle that case.
    WORD procGroup = 0;
    if (api_.GetProcessGroupAffinity) {
        USHORT groups[16];
        USHORT count = 16;
        if (!api_.GetProcessGroupAffinity(GetCurrentProcess(), &count, groups) || count != 1) {
            log::Write("topology: process spans several processor groups, core placement disabled");
            return;
        }
        procGroup = groups[0];
    }

    DWORD_PTR procMask = 0, sysMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) || procMask == 0) return;

    DWORD len = 0;
    api_.GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return;
    std::vector<BYTE> buf(len);
    if (!api_.GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) return;

    for (DWORD off = 0; off + 8 <= len;) {
        auto* r = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
        if (r->Size == 0) break;
        if (r->Relationship == RelationProcessorCore && r->Processor.GroupCount >= 1) {
            const GROUP_AFFINITY& ga = r->Processor.GroupMask[0];
            if (ga.Group == procGroup) {
                const KAFFINITY allowed = ga.Mask & procMask;
                if (allowed) {
                    BYTE first = 0;
                    while (!((allowed >> first) & 1)) ++first;
                    cores_.push_back(Core{ga.Group, first, r->Processor.Reserved[0] /* EfficiencyClass in the Windows SDK layout */, procGroup == 0 && (ga.Mask & 1) != 0});
                }
            }
        }
        off += r->Size;
    }

    {
        unsigned smt = 0, classes = 0;
        BYTE seen[256] = {};
        for (const Core& co : cores_) {
            if (!seen[co.efficiencyClass]) {
                seen[co.efficiencyClass] = 1;
                ++classes;
            }
        }
        for (DWORD off = 0; off + 8 <= len;) {
            auto* r = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
            if (r->Size == 0) break;
            if (r->Relationship == RelationProcessorCore && (r->Processor.Flags & 1 /* LTP_PC_SMT */)) ++smt;
            off += r->Size;
        }
        log::Write("topology: %u physical cores (%u with SMT), %u efficiency class(es)", static_cast<unsigned>(cores_.size()), smt, classes);
    }

    // Core 0 usually takes most interrupts and DPCs. Keep hot threads off it when there are enough other cores.
    if (cfg_.avoidCpu0 && cores_.size() >= 4) {
        cores_.erase(std::remove_if(cores_.begin(), cores_.end(), [](const Core& c) { return c.hasCpu0; }), cores_.end());
    }
    const bool preferPerf = cfg_.preferPerformanceCores;
    std::stable_sort(cores_.begin(), cores_.end(), [preferPerf](const Core& a, const Core& b) {
        if (preferPerf && a.efficiencyClass != b.efficiencyClass) return a.efficiencyClass > b.efficiencyClass;  // higher class = faster core
        return a.firstCpu < b.firstCpu;
    });
    slotUsed_.assign(cores_.size(), false);
}

int CpuTuner::AllocSlot() {
    for (size_t i = 0; i < slotUsed_.size(); ++i) {
        if (!slotUsed_[i]) {
            slotUsed_[i] = true;
            return static_cast<int>(i);
        }
    }
    return -1;  // more hot threads than free cores: tune priority only
}

void CpuTuner::FreeSlot(int slot) {
    if (slot >= 0 && static_cast<size_t>(slot) < slotUsed_.size()) slotUsed_[static_cast<size_t>(slot)] = false;
}

void CpuTuner::ApplyProcessSettings() {
    // 1) timer resolution
    if (cfg_.timerResolution && api_.NtQueryTimerResolution && api_.NtSetTimerResolution) {
        ULONG coarsest = 0, finest = 0, current = 0;  // units of 100 ns; "finest" is the smallest number
        if (api_.NtQueryTimerResolution(&coarsest, &finest, &current) >= 0 && finest > 0) {
            ULONG want = static_cast<ULONG>(cfg_.timerResolutionUs) * 10;
            want = std::max(want, finest);
            if (coarsest > 0) want = std::min(want, coarsest);
            ULONG actual = 0;
            if (api_.NtSetTimerResolution(want, TRUE, &actual) >= 0) {
                timerRequested_ = true;
                timerRequestedUnits_ = want;
                log::Write("timer resolution: requested %lu us, now %lu us", want / 10, actual / 10);
            } else {
                log::Write("timer resolution: request failed");
            }
        }
    }

    // 2) power throttling (EcoQoS) off + do not ignore our timer request when the window is not in focus
    if (cfg_.disablePowerThrottling && api_.SetProcessInformation) {
        PowerThrottlingState st = {kThrottleCurrentVersion, kThrottleExecutionSpeed | kThrottleIgnoreTimerResolution, 0};
        if (api_.SetProcessInformation(GetCurrentProcess(), kProcessPowerThrottling, &st, sizeof(st))) {
            throttlingChanged_ = true;
        } else {
            st.ControlMask = kThrottleExecutionSpeed;  // older Windows 10 builds know only this bit
            if (api_.SetProcessInformation(GetCurrentProcess(), kProcessPowerThrottling, &st, sizeof(st))) {
                throttlingChanged_ = true;
            } else {
                log::Write("power throttling: not supported on this Windows version");
            }
        }
    }

    // 3) priority class: only ever raised
    if (cfg_.processPriority != ProcessPriority::Unchanged) {
        DWORD target = NORMAL_PRIORITY_CLASS;
        switch (cfg_.processPriority) {
            case ProcessPriority::AboveNormal: target = ABOVE_NORMAL_PRIORITY_CLASS; break;
            case ProcessPriority::High: target = HIGH_PRIORITY_CLASS; break;
            default: break;
        }
        const DWORD cur = GetPriorityClass(GetCurrentProcess());
        if (cur != 0 && ProcessRank(cur) < ProcessRank(target) && SetPriorityClass(GetCurrentProcess(), target)) {
            priorityChanged_ = true;
            origPriorityClass_ = cur;
            ourPriorityClass_ = target;
            log::Write("process priority class raised");
        }
    }
}

void CpuTuner::RestoreProcessSettings() {
    if (timerRequested_ && api_.NtSetTimerResolution) {
        ULONG actual = 0;
        api_.NtSetTimerResolution(timerRequestedUnits_, FALSE, &actual);
        timerRequested_ = false;
    }
    if (throttlingChanged_ && api_.SetProcessInformation) {
        PowerThrottlingState st = {kThrottleCurrentVersion, 0, 0};  // 0/0 = let the system decide again
        api_.SetProcessInformation(GetCurrentProcess(), kProcessPowerThrottling, &st, sizeof(st));
        throttlingChanged_ = false;
    }
    if (priorityChanged_) {
        if (GetPriorityClass(GetCurrentProcess()) == ourPriorityClass_) SetPriorityClass(GetCurrentProcess(), origPriorityClass_);
        priorityChanged_ = false;
    }
}

void CpuTuner::SetEnabled(bool on) {
    if (!initialised_) return;
    if (on == enabled_.load(std::memory_order_relaxed)) return;
    if (on) {
        ApplyProcessSettings();
        enabled_.store(true, std::memory_order_relaxed);
        log::Write("CPU fix ENABLED");
    } else {
        enabled_.store(false, std::memory_order_relaxed);
        RestoreAllThreads();
        RestoreProcessSettings();
        log::Write("CPU fix DISABLED, all settings restored");
    }
}

void CpuTuner::Shutdown() {
    SetEnabled(false);
    for (auto& kv : threads_) {
        if (kv.second.handle) CloseHandle(kv.second.handle);
    }
    threads_.clear();
}

// "GameThread" [game.exe] main   -- name set by the game (if any), module the thread started in, and its role
std::string CpuTuner::BuildLabel(DWORD tid, const ThreadState& s) const {
    std::string name, module;
    if (api_.GetThreadDescription && s.handle) {
        PWSTR desc = nullptr;
        if (SUCCEEDED(api_.GetThreadDescription(s.handle, &desc)) && desc) {
            for (const wchar_t* p = desc; *p && name.size() < 40; ++p) name.push_back((*p >= 32 && *p < 127) ? static_cast<char>(*p) : '?');
            LocalFree(desc);
        }
    }
    if (api_.NtQueryInformationThread && s.handle) {
        PVOID start = nullptr;
        // class 9 = ThreadQuerySetWin32StartAddress
        if (api_.NtQueryInformationThread(s.handle, 9, &start, sizeof(start), nullptr) >= 0 && start) {
            HMODULE m = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(start), &m) && m) {
                wchar_t path[MAX_PATH];
                const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
                if (n > 0 && n < MAX_PATH) {
                    const wchar_t* b = path + n;
                    while (b > path && b[-1] != L'\\' && b[-1] != L'/') --b;
                    for (; *b && module.size() < 40; ++b) module.push_back((*b >= 32 && *b < 127) ? static_cast<char>(*b) : '?');
                }
            }
        }
    }
    std::string out;
    if (!name.empty()) out += "\"" + name + "\" ";
    if (!module.empty()) out += "[" + module + "]";
    if (out.empty()) out = "(unnamed)";
    if (tid == mainTid_) out += " main";
    if (tid == lastRenderTid_) out += " render";
    return out;
}

std::string CpuTuner::TopThreadsReport(size_t count) {
    std::vector<std::pair<double, DWORD>> ranked;
    for (const auto& kv : threads_) {
        if (kv.second.load >= 0.02) ranked.emplace_back(kv.second.load, kv.first);
    }
    std::sort(ranked.begin(), ranked.end(), [](const std::pair<double, DWORD>& a, const std::pair<double, DWORD>& b) { return a.first > b.first; });
    if (ranked.size() > count) ranked.resize(count);
    std::string out;
    for (const auto& r : ranked) {
        const ThreadState& st = threads_[r.second];
        char buf[96];
        std::snprintf(buf, sizeof(buf), "tid %lu %.0f%% (kernel %.0f%%) ", static_cast<unsigned long>(r.second), st.load * 100.0, st.kernelShare * 100.0);
        out += buf;
        out += BuildLabel(r.second, st);
        out += "; ";
    }
    return out.empty() ? std::string("no busy threads") : out;
}

void CpuTuner::TuneThread(DWORD tid, ThreadState& s, bool critical) {
    if (!s.handle) return;
    s.tuned = true;
    s.critical = critical;
    s.missed = 0;

    // Only "critical" threads (the one that presents, and the main thread) may get a higher priority, and only if
    // the game left them at normal. Background workers keep whatever priority the game gave them.
    const int cur = GetThreadPriority(s.handle);
    if (cur != THREAD_PRIORITY_ERROR_RETURN) {
        s.origPriority = cur;
        int target = 0;
        bool want = false;
        switch (cfg_.threadPriority) {
            case ThreadPriority::AboveNormal: target = THREAD_PRIORITY_ABOVE_NORMAL; want = true; break;
            case ThreadPriority::Highest: target = THREAD_PRIORITY_HIGHEST; want = true; break;
            default: break;
        }
        if (critical && want && cur == THREAD_PRIORITY_NORMAL && SetThreadPriority(s.handle, target)) {
            s.prioChanged = true;
            s.ourPriority = target;
        }
    }

    // Ideal processor is only a scheduling hint: Windows may still move the thread when the core is busy.
    s.slot = AllocSlot();
    if (s.slot >= 0) {
        PROCESSOR_NUMBER orig{};
        if (api_.GetThreadIdealProcessorEx(s.handle, &orig)) {
            PROCESSOR_NUMBER pn{};
            pn.Group = cores_[static_cast<size_t>(s.slot)].group;
            pn.Number = cores_[static_cast<size_t>(s.slot)].firstCpu;
            PROCESSOR_NUMBER prev{};
            if (api_.SetThreadIdealProcessorEx(s.handle, &pn, &prev)) {
                s.idealChanged = true;
                s.origIdeal = orig;
                s.ourIdeal = pn;
            }
        }
    }
    log::Write("tuned thread %lu %s: load %.0f%%, priority %s, ideal cpu %s", static_cast<unsigned long>(tid), BuildLabel(tid, s).c_str(), s.load * 100.0,
               s.prioChanged ? "raised" : "unchanged", s.idealChanged ? "set" : "unchanged");
}

void CpuTuner::RestoreThread(ThreadState& s) {
    if (s.handle) {
        if (s.prioChanged && GetThreadPriority(s.handle) == s.ourPriority) SetThreadPriority(s.handle, s.origPriority);
        if (s.idealChanged && api_.GetThreadIdealProcessorEx && api_.SetThreadIdealProcessorEx) {
            PROCESSOR_NUMBER now{};
            if (api_.GetThreadIdealProcessorEx(s.handle, &now) && now.Group == s.ourIdeal.Group && now.Number == s.ourIdeal.Number) {
                PROCESSOR_NUMBER orig = s.origIdeal;
                api_.SetThreadIdealProcessorEx(s.handle, &orig, nullptr);
            }
        }
    }
    FreeSlot(s.slot);
    s.slot = -1;
    s.prioChanged = false;
    s.idealChanged = false;
    s.tuned = false;
    s.critical = false;
    s.missed = 0;
}

void CpuTuner::RestoreAllThreads() {
    for (auto& kv : threads_) {
        if (kv.second.tuned) RestoreThread(kv.second);
    }
    metrics_.tunedThreads = 0;
}

void CpuTuner::Tick(DWORD renderTid, DWORD selfTid, bool wantMetrics) {
    if (!initialised_) return;
    const bool enabled = enabled_.load(std::memory_order_relaxed);
    if (!wantMetrics && !enabled) return;
    const uint64_t now = Qpc();
    const uint64_t sinceMs = lastAttemptQpc_ ? (now - lastAttemptQpc_) * 1000 / qpcFreq_ : ~0ull;

    if (wantMetrics) {  // overlay visible or logging: measure every thread
        if (sinceMs < static_cast<uint64_t>(cfg_.sampleMs)) return;
        lastAttemptQpc_ = now;
        Sample(now, renderTid, selfTid);
        return;
    }

    // Nobody looks at the numbers: keep the two critical threads tuned, and rescan everything only once a minute
    // (the scan is the expensive part: it walks every thread in the system).
    if (sinceMs < kIdleTickMs) return;
    lastAttemptQpc_ = now;
    const bool fullDue = mainTid_ == 0 || !threads_.count(mainTid_) || (now - lastFullQpc_) * 1000 / qpcFreq_ > kIdleFullScanMs;
    if (fullDue) {
        Sample(now, renderTid, selfTid);
    } else {
        TuneCriticalOnly(renderTid, selfTid);
    }
}

void CpuTuner::TuneCriticalOnly(DWORD renderTid, DWORD selfTid) {
    if (!cfg_.threadTuning) return;
    auto ensure = [&](DWORD tid) {
        if (tid == 0 || tid == selfTid) return;
        auto it = threads_.find(tid);
        if (it == threads_.end()) {
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, FALSE, tid);
            if (!h) return;
            ThreadState fresh;
            fresh.handle = h;
            it = threads_.emplace(tid, fresh).first;
        }
        ThreadState& st = it->second;
        st.missed = 0;
        if (st.tuned && st.critical) return;
        if (st.tuned) RestoreThread(st);
        TuneThread(tid, st, true);
    };
    ensure(renderTid);
    ensure(mainTid_);
    for (auto& kv : threads_) {
        ThreadState& st = kv.second;
        if (!st.tuned || kv.first == renderTid || kv.first == mainTid_) continue;
        if (++st.missed >= kMissedSamplesBeforeRestore) RestoreThread(st);
    }
    int tuned = 0;
    for (const auto& kv : threads_) tuned += kv.second.tuned ? 1 : 0;
    metrics_.tunedThreads = tuned;
}

void CpuTuner::Sample(uint64_t now, DWORD renderTid, DWORD selfTid) {
    const double wall100ns = lastSampleQpc_ ? static_cast<double>(now - lastSampleQpc_) * 1.0e7 / static_cast<double>(qpcFreq_) : 0.0;

    uint64_t procCpu = 0;
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) procCpu = FileTime100ns(k) + FileTime100ns(u);

    // list this process's threads
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;  // try again at the next sample
    std::vector<DWORD> alive;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) alive.push_back(te.th32ThreadID);
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    std::sort(alive.begin(), alive.end());

    // forget threads that are gone (our open handle kept their id from being reused until now)
    for (auto it = threads_.begin(); it != threads_.end();) {
        if (!std::binary_search(alive.begin(), alive.end(), it->first)) {
            FreeSlot(it->second.slot);
            if (it->second.handle) CloseHandle(it->second.handle);
            it = threads_.erase(it);
        } else {
            ++it;
        }
    }

    // measure each thread
    double hottest = 0;
    DWORD hottestTid = 0;
    for (DWORD tid : alive) {
        auto found = threads_.find(tid);
        if (found == threads_.end()) {
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, FALSE, tid);
            if (!h) h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (!h) continue;
            ThreadState fresh;
            fresh.handle = h;
            found = threads_.emplace(tid, fresh).first;
        }
        ThreadState& st = found->second;
        FILETIME tc, tx, tk, tu;
        if (GetThreadTimes(st.handle, &tc, &tx, &tk, &tu)) {
            const uint64_t kernel = FileTime100ns(tk);
            const uint64_t cpu = kernel + FileTime100ns(tu);
            if (st.created100ns == 0) st.created100ns = FileTime100ns(tc);
            if (st.haveBaseline && wall100ns > 0 && cpu >= st.lastCpu100ns) {
                const uint64_t busy = cpu - st.lastCpu100ns;
                st.load = std::min(1.0, static_cast<double>(busy) / wall100ns);
                const uint64_t kernelDelta = kernel >= st.lastKernel100ns ? kernel - st.lastKernel100ns : 0;
                st.kernelShare = busy ? std::min(1.0, static_cast<double>(kernelDelta) / static_cast<double>(busy)) : 0.0;
            }
            st.lastCpu100ns = cpu;
            st.lastKernel100ns = kernel;
            st.haveBaseline = true;
        }
        if (tid != selfTid && (hottestTid == 0 || st.load > hottest)) {
            hottest = st.load;
            hottestTid = tid;
        }
    }

    // The main thread is the oldest one still alive (the process's initial thread).
    DWORD mainTid = 0;
    uint64_t oldest = ~0ull;
    for (const auto& kv : threads_) {
        if (kv.first == selfTid || kv.second.created100ns == 0) continue;
        if (kv.second.created100ns < oldest) {
            oldest = kv.second.created100ns;
            mainTid = kv.first;
        }
    }
    mainTid_ = mainTid;
    lastRenderTid_ = renderTid;

    // metrics for the overlay and the log
    if (samples_ >= 1 && wall100ns > 0) {
        metrics_.valid = true;
        const double procDelta = procCpu >= lastProcCpu100ns_ ? static_cast<double>(procCpu - lastProcCpu100ns_) : 0.0;
        metrics_.processLoad = std::min(1.0, procDelta / (wall100ns * static_cast<double>(cpuCountForLoad_)));
        metrics_.hottestThreadLoad = hottest;
        metrics_.hottestTid = hottestTid;
        metrics_.hottestKernelShare = 0;
        metrics_.hottestLabel.clear();
        if (hottestTid != 0) {
            ThreadState& hs = threads_[hottestTid];
            metrics_.hottestKernelShare = hs.kernelShare;
            if (!hs.haveLabel || static_cast<uint32_t>(samples_) - hs.labelSeq >= 20) {  // names rarely change: refresh every ~20 samples
                hs.label = BuildLabel(hottestTid, hs);
                hs.haveLabel = true;
                hs.labelSeq = static_cast<uint32_t>(samples_);
            }
            metrics_.hottestLabel = hs.label;
        }
    }
    lastProcCpu100ns_ = procCpu;
    lastSampleQpc_ = now;
    lastFullQpc_ = now;
    ++samples_;
    metrics_.sampleSeq = static_cast<uint32_t>(samples_);

    auto countTuned = [this]() {
        int tuned = 0;
        for (const auto& kv : threads_) tuned += kv.second.tuned ? 1 : 0;
        metrics_.tunedThreads = tuned;
    };
    if (!enabled_.load(std::memory_order_relaxed) || !cfg_.threadTuning || samples_ < 2) {
        countTuned();
        return;
    }

    // Who gets tuned: the thread that presents and the main thread (priority + own core), plus optionally the
    // busiest background workers (own core only, never a priority change).
    struct Want {
        DWORD tid;
        bool critical;
    };
    std::vector<Want> desired;
    auto isDesired = [&desired](DWORD tid) {
        return std::any_of(desired.begin(), desired.end(), [tid](const Want& w) { return w.tid == tid; });
    };
    auto addCritical = [&](DWORD tid) {
        if (tid != 0 && tid != selfTid && threads_.count(tid) && !isDesired(tid)) desired.push_back({tid, true});
    };
    addCritical(renderTid);
    addCritical(mainTid);

    if (cfg_.workerThreads > 0) {
        std::vector<std::pair<double, DWORD>> ranked;
        for (const auto& kv : threads_) {
            if (kv.first == selfTid || isDesired(kv.first)) continue;
            if (kv.second.load >= (kv.second.tuned ? kWorkerStayLoad : kWorkerEnterLoad)) ranked.emplace_back(kv.second.load, kv.first);
        }
        std::sort(ranked.begin(), ranked.end(), [](const std::pair<double, DWORD>& a, const std::pair<double, DWORD>& b) { return a.first > b.first; });
        for (const auto& r : ranked) {
            if (static_cast<int>(desired.size()) >= 2 + cfg_.workerThreads) break;  // render + main + extras
            desired.push_back({r.second, false});
        }
    }

    for (const Want& w : desired) {
        ThreadState& st = threads_[w.tid];
        if (st.tuned && st.critical != w.critical) RestoreThread(st);
        if (!st.tuned) TuneThread(w.tid, st, w.critical);
        st.missed = 0;
    }
    for (auto& kv : threads_) {
        ThreadState& st = kv.second;
        if (!st.tuned || isDesired(kv.first)) continue;
        if (++st.missed >= kMissedSamplesBeforeRestore) RestoreThread(st);
    }
    countTuned();
}

}  // namespace cbf
