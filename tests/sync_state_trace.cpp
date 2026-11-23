// Faithful model of the SyncHook / ApplyLatency state machine (same variables and conditions as source_dxgi.cpp),
// driven by a fake device, to trace the scenarios that cannot be run without Windows.
#include <atomic>
#include <cstdio>
struct FakeDevice { unsigned latency; bool dxgiDevice; int sets = 0, gets = 0; };
static std::atomic<int> g_wantLatency{0}, g_latApplied{0};
static std::atomic<void*> g_latSc{nullptr};
static unsigned g_origLatency = 0; static bool g_origKnown = false;
static int g_devLatency = -1;
static void ApplyLatency(FakeDevice* d, void* sc, int want) {
    if (d->dxgiDevice) {
        unsigned cur = d->latency; ++d->gets;
        if (want > 0) { if (!g_origKnown) { g_origLatency = cur; g_origKnown = true; } if (cur != (unsigned)want) { d->latency = want; ++d->sets; } }
        else if (g_origKnown) { if (cur != g_origLatency) { d->latency = g_origLatency; ++d->sets; } g_origKnown = false; }
        g_devLatency = want > 0 ? want : (int)(g_origLatency ? g_origLatency : cur);
    }
    g_latSc.store(sc); g_latApplied.store(want);
}
static int applies = 0;
static void Present(FakeDevice* d, void* sc) {
    int want = g_wantLatency.load();
    if (want != g_latApplied.load() || (want > 0 && sc != g_latSc.load())) { ++applies; ApplyLatency(d, sc, want); }
}
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main() {
    FakeDevice d11{3, true}; int sc1;
    for (int i = 0; i < 100; ++i) Present(&d11, &sc1);
    EXPECT(applies == 0 && d11.latency == 3 && d11.sets == 0);                 // default: no calls at all
    g_wantLatency = 1;
    for (int i = 0; i < 100; ++i) Present(&d11, &sc1);
    EXPECT(applies == 1 && d11.latency == 1 && d11.sets == 1 && g_devLatency == 1);   // applied once, not per frame
    g_wantLatency = 0;
    for (int i = 0; i < 100; ++i) Present(&d11, &sc1);
    EXPECT(applies == 2 && d11.latency == 3 && g_devLatency == 3 && !g_origKnown);    // exact original restored
    FakeDevice d12{0, false}; int sc2; applies = 0; g_wantLatency = 1;
    for (int i = 0; i < 100; ++i) Present(&d12, &sc2);
    EXPECT(applies == 1 && d12.sets == 0);                                      // D3D12: nothing touched, no retry storm
    g_wantLatency = 0; g_latApplied = 0; g_latSc = nullptr; g_origKnown = false;
    FakeDevice game1{1, true}; applies = 0; g_wantLatency = 1;                 // game already uses 1
    for (int i = 0; i < 10; ++i) Present(&game1, &sc1);
    g_wantLatency = 0; for (int i = 0; i < 10; ++i) Present(&game1, &sc1);
    EXPECT(game1.latency == 1 && game1.sets == 0);                              // never changed a value it did not need to
    std::printf("sync state machine: all traced scenarios behave as intended\n");
    return 0;
}
