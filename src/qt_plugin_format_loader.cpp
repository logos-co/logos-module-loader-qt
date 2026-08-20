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
//
// The value is a BARE COMMA-SEPARATED LIST, not JSON, because it crosses a
// command line. This used to be `["token_registry","token_delivery"]` and it
// broke on Windows only: CommandLineToArgvW treats `"` as a quoting delimiter
// and CONSUMES it, so the child received `[token_registry,token_delivery]`,
// nlohmann's parser discarded it, and lp_grant_host_services returned
// LP_ERR_INVALID_ARG — rejecting the whole list by design. Measured on a real
// Windows run: the host logged the correct JSON, the module process logged the
// quote-stripped form, and the impl reported `host services refused`. POSIX
// exec() passes argv through untouched, which is why Linux and macOS never saw
// it and the basecamp host-services check stayed green.
//
// Service names are a closed set of `[a-z_]+` identifiers, so a comma-separated
// list needs no quoting, no escaping and no brackets, and survives any
// command-line reconstruction. module_initializer turns it back into the JSON
// array that the `hostServices` property and lp_grant_host_services both
// require, so neither of those contracts changes.
const char* hostServicesFor(const std::string& moduleName)
{
    if (moduleName == "capability_module")
        return "token_registry,token_delivery";
    return nullptr;
}

// Standard base64, so a JSON payload can cross a command line intact.
//
// The transport set is arbitrary nested JSON — endpoints, ports, TLS paths —
// so unlike the host-services grant it cannot be flattened into a bare
// identifier list, and it hits the same Windows defect: CommandLineToArgvW
// consumes every `"`, and the child receives an unparseable string. The base64
// alphabet is [A-Za-z0-9+/=] with no quote, space or backslash, so it survives
// any command-line reconstruction on every platform.
//
// Hand-rolled rather than QByteArray::toBase64 because this translation unit is
// deliberately Qt-free (boost + spdlog + std only) and this is the only place
// that needs it; module_initializer, which is already a Qt TU, decodes with
// QByteArray::fromBase64.
std::string base64Encode(const std::string& in)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);

    auto byte = [&in](std::size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(in[i])); };

    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (byte(i) << 16) | (byte(i + 1) << 8) | byte(i + 2);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
        out += kAlphabet[v & 0x3F];
    }
    if (in.size() - i == 1) {
        const unsigned v = byte(i) << 16;
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += "==";
    } else if (in.size() - i == 2) {
        const unsigned v = (byte(i) << 16) | (byte(i + 1) << 8);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
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
        // Base64, not raw JSON — see base64Encode above. The receiver detects
        // which form it got: JSON starts with `{` or `[`, neither of which is
        // in the base64 alphabet, so an older emitter talking to a newer host
        // still works.
        args.push_back("--transport-set");
        args.push_back(base64Encode(desc.transportSetJson));
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
