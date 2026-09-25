#include "command_line_parser.h"
#include "crash_handler.h"
#include "module_dll_search.h"
#include "module_path.h"
#include "native_module_host.h"
#include "parent_lifetime.h"
#include "token_source.h"
#include "transport_set_arg.h"

#include <logos_container/load_status.h>
#include <logos_transport_config_json.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
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
    const std::string transportSet = decodeTransportSetArg(args.transportSetJson);
    if (std::string problem; !logos::parseTransportSet(transportSet, nullptr, &problem)) {
        reportLoadStatus(false, "unusable --transport-set: " + problem);
        return 1;
    }
    logos::native_host::Options options;
    options.name = args.name;
    options.path = args.path;
    options.transportSet = transportSet;
    options.credential = token;
    options.hostServices = args.hostServices.empty() ? std::string{}
                                                     : hostServicesJson(args.hostServices);
    options.maxCalls = logos::native_host::maxCallsFor(args.concurrency, args.maxWorkers);
    options.instancePersistencePath = args.instancePersistencePath;
    logos::native_host::Module module;
    if (std::string error; !module.start(options, error)) {
        reportLoadStatus(false, error);
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
    module.stop(std::chrono::steady_clock::now() + std::chrono::seconds(3),
                logos::native_host::Teardown::Process);
    return 0;
}
