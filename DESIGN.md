# Design notes

The structure follows the vocabulary of the *improve-codebase-architecture* skill (deep **modules** with small
**interfaces**, explicit **seams**, **adapters**, "the interface is the test surface"). There was no existing
codebase to review, so the skill was applied while designing rather than as a refactoring pass.

| Module | Interface (what callers see) | What hides behind it | Test surface |
|---|---|---|---|
| `FrameClock` | `Begin`, `End`, `Snapshot` | lock-free ring buffer, primary-surface selection, pause handling, 1 s / 5 s windows, percentile maths | native unit tests (ASan/UBSan/TSan) |
| `ParseHotkey` | one function | key-name table, modifier handling, validation | native unit tests |
| `ClassifyBottleneck` | one pure function | heuristic thresholds | native unit tests |
| `CpuTuner` | `Init`, `SetEnabled`, `Tick`, `GetMetrics` | CPU topology, hybrid-core ranking, per-thread load sampling, hysteresis, reversible changes | `CpuTuner` logs every action (`Log=1`) |
| `Overlay` | `Create`, `Update`, `Destroy` | layered click-through window, DPI, positioning, focus rules | visual |
| `sources` (**seam**) | `Init`, `Poll`, `PresentScope`, `Adapter` | MinHook, TLS nesting guard, last-error preservation, retry policy | log output |
| adapters `D3D8/D3D9/DXGI/OpenGL/Vulkan` | one `Adapter` struct each | dummy-device vtable discovery, per-API quirks | log output |
| `plugin.cpp` | `InitializeASI`, `DllMain` | init-once, worker thread, hotkeys, overlay text | - |

**Seam / adapters.** "A frame was presented" is the seam. Five adapters sit behind it (a seam with one adapter is
hypothetical, with two or more it is real), so nothing else in the plugin knows which graphics API is in use.

**Deletion test.** Deleting `FrameClock` would spread ring-buffer, window and percentile logic into the
overlay, the log writer and the bottleneck hint: it earns its place. `Overlay` has a deliberately thin interface
because everything it does (window class, DPI, focus rules, positioning) is hidden.

**Why the overlay is a separate window.** Drawing inside the game's swap chain would need a renderer per API and
risks corrupting game state. A click-through top-level window works identically for every API; the cost is that
it cannot appear over true exclusive fullscreen.
