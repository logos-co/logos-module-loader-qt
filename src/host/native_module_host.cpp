#include "native_module_host.h"

#include <logos_module_impl.h>
#include <logos_protocol.h>
#if defined(LOGOS_PROTOCOL_HAS_RUNTIME_DELEGATE)
#include <logos_runtime_delegate.h>
#endif

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace logos::native_host {
namespace {

using json = nlohmann::json;

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
        if (!m_handle || m_keep) return;
#ifdef _WIN32
        FreeLibrary(m_handle);
#else
        dlclose(m_handle);
#endif
        m_handle = nullptr;
    }

    // Never unmapped: code from the image may still run, or run again.
    void keepMapped() { m_keep = true; }

    template <typename T>
    T symbol(const char* name, std::string& error) const
    {
        auto value = optionalSymbol<T>(name);
        if (!value && error.empty()) error = std::string("missing required symbol: ") + name;
        return value;
    }

    template <typename T>
    T optionalSymbol(const char* name) const
    {
#ifdef _WIN32
        return reinterpret_cast<T>(GetProcAddress(m_handle, name));
#else
        dlerror();
        return reinterpret_cast<T>(dlsym(m_handle, name));
#endif
    }

private:
#ifdef _WIN32
    HMODULE m_handle = nullptr;
#else
    void* m_handle = nullptr;
#endif
    bool m_keep = false;
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

} // namespace

struct Module::State {
    DynamicLibrary library;
    ModuleAbi abi;
    std::string name;
    bool inProcess = false;
    lp_provider* provider = nullptr;
    std::mutex unloadMutex;
    std::condition_variable unloadChanged;
    bool unloadDone = false;
    // Calls in the module; once unloading, none enters.
    std::mutex callsMutex;
    std::condition_variable callsChanged;
    int activeCalls = 0;
    bool unloading = false;

    char* copyForProtocol(char* moduleText)
    {
        if (!moduleText) return nullptr;
        char* copy = lp_string_copy(moduleText);
        abi.stringFree(moduleText);
        return copy;
    }

    static char* dispatch(const char* method, const char* args, void* userData)
    {
        auto& state = *static_cast<State*>(userData);
        {
            std::lock_guard<std::mutex> lock(state.callsMutex);
            if (state.unloading) {
                const json refused{{"code", "dispatch_failed"}, {"message", "module is unloading"},
                                   {"origin", state.name}};
                return lp_string_copy(refused.dump().c_str());
            }
            ++state.activeCalls;
        }
        state.abi.setCallCaller(lp_current_caller_json());
        char* result = state.copyForProtocol(state.abi.dispatch(method, args));
        state.abi.setCallCaller(nullptr);
        {
            std::lock_guard<std::mutex> lock(state.callsMutex);
            --state.activeCalls;
        }
        state.callsChanged.notify_all();
        return result;
    }

    static char* getMethods(void* userData)
    {
        auto& state = *static_cast<State*>(userData);
        return state.copyForProtocol(state.abi.getMethods());
    }

    static int acceptInboundToken(const char* module, const char* token, void* userData)
    {
        return static_cast<State*>(userData)->abi.acceptInboundToken(module, token);
    }

    static void emitEvent(const char* event, const char* data, void* userData)
    {
        auto& state = *static_cast<State*>(userData);
        if (state.provider) (void)lp_provider_emit_event(state.provider, event, data);
    }

    static void onUnloadDone(void* userData)
    {
        auto& state = *static_cast<State*>(userData);
        {
            std::lock_guard<std::mutex> lock(state.unloadMutex);
            state.unloadDone = true;
        }
        state.unloadChanged.notify_all();
    }

    bool verifyIdentity(const std::string& expected, std::string& error)
    {
        char* nameText = abi.dispatch("name", "[]");
        const json answered = nameText ? json::parse(nameText, nullptr, false) : json();
        abi.stringFree(nameText);
        if (!answered.is_string() || answered.get<std::string>() != expected) {
            error = "module name mismatch: expected '" + expected + "'";
            if (answered.is_string()) error += ", got '" + answered.get<std::string>() + "'";
            return false;
        }
        return true;
    }

    bool fail(std::string& error, std::string message)
    {
        error = std::move(message);
        if (provider) lp_provider_destroy(provider);
        provider = nullptr;
        return false;
    }
};

unsigned maxCallsFor(const std::string& concurrency, int maxWorkers)
{
    if (concurrency != "multi") return 1;
    if (maxWorkers > 0) return static_cast<unsigned>(maxWorkers);
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware > 0 ? hardware : 1;
}

Module::Module() : m_state(std::make_unique<State>()) {}

Module::~Module()
{
    if (m_state && m_state->provider)
        stop(std::chrono::steady_clock::now() + std::chrono::seconds(3),
             m_state->inProcess ? Teardown::InProcess : Teardown::Process);
}

lp_provider* Module::provider() const { return m_state ? m_state->provider : nullptr; }

bool Module::start(const Options& options, std::string& error)
{
    State& state = *m_state;
    state.name = options.name;
    state.inProcess = options.delegate != nullptr;
    std::string problem;
    if (!state.library.open(options.path, problem))
        return state.fail(error, "failed to load native module: " + problem);
    // Its initializers have run in this process; so may its threads.
    if (state.inProcess) state.library.keepMapped();
    if (!state.abi.resolve(state.library, problem)) return state.fail(error, problem);

    if (options.delegate) {
#if defined(LOGOS_PROTOCOL_HAS_RUNTIME_DELEGATE)
        const auto install = state.library.optionalSymbol<logos_module_set_runtime_delegate_fn>(
            LOGOS_MODULE_SET_RUNTIME_DELEGATE_SYMBOL);
        if (!install)
            return state.fail(error, "module has no runtime delegate export; it cannot run in-process");
        if (install(options.delegate) != LP_OK)
            return state.fail(error, "module refused the runtime delegate");
#else
        return state.fail(error, "this logos-protocol has no runtime delegates");
#endif
    }

    const char* moduleVersion = state.abi.protocolVersion();
    if (protocolMajor(moduleVersion) != LOGOS_PROTOCOL_VERSION_MAJOR)
        return state.fail(error, "incompatible logos-protocol version "
            + std::string(moduleVersion ? moduleVersion : "<missing>"));
    if (!state.verifyIdentity(options.name, problem)) return state.fail(error, problem);

    state.provider = lp_provider_create(options.name.c_str(), options.transportSet.c_str());
    if (!state.provider) return state.fail(error, "qt_remote_plain provider could not be created");
    if (lp_provider_set_max_concurrent_calls(state.provider, options.maxCalls) != LP_OK)
        return state.fail(error, "qt_remote_plain provider rejected its call concurrency");
    if (lp_provider_save_token(state.provider, options.anchor.c_str(), options.credential.c_str())
        != LP_OK)
        return state.fail(error, "qt_remote_plain provider rejected its core credential");
    const int prepareStatus = lp_provider_prepare(state.provider, &State::dispatch,
                                                  &State::getMethods, &State::acceptInboundToken,
                                                  &state);
    if (prepareStatus != LP_OK)
        return state.fail(error, "qt_remote_plain provider handshake could not be published (status "
            + std::to_string(prepareStatus) + ")");

    if (state.abi.acceptToken("core", options.credential.c_str()) != LP_OK
        || state.abi.acceptToken("capability_module", options.credential.c_str()) != LP_OK)
        return state.fail(error, "module refused its outbound auth token");
    if (!options.hostServices.empty()
        && state.abi.grantHostServices(options.hostServices.c_str()) != LP_OK)
        return state.fail(error, "module refused host-services grant");

    state.abi.setEmitCallback(&State::emitEvent, &state);
    const auto persistence = std::filesystem::u8path(options.instancePersistencePath);
    const std::string instance = options.instancePersistencePath.empty()
        ? std::string{} : persistence.filename().u8string();
    const std::string moduleDir =
        std::filesystem::absolute(std::filesystem::u8path(options.path)).parent_path().u8string();
    state.abi.setContext(moduleDir.c_str(), instance.c_str(),
                         options.instancePersistencePath.c_str());

    if (options.stillWanted && !options.stillWanted()) {
        state.abi.setEmitCallback(nullptr, nullptr);
        return state.fail(error, "load abandoned");
    }
    const int publishStatus = lp_provider_register(state.provider, &State::dispatch,
                                                   &State::getMethods, &State::acceptInboundToken,
                                                   &state);
    if (publishStatus != LP_OK) {
        state.abi.setEmitCallback(nullptr, nullptr);
        return state.fail(error, "qt_remote_plain business provider could not be published (status "
            + std::to_string(publishStatus) + ")");
    }
    return true;
}

bool Module::stop(std::chrono::steady_clock::time_point deadline, Teardown teardown)
{
    State& state = *m_state;
    if (!state.provider) return true;
    bool drained = false;
    {
        std::unique_lock<std::mutex> lock(state.callsMutex);
        state.unloading = true;
        drained = state.callsChanged.wait_until(lock, deadline,
                                                [&] { return state.activeCalls == 0; });
    }
    bool unloaded = true;
    state.abi.setUnloadDoneCallback(&State::onUnloadDone, &state);
    if (state.abi.aboutToUnload() == 1) {
        std::unique_lock<std::mutex> lock(state.unloadMutex);
        unloaded = state.unloadChanged.wait_until(lock, deadline,
                                                  [&] { return state.unloadDone; });
    }
    const bool clean = drained && unloaded;
    if (teardown == Teardown::InProcess) state.library.keepMapped();
    if (teardown == Teardown::InProcess && !clean) {
        // A call or the unload still runs in the module: leak all it may touch.
        // Calls that arrive meanwhile are refused as "module is unloading".
        (void)m_state.release();
        return false;
    }
    state.abi.setEmitCallback(nullptr, nullptr);
    state.abi.setUnloadDoneCallback(nullptr, nullptr);
    lp_provider_destroy(state.provider);
    state.provider = nullptr;
    state.library.close();
    return clean;
}

} // namespace logos::native_host
