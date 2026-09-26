#include "command_line_parser.h"
#include "crash_handler.h"
#include "host_process.h"
#include "module_dll_search.h"
#include "module_path.h"
#include "native_module_host.h"
#include "token_source.h"
#include "transport_set_arg.h"

#include <logos_transport_config_json.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

namespace {

using json = nlohmann::json;
using logos::host_process::reportLoadStatus;

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

} // namespace

int main(int argc, char** argv)
{
    logos::host_process::prepare();
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
    // A tls_tcp listener is how the runtime marks an export.
    for (const auto& transport : logos::transportSetFromJsonString(transportSet))
        if (transport.protocol == LogosProtocol::TlsTcp) options.peering = "peering_module";
    logos::native_host::Module module;
    if (std::string error; !module.start(options, error)) {
        reportLoadStatus(false, error);
        return 1;
    }

    logos::host_process::installStopHandlers();
    reportLoadStatus(true);
    logos::host_process::waitForStop();

    // The container kills 5s after the stop; draining calls and the module's
    // unload share 3s of that, as in logos_host_qt, leaving the rest for teardown.
    module.stop(std::chrono::steady_clock::now() + std::chrono::seconds(3),
                logos::native_host::Teardown::Process);
    return 0;
}
