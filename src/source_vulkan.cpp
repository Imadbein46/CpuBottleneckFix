// Adapter: Vulkan. Games reach vkQueuePresentKHR two ways: through the loader's exported function, or through
// a pointer from vkGetDeviceProcAddr / vkGetInstanceProcAddr that bypasses it. We hook the exported function
// (A) and both GetProcAddr functions. The GetProcAddr hooks only RECORD the address they hand out; the real
// hook on that address (B) is installed later from the worker thread, never inside the game's call.
// No Vulkan headers needed: only the shapes of four functions.
#include <atomic>
#include <cstdint>
#include <cstring>

#include "log.h"
#include "sources.h"

namespace cbf {
namespace sources {
namespace {

using VkResult = int32_t;
using VkQueue = void*;
using VkDevice = void*;
using VkInstance = void*;
using PFN_vkVoidFunction = void(__stdcall*)();
using PFN_vkQueuePresentKHR = VkResult(__stdcall*)(VkQueue, const void*);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(__stdcall*)(VkDevice, const char*);
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(__stdcall*)(VkInstance, const char*);

void* g_origPresentA = nullptr;  // hook on vulkan-1!vkQueuePresentKHR
void* g_origPresentB = nullptr;  // hook on the address the driver / layers handed out
void* g_origGetDevice = nullptr;
void* g_origGetInstance = nullptr;
std::atomic<void*> g_exportAddr{nullptr};
std::atomic<void*> g_driverAddr{nullptr};
std::atomic<void*> g_pending{nullptr};

VkResult __stdcall PresentA(VkQueue queue, const void* info) {
    PresentScope scope(reinterpret_cast<uintptr_t>(queue), "Vulkan");
    return reinterpret_cast<PFN_vkQueuePresentKHR>(g_origPresentA)(queue, info);
}
VkResult __stdcall PresentB(VkQueue queue, const void* info) {
    PresentScope scope(reinterpret_cast<uintptr_t>(queue), "Vulkan");
    return reinterpret_cast<PFN_vkQueuePresentKHR>(g_origPresentB)(queue, info);
}

void NoteTarget(PFN_vkVoidFunction fn) {
    void* p = reinterpret_cast<void*>(fn);
    if (!p || p == g_exportAddr.load() || g_driverAddr.load() != nullptr) return;  // already covered, or B already placed
    g_pending.store(p);
}

PFN_vkVoidFunction __stdcall GetDeviceProcAddrHook(VkDevice device, const char* name) {
    PFN_vkVoidFunction r = reinterpret_cast<PFN_vkGetDeviceProcAddr>(g_origGetDevice)(device, name);
    if (r && name && std::strcmp(name, "vkQueuePresentKHR") == 0) NoteTarget(r);
    return r;
}
PFN_vkVoidFunction __stdcall GetInstanceProcAddrHook(VkInstance instance, const char* name) {
    PFN_vkVoidFunction r = reinterpret_cast<PFN_vkGetInstanceProcAddr>(g_origGetInstance)(instance, name);
    if (r && name && std::strcmp(name, "vkQueuePresentKHR") == 0) NoteTarget(r);
    return r;
}

bool Loaded() { return GetModuleHandleW(L"vulkan-1.dll") != nullptr; }

bool Attach() {
    HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk) return false;
    auto get = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(vk, n)); };

    bool any = false;
    void* present = get("vkQueuePresentKHR");
    if (present && InstallHook(present, reinterpret_cast<void*>(&PresentA), &g_origPresentA, "vulkan-1!vkQueuePresentKHR")) {
        g_exportAddr.store(present);
        any = true;
    }
    // Installed even if A failed: B may still catch pointers obtained via GetProcAddr.
    bool procs = false;
    procs |= InstallHook(get("vkGetDeviceProcAddr"), reinterpret_cast<void*>(&GetDeviceProcAddrHook), &g_origGetDevice, "vkGetDeviceProcAddr");
    procs |= InstallHook(get("vkGetInstanceProcAddr"), reinterpret_cast<void*>(&GetInstanceProcAddrHook), &g_origGetInstance, "vkGetInstanceProcAddr");
    return any || procs;
}

void PollTargets() {
    void* p = g_pending.exchange(nullptr);
    if (!p || g_driverAddr.load() != nullptr) return;
    if (InstallHook(p, reinterpret_cast<void*>(&PresentB), &g_origPresentB, "vkQueuePresentKHR (driver/layer address)")) g_driverAddr.store(p);
}

}  // namespace

const Adapter kVulkan = {"Vulkan", &Loaded, &Attach, &PollTargets};

}  // namespace sources
}  // namespace cbf
