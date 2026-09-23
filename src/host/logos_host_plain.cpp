#include "command_line_parser.h"
#include "crash_handler.h"
#include "module_dll_search.h"
#include "module_path.h"
#include "parent_lifetime.h"
#include "token_source.h"
#include "transport_set_arg.h"

#include <logos_container/load_status.h>
#include <logos_module_impl.h>
#include <logos_protocol.h>
#include <logos_transport_config_json.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;

void reportLoadStatus(bool ok, std::string reason = {})
{
    for (char& c : reason)
        if (c == '\n' || c == '\r') c = ' ';
    if (ok)
        std::fprintf(stdout, "\n%s %s\n", LogosCore::kLoadStatusPrefix,
                     LogosCore::kLoadStatusOk);
    else
        std::fprintf(stdout, "\n%s %s %s\n", LogosCore::kLoadStatusPrefix,
                     LogosCore::kLoadStatusFailed, reason.c_str());
    std::fflush(stdout);
}

class DynamicLibrary {
public:
    ~DynamicLibrary() { close(); }

    bool open(const std::string& path, std::string& error)
    {
#ifdef _WIN32
        // The module's own directory resolves its imports, as in the Qt host.
        m_handle = LoadLibraryExW(std::filesystem::u8path(path).c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_handle) {
            error = "LoadLibrary failed with error " + std::to_string(GetLastError());
            return false;
        }
#else
        m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!m_handle) {
            const char* detail = dlerror();
            error = detail ? detail : "dlopen failed";
            return false;
        }
#endif
        return true;
    }

    void close()
    {
        if (!m_handle) return;
#ifdef _WIN32
        FreeLibrary(m_handle);
#else
        dlclose(m_handle);
#endif
        m_handle = nullptr;
    }

    template <typename T>
    T symbol(const char* name, std::string& error) const
    {
#ifdef _WIN32
        auto value = reinterpret_cast<T>(GetProcAddress(m_handle, name));
#else
        dlerror();
        auto value = reinterpret_cast<T>(dlsym(m_handle, name));
#endif
        if (!value && error.empty()) error = std::string("missing required symbol: ") + name;
        return value;
    }

private:
#ifdef _WIN32
    HMODULE m_handle = nullptr;
#else
    void* m_handle = nullptr;
#endif
};

struct ModuleAbi {
    decltype(&logos_module_dispatch) dispatch = nullptr;
    decltype(&logos_module_get_methods) getMethods = nullptr;
    decltype(&logos_module_set_context) setContext = nullptr;
    decltype(&logos_module_set_emit_callback) setEmitCallback = nullptr;
    decltype(&logos_module_accept_token) acceptToken = nullptr;
    decltype(&logos_module_accept_inbound_token) acceptInboundToken = nullptr;
    decltype(&logos_module_grant_host_services) grantHostServices = nullptr;
    decltype(&logos_module_set_unload_done_callback) setUnloadDoneCallback = nullptr;
    decltype(&logos_module_about_to_unload) aboutToUnload = nullptr;
    decltype(&logos_module_set_call_caller) setCallCaller = nullptr;
    decltype(&logos_module_get_protocol_version) protocolVersion = nullptr;
    decltype(&logos_module_string_free) stringFree = nullptr;

    bool resolve(const DynamicLibrary& library, std::string& error)
    {
#define RESOLVE(field, symbolName) field = library.symbol<decltype(field)>(symbolName, error)
        RESOLVE(dispatch, "logos_module_dispatch");
        RESOLVE(getMethods, "logos_module_get_methods");
        RESOLVE(setContext, "logos_module_set_context");
        RESOLVE(setEmitCallback, "logos_module_set_emit_callback");
        RESOLVE(acceptToken, "logos_module_accept_token");
        RESOLVE(acceptInboundToken, "logos_module_accept_inbound_token");
        RESOLVE(grantHostServices, "logos_module_grant_host_services");
        RESOLVE(setUnloadDoneCallback, "logos_module_set_unload_done_callback");
        RESOLVE(aboutToUnload, "logos_module_about_to_unload");
        RESOLVE(setCallCaller, "logos_module_set_call_caller");
        RESOLVE(protocolVersion, "logos_module_get_protocol_version");
        RESOLVE(stringFree, "logos_module_string_free");
#undef RESOLVE
        return error.empty();
    }
};

int protocolMajor(const char* version)
{
    if (!version || !*version) return -1;
    char* end = nullptr;
    const long value = std::strtol(version, &end, 10);
    return end != version && value >= 0 ? static_cast<int>(value) : -1;
}

std::string hostServicesJson(const std::string& csv)
{
    json services = json::array();
    std::size_t start = 0;
    while (start < csv.size()) {
        const std::size_t comma = csv.find(',', start);
        const std::size_t end = comma == std::string::npos ? csv.size() : comma;
        if (end > start) services.push_back(csv.substr(start, end - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return services.dump();
}

struct Runtime {
    ModuleAbi abi;
    std::string name;
    lp_provider* provider = nullptr;
    // Calls the transport runs at once; 1 is "single": arrival order, one thread.
    unsigned maxCalls = 1;
    std::mutex unloadMutex;
    std::condition_variable unloadChanged;
    bool unloadDone = false;
    // Calls in the module; once unloading, none enters.
    std::mutex callsMutex;
    std::condition_variable callsChanged;
    int activeCalls = 0;
    bool unloading = false;
};

char* copyForProtocol(Runtime& runtime, char* moduleText)
{
    if (!moduleText) return nullptr;
    char* copy = lp_string_copy(moduleText);
    runtime.abi.stringFree(moduleText);
    return copy;
}

char* dispatch(const char* method, const char* args, void* userData)
{
    auto& runtime = *static_cast<Runtime*>(userData);
    {
        std::lock_guard<std::mutex> lock(runtime.callsMutex);
        if (runtime.unloading) {
            const json refused{{"code", "dispatch_failed"}, {"message", "module is unloading"},
                               {"origin", runtime.name}};
            return lp_string_copy(refused.dump().c_str());
        }
        ++runtime.activeCalls;
    }
    runtime.abi.setCallCaller(lp_current_caller_json());
    char* result = copyForProtocol(runtime, runtime.abi.dispatch(method, args));
    runtime.abi.setCallCaller(nullptr);
    {
        std::lock_guard<std::mutex> lock(runtime.callsMutex);
        --runtime.activeCalls;
    }
    runtime.callsChanged.notify_all();
    return result;
}

// Refuses new calls and waits for those in the module, so none runs once
// aboutToUnload has been called.
void stopCalls(Runtime& runtime, std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(runtime.callsMutex);
    runtime.unloading = true;
    runtime.callsChanged.wait_until(lock, deadline, [&] { return runtime.activeCalls == 0; });
}

char* getMethods(void* userData)
{
    auto& runtime = *static_cast<Runtime*>(userData);
    return copyForProtocol(runtime, runtime.abi.getMethods());
}

int acceptInboundToken(const char* module, const char* token, void* userData)
{
    return static_cast<Runtime*>(userData)->abi.acceptInboundToken(module, token);
}

void emitEvent(const char* event, const char* data, void* userData)
{
    auto& runtime = *static_cast<Runtime*>(userData);
    if (runtime.provider)
        (void)lp_provider_emit_event(runtime.provider, event, data);
}

void unloadDone(void* userData)
{
    auto& runtime = *static_cast<Runtime*>(userData);
    {
        std::lock_guard<std::mutex> lock(runtime.unloadMutex);
        runtime.unloadDone = true;
    }
    runtime.unloadChanged.notify_all();
}

std::atomic<bool> gStop{false};

#ifdef _WIN32
BOOL WINAPI consoleHandler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT
        || event == CTRL_SHUTDOWN_EVENT) {
        gStop = true;
        return TRUE;
    }
    return FALSE;
}

// The container stops a host by posting WM_QUIT to its main thread, which
// fails unless that thread already has a message queue.
void createMessageQueue()
{
    MSG msg;
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
}

void waitForStop()
{
    MSG msg;
    while (!gStop.load()) {
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 25, QS_ALLPOSTMESSAGE);
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            if (msg.message == WM_QUIT) gStop = true;
    }
}
#else
extern "C" void stopHandler(int) { gStop = true; }

void waitForStop()
{
    while (!gStop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(25));
}
#endif

bool verifyIdentity(Runtime& runtime, const std::string& expected, std::string& error)
{
    char* nameText = runtime.abi.dispatch("name", "[]");
    const json name = nameText ? json::parse(nameText, nullptr, false) : json();
    runtime.abi.stringFree(nameText);
    if (!name.is_string() || name.get<std::string>() != expected) {
        error = "module name mismatch: expected '" + expected + "'";
        if (name.is_string()) error += ", got '" + name.get<std::string>() + "'";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    isolateAndFollowParent();
#ifdef _WIN32
    createMessageQueue();
#endif
    ModuleArgs args = parseProcessArguments(argc, argv);
    if (!args.valid) return 1;
    installCrashHandler(args.name.c_str());

    const std::string token = HostTokenSource::read(args.tokenSource);
    if (token.empty()) {
        reportLoadStatus(false, "no auth token arrived on "
            + (args.tokenSource.empty() ? std::string("stdin") : args.tokenSource));
        return 1;
    }
    if (const std::string problem = ModulePath::fileProblem(args.path); !problem.empty()) {
        reportLoadStatus(false, problem);
        return 1;
    }

    // Its directory stays searchable for libraries it loads later by name.
    if (const std::string problem = ModuleDllSearch::configure(args.path); !problem.empty()) {
        reportLoadStatus(false, problem);
        return 1;
    }
    DynamicLibrary library;
    std::string error;
    if (!library.open(args.path, error)) {
        reportLoadStatus(false, "failed to load native module: " + error);
        return 1;
    }

    Runtime runtime;
    runtime.name = args.name;
    if (args.concurrency == "multi") {
        const unsigned hardware = std::thread::hardware_concurrency();
        runtime.maxCalls = args.maxWorkers > 0 ? static_cast<unsigned>(args.maxWorkers)
                                               : (hardware > 0 ? hardware : 1);
    }
    if (!runtime.abi.resolve(library, error)) {
        reportLoadStatus(false, error);
        return 1;
    }
    const char* moduleVersion = runtime.abi.protocolVersion();
    if (protocolMajor(moduleVersion) != LOGOS_PROTOCOL_VERSION_MAJOR) {
        reportLoadStatus(false, "incompatible logos-protocol version "
            + std::string(moduleVersion ? moduleVersion : "<missing>"));
        return 1;
    }
    if (!verifyIdentity(runtime, args.name, error)) {
        reportLoadStatus(false, error);
        return 1;
    }

    const std::string transportSet = decodeTransportSetArg(args.transportSetJson);
    if (std::string problem; !logos::parseTransportSet(transportSet, nullptr, &problem)) {
        reportLoadStatus(false, "unusable --transport-set: " + problem);
        return 1;
    }
    runtime.provider = lp_provider_create(args.name.c_str(), transportSet.c_str());
    if (!runtime.provider) {
        reportLoadStatus(false, "qt_remote_plain provider could not be created");
        return 1;
    }
    if (lp_provider_set_max_concurrent_calls(runtime.provider, runtime.maxCalls) != LP_OK) {
        reportLoadStatus(false, "qt_remote_plain provider rejected its call concurrency");
        lp_provider_destroy(runtime.provider);
        return 1;
    }
    if (lp_provider_save_token(runtime.provider, "core", token.c_str()) != LP_OK) {
        reportLoadStatus(false, "qt_remote_plain provider rejected its core credential");
        lp_provider_destroy(runtime.provider);
        return 1;
    }
    const int prepareStatus = lp_provider_prepare(
        runtime.provider, &dispatch, &getMethods, &acceptInboundToken, &runtime);
    if (prepareStatus != LP_OK) {
        reportLoadStatus(false, "qt_remote_plain provider handshake could not be published (status "
            + std::to_string(prepareStatus) + ")");
        if (runtime.provider) lp_provider_destroy(runtime.provider);
        return 1;
    }

    if (runtime.abi.acceptToken("core", token.c_str()) != LP_OK
        || runtime.abi.acceptToken("capability_module", token.c_str()) != LP_OK) {
        reportLoadStatus(false, "module refused its outbound auth token");
        lp_provider_destroy(runtime.provider);
        return 1;
    }
    if (!args.hostServices.empty()) {
        const std::string services = hostServicesJson(args.hostServices);
        if (runtime.abi.grantHostServices(services.c_str()) != LP_OK) {
            reportLoadStatus(false, "module refused host-services grant");
            lp_provider_destroy(runtime.provider);
            return 1;
        }
    }

    runtime.abi.setEmitCallback(&emitEvent, &runtime);
    const auto persistence = std::filesystem::u8path(args.instancePersistencePath);
    const std::string instance = args.instancePersistencePath.empty()
        ? std::string{} : persistence.filename().u8string();
    const std::string moduleDir =
        std::filesystem::absolute(std::filesystem::u8path(args.path)).parent_path().u8string();
    runtime.abi.setContext(moduleDir.c_str(), instance.c_str(),
                           args.instancePersistencePath.c_str());

    const int publishStatus = lp_provider_register(
        runtime.provider, &dispatch, &getMethods, &acceptInboundToken, &runtime);
    if (publishStatus != LP_OK) {
        reportLoadStatus(false, "qt_remote_plain business provider could not be published (status "
            + std::to_string(publishStatus) + ")");
        lp_provider_destroy(runtime.provider);
        runtime.provider = nullptr;
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(&consoleHandler, TRUE);
#else
    struct sigaction action{};
    action.sa_handler = &stopHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
#endif

    reportLoadStatus(true);
    waitForStop();

    // The container kills 5s after the stop; draining calls and the module's
    // unload share 3s of that, as in logos_host_qt, leaving the rest for teardown.
    const auto unloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    stopCalls(runtime, unloadDeadline);
    runtime.abi.setUnloadDoneCallback(&unloadDone, &runtime);
    if (runtime.abi.aboutToUnload() == 1) {
        std::unique_lock<std::mutex> lock(runtime.unloadMutex);
        runtime.unloadChanged.wait_until(lock, unloadDeadline,
                                         [&] { return runtime.unloadDone; });
    }
    runtime.abi.setEmitCallback(nullptr, nullptr);
    runtime.abi.setUnloadDoneCallback(nullptr, nullptr);
    lp_provider_destroy(runtime.provider);
    runtime.provider = nullptr;
    library.close();
    return 0;
}
