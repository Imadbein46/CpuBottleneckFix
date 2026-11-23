// Native unit tests for the portable modules: hotkey parsing, FrameClock, bottleneck classifier.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../src/bottleneck.h"
#include "../src/frame_clock.h"
#include "../src/hotkey.h"
#include "../src/power_guids.h"
#include "../src/tuner_policy.h"

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

using namespace cbf;

static Hotkey P(const char* s, bool* ok = nullptr) {
    Hotkey h;
    bool r = ParseHotkey(s, h);
    if (ok) *ok = r;
    return h;
}

static void TestHotkeys() {
    bool ok;
    Hotkey h = P("CTRL+F9", &ok);
    CHECK(ok && h.vk == 0x78 && h.ctrl && !h.alt && !h.shift);
    h = P("  ctrl + alt + shift + f12 ", &ok);
    CHECK(ok && h.vk == 0x7B && h.ctrl && h.alt && h.shift);
    h = P("F1", &ok);
    CHECK(ok && h.vk == 0x70 && !h.ctrl);
    h = P("f24", &ok);
    CHECK(ok && h.vk == 0x87);
    h = P("x", &ok);
    CHECK(ok && h.vk == 'X');
    h = P("7", &ok);
    CHECK(ok && h.vk == '7');
    h = P("NUM5", &ok);
    CHECK(ok && h.vk == 0x65);
    h = P("shift+INSERT", &ok);
    CHECK(ok && h.vk == 0x2D && h.shift);
    h = P("0x77", &ok);
    CHECK(ok && h.vk == 0x77);
    h = P("F9 ; trailing comment", &ok);
    CHECK(ok && h.vk == 0x78);
    h = P("", &ok);
    CHECK(ok && !h.bound());
    h = P("none", &ok);
    CHECK(ok && !h.bound());
    h = P("Disabled", &ok);
    CHECK(ok && !h.bound());
    // garbage must be rejected and leave the key unbound
    h = P("F", &ok);
    CHECK(ok && h.vk == 'F');  // the letter key F, not a function key
    for (const char* bad : {"CTRL", "CTRL+", "+F9", "F25", "F0", "FOO", "A+B", "CTRL+ALT", "0x00", "0xFF", "0x", "NUM", "NUM10", "CTRL++F9"}) {
        h = P(bad, &ok);
        CHECK(!ok);
        CHECK(!h.bound());
    }
}

// ---- FrameClock ----
static const uint64_t TPS = 10000000;  // 10 MHz like a typical QPC
static uint64_t Ms(double ms) { return static_cast<uint64_t>(ms * TPS / 1000.0); }

// feed `count` frames of `frameMs` each, spending `presentMs` inside Present; returns the end time
static uint64_t Feed(FrameClock& c, uint64_t t, int count, double frameMs, double presentMs, uintptr_t key = 1) {
    for (int i = 0; i < count; ++i) {
        auto tk = c.Begin(key, t);
        c.End(tk, t + Ms(presentMs));
        t += Ms(frameMs);
    }
    return t;
}

static void TestFrameClockBasics() {
    FrameClock c(TPS);
    CHECK(!c.Snapshot(0).valid);

    uint64_t t = Ms(1000);
    t = Feed(c, t, 300, 10.0, 2.0);  // 100 fps
    FrameStats s = c.Snapshot(t);
    CHECK(s.valid);
    NEAR(s.fps, 100.0, 0.5);
    NEAR(s.frameMsAvg, 10.0, 0.05);
    NEAR(s.frameMsMax, 10.0, 0.05);
    NEAR(s.presentMsAvg, 2.0, 0.05);
    NEAR(s.low1pctFps, 100.0, 1.0);
    CHECK(s.totalFrames == 299);  // the first frame has no interval
    NEAR(s.msSinceLastFrame, 10.0, 0.1);
}

static void TestFrameClockSpikesAndLows() {
    FrameClock c(TPS);
    uint64_t t = Ms(5000);
    // 2% of frames are 50 ms spikes (one in 50), the rest 5 ms. Total 4.7 s, inside the 5 s window.
    for (int rep = 0; rep < 16; ++rep) {
        t = Feed(c, t, 49, 5.0, 1.0);
        t = Feed(c, t, 1, 50.0, 1.0);
    }
    FrameStats s = c.Snapshot(t);
    CHECK(s.valid);
    NEAR(s.frameMsMax, 50.0, 0.1);                       // the spike is visible in the last second
    CHECK(s.low1pctFps > 19.0 && s.low1pctFps < 21.0);   // 1% low = 1000/50 = 20 fps
    CHECK(s.fps > 100.0 && s.fps < 200.0);

    // Exactly 1% spikes: the 99th-percentile frame is a normal one, so the 1% low stays at the normal rate.
    FrameClock d(TPS);
    uint64_t u = Ms(5000);
    for (int rep = 0; rep < 8; ++rep) {
        u = Feed(d, u, 99, 5.0, 1.0);
        u = Feed(d, u, 1, 50.0, 1.0);
    }
    NEAR(d.Snapshot(u).low1pctFps, 200.0, 1.0);
}

static void TestFrameClockPauseAndSecondarySurface() {
    FrameClock c(TPS);
    uint64_t t = Ms(1000);
    t = Feed(c, t, 100, 10.0, 1.0);
    const uint64_t before = c.Snapshot(t).totalFrames;

    // a pause longer than 5 s (alt-tab) must not become a giant "frame"
    t += Ms(30000);
    t = Feed(c, t, 50, 10.0, 1.0);
    FrameStats s = c.Snapshot(t);
    NEAR(s.frameMsMax, 10.0, 0.1);
    CHECK(s.totalFrames == before + 49);  // first frame after the pause only restarts the interval

    // a second surface (other window / swap chain) is ignored while the first is alive
    FrameClock d(TPS);
    uint64_t u = Ms(1000);
    for (int i = 0; i < 100; ++i) {
        auto a = d.Begin(111, u);
        d.End(a, u + Ms(1));
        auto b = d.Begin(222, u + Ms(5));   // secondary surface, presented in between
        CHECK(!b.counted);  // secondary surface must never be counted
        d.End(b, u + Ms(6));
        u += Ms(10);
    }
    FrameStats ds = d.Snapshot(u);
    NEAR(ds.fps, 100.0, 1.0);  // not 200

    // when the primary goes silent for > 1 s the other surface takes over
    uint64_t w = u + Ms(2000);
    for (int i = 0; i < 50; ++i) {
        auto b = d.Begin(222, w);
        d.End(b, w + Ms(1));
        w += Ms(20);
    }
    FrameStats ws = d.Snapshot(w);
    NEAR(ws.fps, 50.0, 1.0);
}

static void TestFrameClockFewFramesAndStall() {
    FrameClock c(TPS);
    uint64_t t = Ms(100);
    t = Feed(c, t, 3, 500.0, 0.0);  // 2 fps
    FrameStats s = c.Snapshot(t);
    CHECK(s.valid);
    NEAR(s.fps, 2.0, 0.1);
    CHECK(s.low1pctFps == 0.0);  // too few frames for a 1% low

    FrameStats stale = c.Snapshot(t + Ms(3000));
    CHECK(stale.msSinceLastFrame > 3000.0);
}

static void TestFrameClockRingWrap() {
    FrameClock c(TPS);
    uint64_t t = Ms(1000);
    t = Feed(c, t, FrameClock::kRing * 3 + 17, 0.5, 0.1);  // 2000 fps, wraps the ring several times
    FrameStats s = c.Snapshot(t);
    CHECK(s.valid);
    NEAR(s.fps, 2000.0, 20.0);
    NEAR(s.frameMsMax, 0.5, 0.01);
}

static void TestFrameClockConcurrent() {
    // one writer, one reader hammering Snapshot: must never crash or produce garbage
    FrameClock c(TPS);
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::thread reader([&] {
        while (!stop.load()) {
            FrameStats s = c.Snapshot(Ms(1000) + Ms(1000000));
            if (s.valid && (s.fps < 0 || s.fps > 1e7 || s.frameMsAvg < 0 || std::isnan(s.fps))) ++bad;
        }
    });
    uint64_t t = Ms(1000);
    for (int i = 0; i < 400000; ++i) {
        auto tk = c.Begin(1, t);
        c.End(tk, t + Ms(0.2));
        t += Ms(1.0);
    }
    stop = true;
    reader.join();
    CHECK(bad == 0);
}

static void TestFrameClockCounters() {
    FrameClock c(TPS);
    uint64_t t = Ms(1000);
    t = Feed(c, t, 10, 10.0, 1.0);                       // 10 frames on surface 1
    auto sec = c.Begin(2, t);                            // a secondary surface
    c.End(sec, t + Ms(1));
    t += Ms(30000);                                      // 30 s pause
    t = Feed(c, t, 3, 10.0, 1.0);                        // surface 1 again after a long gap
    FrameCounters k = c.Counters();
    CHECK(k.begins == 14);
    CHECK(k.ignoredSecondary == 1);
    CHECK(k.primarySwitches == 1);
    CHECK(k.longGaps == 1);
    CHECK(k.clockNotForward == 0);
    CHECK(k.entries == 9 + 2);                           // first frame and the one after the pause have no interval

    // a clock that does not move forward never produces a frame, and the counter says so
    FrameClock d(TPS);
    for (int i = 0; i < 20; ++i) {
        auto tk = d.Begin(7, Ms(5000));                  // same timestamp every time
        d.End(tk, Ms(5000));
    }
    CHECK(d.Snapshot(Ms(5000)).totalFrames == 0);
    CHECK(d.Counters().clockNotForward == 19);
}

static std::vector<uint32_t> Ids(const std::vector<Target>& t) {
    std::vector<uint32_t> v;
    for (const Target& x : t) v.push_back(x.tid);
    return v;
}

static void TestTunerPolicy() {
    const std::vector<ThreadView> th = {{10, 0.05, false}, {11, 0.95, false}, {12, 0.70, false}, {13, 0.65, false}, {14, 0.35, false}, {15, 0.35, true}, {99, 0.99, false}};
    // critical threads only (workers off): render first, then main
    auto t = ChooseTargets(th, 11, 10, 99, 0);
    CHECK(t.size() == 2 && t[0].tid == 11 && t[0].critical && t[1].tid == 10 && t[1].critical);
    // render == main collapses to one target
    t = ChooseTargets(th, 10, 10, 99, 0);
    CHECK(t.size() == 1 && t[0].tid == 10);
    // unknown render thread (0) and a thread id that does not exist are ignored
    t = ChooseTargets(th, 0, 10, 99, 0);
    CHECK(Ids(t) == std::vector<uint32_t>{10});
    t = ChooseTargets(th, 777, 10, 99, 0);
    CHECK(Ids(t) == std::vector<uint32_t>{10});
    // the calling thread is never chosen, even if it is the busiest or named as render/main
    t = ChooseTargets(th, 99, 99, 99, 3);
    for (const Target& x : t) CHECK(x.tid != 99);
    // workers: busiest first, hysteresis (14 at 35% does not enter, 15 at 35% already tuned stays), cap respected
    t = ChooseTargets(th, 10, 10, 99, 6);
    CHECK((Ids(t) == std::vector<uint32_t>{10, 11, 12, 13, 15}));
    for (size_t i = 1; i < t.size(); ++i) CHECK(!t[i].critical);
    t = ChooseTargets(th, 10, 10, 99, 2);
    CHECK((Ids(t) == std::vector<uint32_t>{10, 11, 12}));   // main + 2 workers
    // a critical thread is not repeated as a worker
    t = ChooseTargets(th, 11, 10, 99, 6);
    CHECK((Ids(t) == std::vector<uint32_t>{11, 10, 12, 13, 15}));
    // equal loads: ordered by thread id, so the result is deterministic
    const std::vector<ThreadView> eq = {{30, 0.8, false}, {20, 0.8, false}, {25, 0.8, false}};
    t = ChooseTargets(eq, 0, 0, 1, 2);
    CHECK((Ids(t) == std::vector<uint32_t>{20, 25}));
    // nothing known: nothing chosen
    CHECK(ChooseTargets({}, 5, 6, 7, 3).empty());
}

static void TestAffinityPolicy() {
    CHECK(ShouldWidenAffinity(0x01, 0xFF));          // pinned to one CPU of eight
    CHECK(ShouldWidenAffinity(0x0F, 0xFF));          // half the CPUs
    CHECK(ShouldWidenAffinity(0xFE, 0xFF));          // all but CPU 0
    CHECK(!ShouldWidenAffinity(0xFF, 0xFF));         // already everything
    CHECK(!ShouldWidenAffinity(0, 0xFF));            // empty mask: not ours to touch
    CHECK(!ShouldWidenAffinity(0x01, 0));            // nothing to widen to
    CHECK(!ShouldWidenAffinity(0x1FF, 0xFF));        // has a CPU outside the target
    CHECK(!ShouldWidenAffinity(0x100, 0xFF));        // entirely outside the target
    CHECK(ShouldWidenAffinity(0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull));  // 64-bit masks
    CHECK(CountCpus(0) == 0 && CountCpus(0xFF) == 8 && CountCpus(0xFE) == 7 && CountCpus(0xFFFFFFFFFFFFFFFFull) == 64);
}

static std::string GuidText(const GuidBytes& g) {
    char b[64];
    std::snprintf(b, sizeof(b), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.d1, g.d2, g.d3, g.d4[0], g.d4[1], g.d4[2], g.d4[3], g.d4[4], g.d4[5], g.d4[6], g.d4[7]);
    return b;
}

static void TestPowerGuids() {
    static_assert(sizeof(GuidBytes) == 16, "must have the layout of a GUID");
    // strings as published by Microsoft Learn (powercfg / "Processor power management options")
    CHECK(GuidText(kGuidProcessorSubgroup) == "54533251-82be-4824-96c1-47b60b740d00");
    CHECK(GuidText(kGuidMinProcessorState) == "893dee8e-2bef-41e0-89c6-b55d0929964c");
    CHECK(GuidText(kGuidMaxProcessorState) == "bc5038f7-23e0-4960-96da-33abaf5935ec");
    CHECK(GuidText(kGuidCoreParkingMinCores) == "0cc5b647-c1df-4637-891a-dec35c318583");
    CHECK(GuidText(kGuidPerfBoostMode) == "be337238-0d82-4146-a960-4f3749d470c7");
}

static void TestBottleneck() {
    FrameStats s;
    CHECK(ClassifyBottleneck(s, true, 1.0) == Bound::Unknown);
    s.valid = true;
    s.frameMsAvg = 10.0;
    s.presentMsAvg = 7.0;
    CHECK(ClassifyBottleneck(s, true, 0.99) == Bound::GpuOrVsync);  // waiting in Present wins
    s.presentMsAvg = 0.5;
    CHECK(ClassifyBottleneck(s, true, 0.95) == Bound::Cpu);
    CHECK(ClassifyBottleneck(s, true, 0.40) == Bound::Neither);
    CHECK(ClassifyBottleneck(s, false, 0.99) == Bound::Neither);    // no thread data: don't claim CPU
}

int main() {
    TestHotkeys();
    TestFrameClockBasics();
    TestFrameClockSpikesAndLows();
    TestFrameClockPauseAndSecondarySurface();
    TestFrameClockFewFramesAndStall();
    TestFrameClockRingWrap();
    TestFrameClockConcurrent();
    TestFrameClockCounters();
    TestTunerPolicy();
    TestAffinityPolicy();
    TestPowerGuids();
    TestBottleneck();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
