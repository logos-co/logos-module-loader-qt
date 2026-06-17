#include "qt_plugin_format_loader.h"

#include <boost/dll/runtime_symbol_info.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path findInDir(const fs::path& dir) {
    for (const auto& name : {"logos_host_qt", "logos_host"}) {
        auto candidate = (dir / name).lexically_normal();
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

    return args;
}
