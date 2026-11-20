// Tiny thread-safe file logger. Never called from the Present hooks (hot path).
#pragma once

namespace cbf {
namespace log {

void Init(const wchar_t* path, bool enabled);  // truncates the file; no-op if !enabled
void Write(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void Shutdown();

}  // namespace log
}  // namespace cbf
