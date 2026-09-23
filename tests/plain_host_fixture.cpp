#include "logos_module_impl.h"
#include "logos_protocol.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

constexpr std::size_t kAllocationSize = 16384;
std::atomic<bool> initialized{false};

// Memory the host must return through logos_module_string_free, never free().
char* allocateMapped()
{
#ifdef _WIN32
    return static_cast<char*>(VirtualAlloc(nullptr, kAllocationSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
    void* mapped = mmap(nullptr, kAllocationSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    return mapped == MAP_FAILED ? nullptr : static_cast<char*>(mapped);
#endif
}

void freeMapped(char* value)
{
#ifdef _WIN32
    VirtualFree(value, 0, MEM_RELEASE);
#else
    munmap(value, kAllocationSize);
#endif
}

char* copyResult(const char* value)
{
    char* result = nullptr;
    if (std::getenv("LOGOS_TEST_CUSTOM_ALLOCATOR"))
        result = allocateMapped();
    else
        result = static_cast<char*>(std::malloc(std::strlen(value) + 1));
    if (result) std::strcpy(result, value);
    return result;
}

} // namespace

extern "C" {

char* logos_module_dispatch(const char* method, const char*)
{
    if (std::strcmp(method, "name") == 0)
        return copyResult("\"plain_host_fixture\"");
    if (std::strcmp(method, "ready") == 0)
        return copyResult(initialized ? "true" : "false");
    if (std::strcmp(method, "thread") == 0) {
        std::ostringstream id;
        id << '"' << std::this_thread::get_id() << '"';
        return copyResult(id.str().c_str());
    }
    if (std::strcmp(method, "slow") == 0) {
        std::puts("SLOW_ENTERED");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
    }
    return copyResult("\"ok\"");
}

char* logos_module_get_methods() { return copyResult("[]"); }

void logos_module_set_context(const char*, const char*, const char*)
{
    if (std::getenv("LOGOS_TEST_DELAY_INIT")) {
        std::puts("INIT_ENTERED");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }
    initialized = true;
}

void logos_module_set_emit_callback(logos_module_emit_cb, void*) {}
int logos_module_accept_token(const char*, const char*) { return 0; }
int logos_module_accept_inbound_token(const char*, const char*) { return 0; }
int logos_module_grant_host_services(const char*) { return 0; }
void logos_module_set_unload_done_callback(logos_module_unload_done_cb, void*) {}
int logos_module_about_to_unload()
{
    std::puts("ABOUT_TO_UNLOAD");
    std::fflush(stdout);
    return 0;
}
void logos_module_set_call_caller(const char*) {}
const char* logos_module_get_protocol_version() { return LOGOS_PROTOCOL_VERSION_STRING; }

void logos_module_string_free(char* value)
{
    if (!value) return;
    if (std::getenv("LOGOS_TEST_CUSTOM_ALLOCATOR"))
        freeMapped(value);
    else
        std::free(value);
}

} // extern "C"
