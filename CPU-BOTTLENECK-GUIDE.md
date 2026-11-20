# Removing a CPU bottleneck: what a plugin can and cannot do

## What your first log showed

| Situation | FPS | Present wait | Busiest thread | Whole CPU | Meaning |
|---|---|---|---|---|---|
| Most of the session | 157.7 (6.34 ms, flat) | ~5 ms of 6.34 | 40-58 % | 14-23 % | **Not CPU-bound.** The game waits in Present: a frame cap / vsync / GPU limit. Healthy. |
| t = 70-90 s and 200-230 s | 77-93 | ~0.4 ms | **93-100 %** | 38-45 % | **CPU-bound by one thread.** The GPU waits for the CPU. |
| t = 95 s | 20 (worst frame 1602 ms) | - | two workers at 97 % | 40 % | A hitch: shader compilation or streaming. |

* CPU fix OFF at 85 s: 79.8 fps. ON at 90 s: 76.4 fps. **No measurable difference**, and that is expected: only
  ~40 % of the CPU was in use, so the pegged thread already had a core to itself. Priorities and core placement
  only help a thread that has to *compete* for CPU time. A thread limited by its own work can only be helped by
  doing less work or by a faster core.
* The process was limited to **7 logical CPUs, no hyper-threading**. That is unusual. Check Task Manager ->
  Details -> right-click the game -> *Set affinity*: if some CPUs are unticked, something (a launcher, Process
  Lasso, a shortcut with `/affinity`) restricted it. Ignore this if you did it on purpose.

## Step 1: find out what the limiting thread is

Set `Log=1`, play until the slow scene, and look for `LIMIT:` lines in `CpuBottleneckFix.log`:

```
LIMIT: a thread has been at 98% of a core for 3 s while the whole process uses 41% of the CPU | tid 1968 98% (kernel 9%) "GameThread" [game.exe] main; ...
```

| What the line says | Likely cause | What to try |
|---|---|---|
| Module is the **game .exe**, low kernel % | game logic / draw-call submission | step 2 (in-game settings), step 6 (hardware) |
| Module is a **GPU driver** (`nvwgf2umx.dll`, `atidxx64.dll`, `igd*`) or `d3d11/d3d12.dll` | driver / API overhead | step 3 (driver settings), step 4 (API choice) |
| **Kernel share 30 %+** | system calls, page faults, antivirus, memory pressure | step 5 (system) |
| `ntdll.dll` pool thread or a compiler/streaming thread, with `HITCH:` lines | shader compilation / streaming | step 7 (hitches) |

## Step 2: in-game settings that cost CPU, not GPU

Resolution, texture quality and upscalers (DLSS/FSR quality modes) barely change CPU load. These do: crowd /
NPC / traffic density, object and view distance (LOD), shadow quality and shadow distance, ray tracing (the
acceleration structure is built on the CPU), physics and simulation quality, number of dynamic lights, particle
and effects density. Change one at a time and compare FPS **in the same slow scene**.

**Frame generation** (DLSS-FG, FSR 3 FG) is the one feature that raises the *displayed* FPS when you are
CPU-bound. The game logic still runs at ~80, but you see about twice as many frames. It adds a little input latency.

## Step 3: driver and Windows settings

* NVIDIA Control Panel: *Threaded optimization* = On (or Auto), *Power management mode* = Prefer maximum
  performance, *Shader cache size* = Unlimited, *Low Latency Mode* = Off (it can throttle CPU run-ahead).
* Windows: *Game Mode* On, Power plan = High performance (or Ultimate), update chipset and GPU drivers.
  Test *Hardware-accelerated GPU scheduling* on and off.
* Close overlays and background apps that poll the game (extra overlays, RGB / monitoring suites, browsers).
  Your log shows only ~40 % total CPU use, so background load is **not** your main problem, but overlays that
  hook the render thread can still add cost.

## Step 4: API choice

If the game offers DirectX 11, DirectX 12 and/or Vulkan, test each. The overlay's last lines show which API is
active. Draw-call overhead differs a lot between a game's renderers; there is no universal winner.

## Step 5: the system

* BIOS: enable XMP / EXPO. Game logic is very sensitive to RAM speed and latency.
* Check clocks and temperature while the slow scene runs (HWiNFO, Task Manager -> Performance -> CPU speed).
  A core that throttles from heat or power limits looks exactly like a CPU bottleneck.
* Antivirus: exclude the game folder and the shader cache folder.
* Hybrid Intel CPUs: the game thread should be on a P-core (the plugin prefers them for its tuned threads).
  Dual-CCD AMD X3D CPUs: make sure Windows Game Bar / the AMD 3D V-Cache driver is working.

## Step 6: hardware

If the limiting thread is the game's own logic and nothing above changes it, the only real fix is a CPU with
higher single-thread performance (clock speed, IPC or large L3 cache such as AMD X3D) and faster RAM.
GPU upgrades do nothing for this symptom.

## Step 7: hitches (frame spikes of 100 ms to 1.6 s)

Let the game finish "compiling shaders" at start-up if it has that option, do not delete the driver shader
cache, keep the game on an SSD, keep some free RAM, and exclude the folders from the antivirus. Shader-compile
stutter often disappears after the first pass through an area. `HITCH:` lines show which threads were busy.

## How to compare fairly

Use the overlay in the same slow scene, one change at a time, and note FPS and the *Limit:* line. If *Limit*
changes from "CPU" to "GPU or VSync" you removed the CPU bottleneck. The CPU fix hotkey (Ctrl+F10) lets you
compare the plugin's own effect instantly.
