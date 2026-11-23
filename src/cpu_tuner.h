// CpuTuner: the "CPU bottleneck fix". Everything it changes is reversible (SetEnabled(false) restores the
// original values), and it never lowers a priority the game chose itself.
//
// What it does while enabled:
//   * asks Windows for a finer timer (better Sleep / wait accuracy -> steadier frame pacing)
//   * opts the process out of EcoQoS / power throttling and out of the "ignore timer resolution" behaviour
//   * raises the process priority class (never lowers it)
//   * finds the busiest threads (plus the thread that calls Present) and gives them a slightly higher priority
//     and a distinct physical core as ideal processor (soft hint, prefers performance cores, avoids core 0)
// It also measures per-thread CPU load, which the overlay uses to tell whether one thread is the bottleneck.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"

namespace cbf {

class CpuTuner {
public:
    struct Metrics {
        bool valid = false;            // at least two samples taken
        double processLoad = 0;        // 0..1 of all CPUs available to the process
        double hottestThreadLoad = 0;  // 0..1 of one core, busiest thread
        double hottestKernelShare = 0; // share of that thread's busy time spent in kernel mode (drivers, syscalls)
        DWORD hottestTid = 0;
        std::string hottestLabel;      // e.g. "RenderThread" [game.exe] render
        int tunedThreads = 0;
        int allowedCpus = 0;           // logical CPUs the process may run on
        int systemCpus = 0;            // logical CPUs the system has (in the process's group)
        int restrictedThreads = 0;     // threads pinned to fewer CPUs than the process may use
        int widenedThreads = 0;        // of those, threads we widened
        uint32_t sampleSeq = 0;        // increments with every completed sample
    };

    CpuTuner();
    ~CpuTuner();

    void Init(const CpuFixConfig& cfg);   // resolves APIs and CPU topology; changes nothing yet
    void SetEnabled(bool on);             // apply / restore. Worker thread only.
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Call regularly from ONE thread. Cheap: with wantMetrics it measures every thread each cfg.sampleMs; without it
    // (overlay hidden, log off) it only keeps the two critical threads tuned and does a full scan once a minute.
    // renderThreadId = thread that calls Present (0 if unknown), selfThreadId = the calling thread (never tuned).
    void Tick(DWORD renderThreadId, DWORD selfThreadId, bool wantMetrics);

    Metrics GetMetrics() const { return metrics_; }
    // One-line description of the `count` busiest threads (id, load, kernel share, name, module). Worker thread only.
    std::string TopThreadsReport(size_t count);
    void Shutdown();                      // same as SetEnabled(false)

private:
    struct Core {
        WORD group;
        BYTE firstCpu;
        BYTE efficiencyClass;
        bool hasCpu0;
    };
    struct ThreadState {
        HANDLE handle = nullptr;
        uint64_t lastCpu100ns = 0;
        bool haveBaseline = false;
        double load = 0;
        uint64_t created100ns = 0;
        uint64_t lastKernel100ns = 0;
        double kernelShare = 0;
        std::string label;
        bool haveLabel = false;
        uint32_t labelSeq = 0;
        DWORD_PTR affinityMask = 0;       // last seen affinity mask of the thread
        bool affinityWidened = false;
        DWORD_PTR origAffinity = 0, ourAffinity = 0;
        bool tuned = false;
        bool ignore = false;     // looked at and deliberately left alone (e.g. a frame-generation pacer thread)
        bool critical = false;
        int slot = -1;
        int missed = 0;
        bool prioChanged = false;
        int origPriority = 0, ourPriority = 0;
        bool idealChanged = false;
        PROCESSOR_NUMBER origIdeal{}, ourIdeal{};
    };

    void BuildTopology();
    void ApplyProcessSettings();
    void RestoreProcessSettings();
    void Sample(uint64_t nowQpc, DWORD renderTid, DWORD selfTid);
    void TuneCriticalOnly(DWORD renderTid, DWORD selfTid);
    void TuneThread(DWORD tid, ThreadState& s, bool critical);
    std::string BuildLabel(DWORD tid, const ThreadState& s) const;
    void RestoreThread(ThreadState& s);
    void RestoreAffinity(ThreadState& s);
    void RefreshAffinityState();
    void RestoreAllThreads();
    int AllocSlot();
    void FreeSlot(int slot);

    CpuFixConfig cfg_;
    std::atomic<bool> enabled_{false};
    bool initialised_ = false;

    // resolved at runtime so the plugin also loads on old Windows versions
    struct Api {
        LONG(NTAPI* NtSetTimerResolution)(ULONG, BOOLEAN, PULONG) = nullptr;
        LONG(NTAPI* NtQueryTimerResolution)(PULONG, PULONG, PULONG) = nullptr;
        BOOL(WINAPI* SetProcessInformation)(HANDLE, int, LPVOID, DWORD) = nullptr;
        BOOL(WINAPI* GetLogicalProcessorInformationEx)(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD) = nullptr;
        BOOL(WINAPI* SetThreadIdealProcessorEx)(HANDLE, PPROCESSOR_NUMBER, PPROCESSOR_NUMBER) = nullptr;
        BOOL(WINAPI* GetThreadIdealProcessorEx)(HANDLE, PPROCESSOR_NUMBER) = nullptr;
        BOOL(WINAPI* GetProcessGroupAffinity)(HANDLE, PUSHORT, PUSHORT) = nullptr;
        LONG(NTAPI* NtQueryInformationThread)(HANDLE, ULONG, PVOID, ULONG, PULONG) = nullptr;
        HRESULT(WINAPI* GetThreadDescription)(HANDLE, PWSTR*) = nullptr;
        BOOL(WINAPI* GetProcessInformation)(HANDLE, int, LPVOID, DWORD) = nullptr;
        BOOL(WINAPI* GetThreadGroupAffinity)(HANDLE, PGROUP_AFFINITY) = nullptr;
    } api_;

    // process-level state
    bool timerRequested_ = false;
    ULONG timerRequestedUnits_ = 0;
    bool throttlingChanged_ = false;
    bool affinityChanged_ = false;            // we widened the process affinity
    DWORD_PTR origProcessAffinity_ = 0, ourProcessAffinity_ = 0;
    bool singleGroup_ = true;                 // the process lives in one processor group (affinity work only then)
    WORD procGroup_ = 0;
    int lastRestrictedLogged_ = -1;
    uint64_t nextRestrictedLogQpc_ = 0;
    bool haveOrigThrottle_ = false;           // the state before we changed it could be read
    ULONG origThrottleControl_ = 0, origThrottleState_ = 0;
    bool priorityChanged_ = false;
    DWORD origPriorityClass_ = 0, ourPriorityClass_ = 0;

    // topology
    std::vector<Core> cores_;     // candidate cores, best first
    std::vector<bool> slotUsed_;
    int slotWrap_ = 0;
    unsigned cpuCountForLoad_ = 1;

    // sampling
    std::unordered_map<DWORD, ThreadState> threads_;
    uint64_t lastAttemptQpc_ = 0;  // last time a sample was tried
    uint64_t lastSampleQpc_ = 0;   // last time a sample succeeded (basis for load deltas)
    uint64_t lastFullQpc_ = 0;     // last full scan of all threads
    uint64_t lastProcCpu100ns_ = 0;
    uint64_t qpcFreq_ = 1;
    int samples_ = 0;
    DWORD mainTid_ = 0;
    DWORD lastRenderTid_ = 0;
    Metrics metrics_;
};

}  // namespace cbf
