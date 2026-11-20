// Native unit tests for the portable modules: hotkey parsing, FrameClock, bottleneck classifier.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "../src/bottleneck.h"
#include "../src/frame_clock.h"
#include "../src/hotkey.h"

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
    TestBottleneck();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
