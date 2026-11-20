#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace cbf {
namespace log {
namespace {
CRITICAL_SECTION g_cs;
bool g_csReady = false;
HANDLE g_file = INVALID_HANDLE_VALUE;
DWORD g_t0 = 0;
}  // namespace

void Init(const wchar_t* path, bool enabled) {
    if (!enabled || g_csReady) return;
    InitializeCriticalSection(&g_cs);
    g_csReady = true;
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_t0 = GetTickCount();
}

void Write(const char* fmt, ...) {
    if (!g_csReady || g_file == INVALID_HANDLE_VALUE) return;
    char line[640];
    const DWORD dt = GetTickCount() - g_t0;
    int n = std::snprintf(line, sizeof(line), "[%6lu.%03lu] ", static_cast<unsigned long>(dt / 1000), static_cast<unsigned long>(dt % 1000));
    if (n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(line + n, sizeof(line) - static_cast<size_t>(n) - 3, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    size_t len = static_cast<size_t>(n) + static_cast<size_t>(m);
    if (len > sizeof(line) - 3) len = sizeof(line) - 3;  // truncated by snprintf
    line[len++] = '\r';
    line[len++] = '\n';
    EnterCriticalSection(&g_cs);
    DWORD written = 0;
    WriteFile(g_file, line, static_cast<DWORD>(len), &written, nullptr);
    LeaveCriticalSection(&g_cs);
}

void Shutdown() {
    if (!g_csReady) return;
    EnterCriticalSection(&g_cs);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_cs);
    // The critical section is intentionally not deleted: a late Write() must stay harmless.
}

}  // namespace log
}  // namespace cbf
