# CpuBottleneckFix

An ASI plugin for [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) that

* **helps games that are limited by one busy CPU thread** (the "CPU fix", toggle with a hotkey), and
* shows an **FPS / latency overlay** (toggle with a hotkey) that also tells you whether the CPU or the GPU is the limit.

Works with Direct3D 8, 9, 10, 11, 12, OpenGL and Vulkan games. 32-bit and 64-bit builds included.

## Install

1. Install Ultimate ASI Loader in the game folder (any of its supported DLL names, e.g. `dinput8.dll`).
2. Copy the build **that matches the game's bitness** into the game folder (or its `scripts` / `plugins` folder):
   `x86\CpuBottleneckFix.asi` for 32-bit games, `x64\CpuBottleneckFix.asi` for 64-bit games.
3. Copy `CpuBottleneckFix.ini` next to it. Start the game.

Hotkeys (work only while the game window has the focus; change them in the ini):

| Key | Action |
|---|---|
| `Ctrl+F9` | overlay on / off |
| `Ctrl+F10` | CPU fix on / off (a short message confirms it, even when the overlay is hidden) |

Uninstall: delete the `.asi` and `.ini`.

## What the overlay shows

```
FPS: 143   1% low: 98
Latency: 6.98 ms (max 9.4)
Present wait: 1.20 ms
CPU: 31%  busiest thread: 96%
Limit: CPU (one thread maxed)
API: DXGI (D3D10/11/12)
CPU fix: ON  (3 threads tuned)
```

* **Latency** is *frame time*: the time between two presented frames (and the worst frame of the last second).
  It is **not** input-to-photon latency; that cannot be measured by software alone.
* **Present wait** is how long the game sits inside its Present call, waiting for the GPU or vsync.
* **Limit** is a heuristic: lots of Present wait means GPU / vsync-bound; a thread near 100 % of one core with
  little Present wait means CPU-bound. Treat it as a hint.

## What the CPU fix does (and does not)

While it is ON the plugin, all of it reversible (toggle OFF restores the original values):

1. asks Windows for a finer timer (0.5 ms) so sleeps and waits are accurate, which steadies frame pacing;
2. opts the game out of power throttling (EcoQoS);
3. raises the process priority class to *Above normal* (never lowers anything, never uses Realtime);
4. gives the thread that calls Present and the process's main thread a slightly higher priority and their own
   physical core as *ideal processor* (a hint, not a hard pin). On CPUs with fast and slow cores it prefers the
   fast ones; it avoids core 0. Threads the game set to a non-default priority are left alone, and background
   worker threads are never given a priority boost;
5. registers the presenting thread with the Windows multimedia scheduler ("Games" task).

**Honest expectations:** this improves *scheduling*. It cannot make a game's own code faster, so if a game is
bound by its own single-threaded logic the gain is small. Gains are largest on systems with background load,
power-saving settings or hybrid cores, and sometimes there is no visible change. Use the hotkey and the overlay
to compare ON and OFF on your system.

## What changed in 1.1 and 1.2 (driven by real-world logs)

* **1.2** The "render thread" is only tuned if the same thread has been presenting frames for 5 s. Engines with a
  job system (e.g. Cyberpunk 2077) and frame generation present from a different thread every frame; v1.1 kept
  re-tuning a random worker thread there.
* **1.2** When the overlay is hidden and `Log=0` the plugin no longer measures every thread: it keeps the two
  critical threads tuned and rescans once a minute. (v1.1 measured everything once a second, about 3 % of one
  core, mostly kernel time spent walking the system thread list.) With the overlay or log on, sampling is now
  every 2 s by default.
* **1.2** With `Log=1` the log starts with a system summary: CPU model, RAM, Windows build, power plan, Game
  Mode, memory integrity (HVCI) and virtualization-based security. It stays in the local log file.
* **1.2** The overlay notes when a frame-generation module (DLSS-G, FSR 3) is loaded, because the shown FPS
  then includes generated frames.
* **1.1** Only the presenting thread and the main thread are tuned (`HotThreads` became `TuneWorkerThreads`,
  default 0). `LIMIT:` and `HITCH:` log lines name the busiest threads with their name, module and kernel share.
  The 5-second stats line no longer repeats stale numbers while the game is paused.

See `CPU-BOTTLENECK-GUIDE.md` for how to read these lines and what to do about them.

## Limits you should know about

* **Do not use with games that have anti-cheat** (EasyAntiCheat, BattlEye, Vanguard, VAC-secured servers, ...).
  Hooking and ASI loaders can get you flagged or banned. Single-player use only.
* The overlay is a separate click-through window on top of the game. It appears in windowed, borderless and
  most modern fullscreen modes, but **not over true exclusive fullscreen** (some older D3D9 / D3D8 / OpenGL games).
  The FPS numbers are still collected and appear in `CpuBottleneckFix.log` if you set `Log=1`.
* **Frame generation** (DLSS-G / FSR 3 / Lossless Scaling style) makes the overlay show *displayed* frames, which
  can be 2-4x the rate at which the game itself runs. The *Limit* hint is unreliable while it is active.
* DirectDraw and software-rendered games are not supported (no frame counter). The CPU fix still works for them.
* Games that already pin their own threads or set their own timer/priority may leave less for the fix to do.
* The plugin creates a throwaway D3D device once at start-up to find the present functions (the same technique as
  the Ultimate ASI Loader `FrameLimiter` demo). If a game is *already* running in exclusive fullscreen when the
  plugin starts, that can occasionally disturb it; start the plugin with the game, as the loader normally does.

## Testing status (please read)

Verified: compiles without warnings for x86 and x64 (GCC 13 mingw-w64, `-Wall -Wextra -Wshadow -Wformat=2`);
`cppcheck` clean; 173 unit checks for the frame-timing, hotkey and classifier logic pass under AddressSanitizer,
UndefinedBehaviorSanitizer and ThreadSanitizer; binaries import only Windows system DLLs and export exactly
`InitializeASI`.

**Not verified:** a run on real Windows with a real game. The build environment had no Windows and Wine could
not start there. The hook code follows the loader's own `FrameLimiter` demo, but the first run on your machine is
the real test. If anything misbehaves, set `Log=1` and send `CpuBottleneckFix.log`.
`tests/wine/harness.cpp` is a small fake game (loads the `.asi`, presents frames through OpenGL or D3D9) you can
build and run to check a system quickly.

## Troubleshooting

* Overlay says *Waiting for frames... No graphics API detected*: the game uses an API that is not hooked (see
  limits) or hasn't created its device yet.
* No overlay in fullscreen: switch the game to borderless / windowed.
* Hotkeys do nothing: another program may own that combination; change it in the ini. They only work while the
  game window has the focus.
* Set `Log=1` in the ini: the log lists every hook installed, every thread tuned and one stats line every 5 s.

## Build

Needs mingw-w64 (`g++-mingw-w64-i686-win32`, `g++-mingw-w64-x86-64-win32`).

```
make          # build/x86/CpuBottleneckFix.asi and build/x64/CpuBottleneckFix.asi
make test     # native unit tests
```

Third-party: [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause, in `third_party/minhook`).
See `DESIGN.md` for the module layout.
