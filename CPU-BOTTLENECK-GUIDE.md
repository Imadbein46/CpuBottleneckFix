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

## Using all CPU cores

A game uses as many threads as its engine creates; no tool can add parallelism to code that is not parallel.
What *can* hold a game back from your cores is outside the game:

* **Restricted affinity.** Task Manager -> Details -> right-click the game -> *Set affinity*. If CPUs are unticked,
  something restricted it. The plugin's `UseAllCpus` lifts that while the CPU fix is on (your first recorded game
  was limited to 7 of 8 CPUs; the others already had all 8).
* **The power plan.** With `Log=1` the `system:` lines show the plan's limits. A *maximum processor state* below
  100 % caps the CPU, *core parking* below 100 % lets Windows put cores to sleep, and *boost mode* disabled turns
  turbo off. To fix a plan (administrator Command Prompt; plugged-in values; the GUIDs are Microsoft's):

  ```
  powercfg /setacvalueindex SCHEME_CURRENT 54533251-82be-4824-96c1-47b60b740d00 bc5038f7-23e0-4960-96da-33abaf5935ec 100
  powercfg /setacvalueindex SCHEME_CURRENT 54533251-82be-4824-96c1-47b60b740d00 0cc5b647-c1df-4637-891a-dec35c318583 100
  powercfg /setactive SCHEME_CURRENT
  ```
  The first line sets *maximum processor state* to 100 %, the second keeps all cores awake. The plugin never
  changes your power plan itself, because that is a persistent system-wide change.
* **Hyper-threading / SMT.** Your i7-9700K has none (8 cores, 8 threads), so there are no extra threads to unlock.
* **Thread pools sized at start-up.** Many engines size their worker pools from the CPU count they see when
  they launch, so a restriction that was lifted later does not grow the pool; start the game with the full
  affinity if you changed it.

In the recordings the limiting thread was one game thread at about 100 % while the process used 2-5 cores' worth
of work: the other cores were not forbidden, the game had no more parallel work to give them.

## Syncing the CPU with the GPU

"Sync" can mean three different things, and only one of them is a performance question:

1. **Who waits for whom.** Present wait is the CPU blocked on the GPU or vsync. In your three recordings it was
   almost zero in 66-94 % of the samples: the GPU was waiting for the CPU. Nothing in the sync settings can fix
   that; only the single busiest thread getting faster can.
2. **How far ahead the CPU may run.** The render queue (Direct3D 11: maximum frame latency, default 3). A shorter
   queue means lower input latency and less run-ahead, and a few percent less FPS in GPU-bound scenes. The plugin
   can cap it (`[Sync]`, `Ctrl+F11`).
3. **How the frame reaches the display.** vsync on/off, tearing allowed, flip or blit. With variable refresh
   rate (G-Sync / FreeSync) the best-known recipe is: vsync on in the driver, the game's own limiter or a cap
   about 3 fps below the refresh rate, and the low-latency mode of the driver (NVIDIA Reflex / Low Latency) on.
   That keeps the queue empty without wasting frames.

For a game that is CPU-bound, as all three of your recordings were, items 2 and 3 change feel (latency,
pacing), not the CPU limit.

## Frame generation and the overlay

With DLSS Frame Generation (thread names such as `nv.sl.dlss_g.thread.dlssg`) or FSR 3, the FPS the overlay shows
is what reaches the screen, including generated frames. The game's own update rate is lower (half, with 2x). Frame
generation improves smoothness, it does not remove a CPU limit: if the busiest thread is still at 95-100 %, the
game logic is still CPU-bound.

## Windows 11 settings worth checking

* *Settings -> Privacy & security -> Windows Security -> Device security -> Core isolation -> Memory integrity*.
  On some systems it costs a few percent of CPU performance in games. Turning it off lowers a security feature,
  so decide for yourself. With `Log=1` the log's `system:` lines tell you whether it is on.
* *Settings -> System -> Power -> Power mode = Best performance* (and the High performance plan on desktops).
* *Settings -> Gaming -> Game Mode = On*.

## Case studies (two logs, Windows 11)

| Game | Limiting thread | Evidence | Reading |
|---|---|---|---|
| Assassin's Creed IV (32-bit, D3D11) | `[AC4BFSP.exe] render`, 93-100 %, 3 % kernel | whole process only ~35 % CPU | game's own draw-call submission on one thread; settings, driver or DXVK experiments |
| Cyberpunk 2077 (64-bit, D3D12) | `"GameThread" [Cyberpunk2077.exe]`, 91-100 %, 2 % kernel | 97 % of samples CPU-bound; process CPU 18 % for minutes at a time | game logic on the main thread; settings, mods, CPU speed |

Cyberpunk 2077 again, with system info (Intel Core i7-9700K, 8 cores / 8 threads, 2018; Windows 11 24H2): `GameThread` at 90-100 % in 19 of 22 limit reports, steady gameplay ~99 displayed fps (DLSS Frame Generation was active, so the game itself ran at roughly half that), 173 of 218 samples CPU-bound. The first minute is shader compilation: all eight cores at 93-100 % and a 4-second hitch, which is normal.

In all runs the CPU fix toggle made no clear difference, as expected for a thread limited by its own work.

## The one thing the logs point at: other tools on the render thread

In both newer recordings the swap-chain code the plugin found was not Windows' own: it lived in a module named
`d3d11.dll` (Assassin's Creed IV) and `WINHTTP.dll` (Cyberpunk), i.e. another injected tool or wrapper under a
proxy name, and Cyberpunk also loads RivaTuner's hook DLL. Such tools run inside every Present call on the thread
that presents. When that thread is at 100 %, they are part of what it is busy with. Test, one change at a time,
in the same scene: close RivaTuner/MSI Afterburner OSD, and temporarily rename the injector DLL (or disable the
ReShade / OptiScaler-style add-on), then compare FPS and the *busiest thread* percentage. This is a test, not a
promise: if the percentage and FPS do not change, that tool is not your cost.

## How to compare fairly

Use the overlay in the same slow scene, one change at a time, and note FPS and the *Limit:* line. If *Limit*
changes from "CPU" to "GPU or VSync" you removed the CPU bottleneck. The CPU fix hotkey (Ctrl+F10) lets you
compare the plugin's own effect instantly.
