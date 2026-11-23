// Models the affinity decisions of CpuTuner (same conditions, same order) with fake process/thread masks, to trace
// scenarios that cannot be run without Windows. Build: g++ -std=c++17 -I src tests/affinity_state_trace.cpp src/tuner_policy.cpp
#include <cstdint>
#include <cstdio>
#include <vector>
#include "tuner_policy.h"
using namespace cbf;
struct Thread { uint64_t mask; bool widened = false; uint64_t orig = 0, ours = 0; };
struct Proc { uint64_t mask, sys; bool changed = false; uint64_t orig = 0, ours = 0; std::vector<Thread> threads; };
static void ApplyProcess(Proc& p) { if (ShouldWidenAffinity(p.mask, p.sys)) { p.changed = true; p.orig = p.mask; p.ours = p.sys; p.mask = p.sys; } }
static void SampleThreads(Proc& p, bool widenThreads) {
    for (Thread& t : p.threads) {
        if (t.widened && t.mask == t.ours) continue;
        if (ShouldWidenAffinity(t.mask, p.mask) && widenThreads) { t.widened = true; t.orig = t.mask; t.ours = p.mask; t.mask = p.mask; }
    }
}
// A thread's affinity can be re-sampled many times while it stays widened; SampleThreads mirrors the real
// cpu_tuner.cpp fix: only the FIRST original mask is ever recorded.
static void SampleThreadsRealistic(Proc& p, bool widenThreads) {
    for (Thread& t : p.threads) {
        if (t.widened && t.mask == t.ours) continue;             // still exactly as we left it
        if (ShouldWidenAffinity(t.mask, p.mask) && widenThreads) {
            if (!t.widened) t.orig = t.mask;                       // <-- the fix: set once, never again
            t.widened = true;
            t.ours = p.mask;
            t.mask = p.mask;
        }
    }
}

static int g_bad = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_bad; } } while (0)
static void Disable(Proc& p) {
    for (Thread& t : p.threads) if (t.widened) { if (t.mask == t.ours) t.mask = t.orig; t.widened = false; }   // threads first ...
    if (p.changed) { if (p.mask == p.ours) p.mask = p.orig; p.changed = false; }                                  // ... then the process
}
int main() {
    { Proc p{0x7F, 0xFF, false, 0, 0, {{0x01}, {0x7F}, {0x0F}}};                  // game restricted to 7 of 8 CPUs, two pinned threads
      ApplyProcess(p); EXPECT(p.mask == 0xFF);
      SampleThreads(p, false); EXPECT(p.threads[0].mask == 0x01 && p.threads[2].mask == 0x0F);     // thread widening is opt-in
      SampleThreads(p, true);  EXPECT(p.threads[0].mask == 0xFF && p.threads[1].mask == 0xFF && p.threads[2].mask == 0xFF);
      Disable(p); EXPECT(p.mask == 0x7F && p.threads[0].mask == 0x01 && p.threads[1].mask == 0x7F && p.threads[2].mask == 0x0F); }
    { Proc p{0xFF, 0xFF, false, 0, 0, {{0xFF}}};                                      // already everything: no change, nothing to restore
      ApplyProcess(p); EXPECT(!p.changed && p.mask == 0xFF); Disable(p); EXPECT(p.mask == 0xFF); }
    { Proc p{0x7F, 0xFF, false, 0, 0, {{0x01}}};                                      // the game changes masks itself after we widened: leave its choice alone
      ApplyProcess(p); SampleThreads(p, true);
      p.mask = 0x0F; p.threads[0].mask = 0x02; Disable(p);
      EXPECT(p.mask == 0x0F && p.threads[0].mask == 0x02); }
    { Proc p{0x7F, 0xFF, false, 0, 0, {{0x80}}};                                      // pinned to CPU 7: only valid once the process is widened,
      ApplyProcess(p); SampleThreads(p, true);                                         // and then it is a normal pinned thread, widened like the others
      EXPECT(p.threads[0].mask == 0xFF && p.threads[0].widened); Disable(p); EXPECT(p.threads[0].mask == 0x80 && p.mask == 0x7F); }
    { Proc p{0x0F, 0x0F, false, 0, 0, {{0x1F}}};                                      // a mask with a CPU outside the process mask is never touched
      ApplyProcess(p); SampleThreads(p, true); EXPECT(p.threads[0].mask == 0x1F && !p.threads[0].widened); }
    // Regression for a real bug found by manual review (cpu_tuner.cpp:619-624, fixed): a thread that is re-narrowed
    // to a DIFFERENT subset mask while already widened must still restore to its TRUE original, not to whatever
    // intermediate mask it held at the moment we last touched it.
    { Proc p{0xFF, 0xFF, false, 0, 0, {{0x01}}};
      SampleThreadsRealistic(p, true); EXPECT(p.threads[0].mask == 0xFF && p.threads[0].orig == 0x01);
      p.threads[0].mask = 0x0F;                                    // re-narrowed to a different subset
      SampleThreadsRealistic(p, true);
      EXPECT(p.threads[0].mask == 0xFF && p.threads[0].orig == 0x01);   // must still be the TRUE original, not 0x0F
      p.threads[0].mask = 0x03;                                    // re-narrowed again
      SampleThreadsRealistic(p, true);
      EXPECT(p.threads[0].orig == 0x01);
      Disable(p); EXPECT(p.threads[0].mask == 0x01); }              // restores to the real original
    std::printf(g_bad ? "affinity model: PROBLEMS\n" : "affinity model: all traced scenarios behave as intended\n");
    return g_bad ? 1 : 0;
}
