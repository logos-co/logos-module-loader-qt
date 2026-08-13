#include "qt_plugin_format_loader.h"

#include <boost/dll/runtime_symbol_info.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

// Windows executables carry a .exe extension, so probing the bare name finds
// nothing and EVERY module load fails with "logos_host_qt ... not found" — the
// host is resolved before any module can be spawned. logos-view-module-runtime
// already does this for its ui-host (ViewModuleHost.cpp).
#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

// ── The host-services policy ────────────────────────────────────────────────
//
// Which modules may hold the TRUST-ROOT services, decided HERE — host side and
// bound to the module name the registry trusts, not to anything the module
// asserts about itself. A module's metadata.json declaration is advisory; this
// is the authority.
//
// Deliberately a hardcoded table rather than configuration: these two services
// let their holder enumerate the token store and hand authority to an
// arbitrary target, which is capability_module's job and nothing else's. A
// deployment that wants a different trust root is a different build.
//
// `dynamic_calls` is NOT granted here. It is elevated but not a trust root, so
// it belongs with the per-module access policy the daemon already applies,
// alongside allowedCallers — not in a table that exists to keep a list at two
// entries.
const char* hostServicesFor(const std::string& moduleName)
{
    if (moduleName == "capability_module")
        return R"(["token_registry","token_delivery"])";
    return nullptr;
}

fs::path findInDir(const fs::path& dir) {
    for (const auto& name : {"logos_host_qt", "logos_host"}) {
        auto candidate = (dir / (std::string(name) + kExeSuffix)).lexically_normal();
        if (fs::exists(candidate))
            return candidate;
    }
    return {};
}

std::string resolveLogosHostPath(const std::vector<std::string>& modulesDirs) {
    std::string logosHostPath;

    const char* envPath = std::getenv("LOGOS_HOST_PATH");
    if (envPath)
        logosHostPath = envPath;

    if (logosHostPath.empty()) {
        auto found = findInDir(fs::path(boost::dll::program_location().parent_path().string()));
        if (!found.empty())
            logosHostPath = found.string();
    }

    if (logosHostPath.empty() || !fs::exists(logosHostPath)) {
        if (!modulesDirs.empty()) {
            auto binDir = fs::absolute(
                fs::path(modulesDirs.front()) / ".." / "bin"
            ).lexically_normal();
            auto found = findInDir(binDir);
            if (!found.empty())
                logosHostPath = found.string();
        }
    }

    if (logosHostPath.empty() || !fs::exists(logosHostPath)) {
        spdlog::critical("logos_host_qt (or logos_host) not found - set LOGOS_HOST_PATH or place it next to the executable (last tried: {})",
                         logosHostPath);
        return {};
    }

    return logosHostPath;
}

} // anonymous namespace

bool QtPluginFormatLoader::canHandle(const LogosCore::ModuleDescriptor& desc) const
{
    return desc.format == "qt-plugin" || desc.format.empty();
}

std::string QtPluginFormatLoader::resolveHostBinary(const LogosCore::ModuleDescriptor& desc) const
{
    return resolveLogosHostPath(desc.modulesDirs);
}

std::vector<std::string> QtPluginFormatLoader::buildArguments(const LogosCore::ModuleDescriptor& desc) const
{
    std::vector<std::string> args = {
        "--name", desc.name,
        "--path", desc.path
    };

    if (!desc.instancePersistencePath.empty()) {
        args.push_back("--instance-persistence-path");
        args.push_back(desc.instancePersistencePath);
    }

    if (!desc.transportSetJson.empty()) {
        args.push_back("--transport-set");
        args.push_back(desc.transportSetJson);
    }

    // Privileged modules carry their grant on the command line, so it is in
    // place before the plugin's provider init() runs — that init is where a
    // cdylib module forwards the grant across the module-impl C ABI into its
    // own image. Ordinary modules get no flag at all and stay fail-closed.
    if (const char* services = hostServicesFor(desc.name)) {
        spdlog::info("Granting host services to '{}': {}", desc.name, services);
        args.push_back("--host-services");
        args.push_back(services);
    }

    return args;
}
