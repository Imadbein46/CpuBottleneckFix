// One-time system summary for the log (CPU model, RAM, Windows build, power plan, security features that cost
// CPU time). Only written when Log=1; nothing is sent anywhere.
#pragma once

namespace cbf {
void LogSystemInfo();
// Names of the DLLs loaded in the process (to spot overlays, wrappers, second copies of graphics DLLs).
void LogLoadedModules();
}
