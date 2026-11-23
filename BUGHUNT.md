# Bug hunt (v1.6.1 -> v1.6.2)

## Method

The linked tool, `elementalsouls/Claude-BugHunter`, is a bug-bounty / red-team skill bundle: 83 skills for
hunting *web* vulnerabilities (XSS, SQLi, IDOR, SSRF, OAuth, enterprise identity platforms). It targets
internet-facing attack surfaces. CpuBottleneckFix is a local native Windows DLL with no web surface, no HTTP, no
auth flow — none of that bundle's skills apply to it. Using it here would not have found anything real.

Instead, this pass used tools and techniques suited to native C++: clang-tidy (`bugprone-*`, `cert-*`,
`concurrency-*`, `misc-*`, `performance-*`, `portability-*`, plus the Clang Static Analyzer's dataflow checks:
null dereference, double free, use of uninitialised values, dead stores, new/delete mismatches), cppcheck, the
existing sanitizer-backed unit tests, and a manual audit of every "remember the original value, restore it
later" pattern in the codebase, since that is the pattern class most likely to hide an ordering bug as a project
grows across many releases.

## What the tools found

clang-tidy and the Clang Static Analyzer, run over every `.cpp` file with a corrected mingw-w64 sysroot (the
default invocation silently failed to find `windows.h`/`d3d11.h` for 10 of 14 files, which would have made the
earlier "cppcheck clean" claims only ever cover the 4 files without Windows headers, hotkey.cpp, frame_clock.cpp,
tuner_policy.cpp, and partially cpu_tuner.cpp; that gap is now closed and documented), produced no findings that
indicate a real defect: the `bugprone-casting-through-void` warnings are the standard double-cast idiom for
`GetProcAddress` results and are intentional; the dataflow checks (null dereference, uninitialised reads, double
free, dead stores) found nothing in any file.

## What manual review found

**Bug: thread-affinity restore could restore the wrong mask (`cpu_tuner.cpp`, `WidenThreadAffinity=1` only).**

When `WidenThreadAffinity` is on, a thread pinned to fewer CPUs than the process is widened, and its original
mask is saved to restore when the CPU fix is switched off. The save happened on *every* sample where the branch
was taken, not only the first time. If the thread's affinity changed again afterwards — to another mask still
narrower than the process's (the game re-pinning it, or anything else touching it) — the code re-entered the
same branch and overwrote the saved value with that intermediate mask. The CPU fix would then restore the wrong
affinity on shutdown, silently breaking the "restores the original values" guarantee for that one thread.

Not currently triggered in the field: `WidenThreadAffinity` defaults to 0 and none of the logs seen so far had
it enabled. It is a real, reachable bug in an opt-in feature, not a hypothetical one — reproduced in an isolated
model before touching the real code, fixed with a one-line guard (keep the first saved value, never overwrite
it), and the fix verified against the original repro plus two regression cases. A permanent regression test for
this exact scenario is now in `tests/affinity_state_trace.cpp`.

**Latent, non-triggering risk noted for the record:** `CpuTuner::RefreshAffinityState()` rebuilds the core list
and clears the record of which cores are claimed by tuned threads. It is only ever called at start-up or at the
two points in `SetEnabled()` where threads have already been fully untuned first, so there is no live bug today.
But that safety depends on the order of two calls inside `SetEnabled()`, not on anything the type system
enforces — a future change that reordered them would reintroduce the same class of bug. Left as a comment in the
code rather than refactored, since refactoring it would be a larger change than this pass's scope.

## Verified after the fix

Same checks as every prior release, plus the clang-tidy/analyzer pass above: vtable slots verified against the
headers, unit tests (213 now, the new regression case) pass under ASan/UBSan/TSan, strict `-Wall -Wextra -Wshadow
-Wformat=2 -Wcast-align` on x86/x64 at -O0/-O3 is clean, cppcheck is clean, the sync and affinity state machines
re-verified in their native models, both binaries export exactly `InitializeASI` and import only Windows system
DLLs. Not verified: this fix has not run on real Windows, because `WidenThreadAffinity` has never been turned on
in a session that sent back a log.
