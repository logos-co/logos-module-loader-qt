#include "module_initializer.h"
#include <QByteArray>
#include <QObject>
#include <spdlog/spdlog.h>
#include <filesystem>
// Explicit: the host-services grant arrives as a bare comma-separated list over
// argv and is re-serialised to a JSON array here. logos_transport_config_json.h
// deliberately keeps its nlohmann include off the fast path, so do not rely on
// picking it up transitively from there.
#include <nlohmann/json.hpp>
#include "interface.h"
#include "logos_api.h"
#include "logos_api_provider.h"
#include "logos_transport_config.h"
#include "logos_transport_config_json.h"
#include "token_manager.h"
#include "module_lib.h"

namespace fs = std::filesystem;

using namespace ModuleLib;

namespace {

// `--transport-set` carries base64 so its JSON survives the command line: on
// Windows, CommandLineToArgvW consumes `"` as a quoting delimiter, so raw JSON
// arrives unparseable. See base64Encode() in qt_plugin_format_loader.cpp, which
// is the only emitter.
//
// Both forms are accepted, and the discrimination is exact rather than
// heuristic: a JSON transport set always begins with `{` or `[`, and neither
// character is in the base64 alphabet. That keeps an older daemon paired with a
// newer logos_host working.
//
// A payload that is neither valid base64 nor JSON is returned unchanged, so the
// error surfaces where it is diagnosable — in transportSetFromJsonString —
// rather than as a silently empty transport set here.
std::string decodeTransportSetArg(const std::string& arg)
{
    if (!arg.empty() && (arg.front() == '{' || arg.front() == '[')) {
        spdlog::debug("transport set supplied as raw JSON (pre-base64 emitter)");
        return arg;
    }
    const QByteArray decoded = QByteArray::fromBase64(
        QByteArray::fromStdString(arg), QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.isEmpty()) {
        spdlog::warn("transport set is neither JSON nor valid base64; passing through unchanged");
        return arg;
    }
    return decoded.toStdString();
}

} // namespace

LogosModule loadModule(const std::string& modulePath, const std::string& expectedName)
{
    std::string errorString;
    LogosModule module = LogosModule::loadFromPath(modulePath, &errorString);

    if (!module.isValid()) {
        spdlog::critical("Failed to load module: {}", errorString);
        return LogosModule();
    }

    PluginInterface *basePlugin = module.as<PluginInterface>();
    if (!basePlugin) {
        spdlog::critical("Module does not implement the PluginInterface");
        return LogosModule();
    }

    // Defense-in-depth against privileged-name impersonation (F-022). The
    // parent passes the trusted registry key as `expectedName`; the plugin's
    // own name() is its self-asserted identity. If they disagree, the binary
    // is not the module it was loaded as — refuse to initialize it rather than
    // let it run (and receive tokens) under a name it doesn't implement.
    if (expectedName != basePlugin->name().toStdString()) {
        spdlog::critical("Refusing module: name mismatch, expected '{}' got '{}'",
                         expectedName, basePlugin->name().toStdString());
        return LogosModule();
    }

    return module;
}

LogosAPI* initializeLogosAPI(const std::string& moduleName, QObject* module,
                              PluginInterface* basePlugin, const std::string& authToken,
                              const std::string& hostServices,
                              const std::string& modulePath,
                              const std::string& instancePersistencePath,
                              const std::string& transportSetJson)
{
    // If the daemon passed a transport set for this module, deserialize
    // and use the explicit-transport LogosAPI constructor so the
    // module's LogosAPIProvider binds every listener (LocalSocket +
    // any TCP / TCP+SSL endpoints). Otherwise fall back to the
    // single-arg constructor → global default (LocalSocket only),
    // matching the long-standing behaviour for modules the daemon
    // hasn't explicitly configured.
    LogosAPI* logos_api = nullptr;
    if (!transportSetJson.empty()) {
        LogosTransportSet set =
            logos::transportSetFromJsonString(decodeTransportSetArg(transportSetJson));
        logos_api = new LogosAPI(QString::fromStdString(moduleName),
                                  std::move(set), module);
    } else {
        logos_api = new LogosAPI(moduleName, module);
    }
    logos_api->setProperty("modulePath",
        fs::absolute(fs::path(modulePath)).parent_path().string());

    if (!instancePersistencePath.empty()) {
        logos_api->setProperty("instancePersistencePath", instancePersistencePath);
        logos_api->setProperty("instanceId",
            fs::path(instancePersistencePath).filename().string());
    }

    // Surface the token as a QObject property BEFORE registerObject:
    // registration runs the provider object's init(), where cdylib-authored
    // modules read this property (a cross-image-safe dynamic lookup, like
    // modulePath above) and forward it across the module-impl C ABI via
    // logos_module_accept_token — their statically-linked protocol stack has
    // its own TokenManager copy the host's saveToken calls below never reach.
    logos_api->setProperty("authToken", QString::fromStdString(authToken));

    // Same channel, same reason, for the privileged host-services grant. Only
    // stamped when the host actually granted something: the property's ABSENCE
    // is what keeps an ordinary module fail-closed, and lp_grant_host_services
    // REPLACES rather than adds, so pushing an empty array would be a needless
    // clear.
    //
    // This sits after loadModule()'s name check (module_initializer.cpp's
    // loadModule refuses a plugin whose own name() disagrees with the trusted
    // registry key the parent passed), so by here the identity the grant is
    // bound to has already been verified against the binary.
    //
    // The flag arrives as a BARE COMMA-SEPARATED LIST and is re-serialised to a
    // JSON array here. It is not carried as JSON across the command line
    // because Windows' CommandLineToArgvW consumes `"` as a quoting delimiter:
    // `["token_registry","token_delivery"]` reached this process as
    // `[token_registry,token_delivery]`, which nlohmann discards, and
    // lp_grant_host_services rejects an unparseable list WHOLESALE. The symptom
    // was Windows-only and silent apart from capability_module's own
    // `host services refused` line. See hostServicesFor() in
    // qt_plugin_format_loader.cpp.
    //
    // The property and the C ABI both still speak JSON; only the argv hop
    // changed. Serialise through nlohmann rather than by string concatenation so
    // a name that ever needs escaping is escaped.
    if (!hostServices.empty()) {
        nlohmann::json services = nlohmann::json::array();
        std::size_t start = 0;
        while (start <= hostServices.size()) {
            const std::size_t comma = hostServices.find(',', start);
            const std::size_t end = (comma == std::string::npos) ? hostServices.size() : comma;
            std::string name = hostServices.substr(start, end - start);
            if (!name.empty())
                services.push_back(name);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        const std::string servicesJson = services.dump();
        spdlog::info("Granting host services to {}: {}", moduleName, servicesJson);
        logos_api->setProperty("hostServices", QString::fromStdString(servicesJson));
    }

    bool success = logos_api->getProvider()->registerObject(basePlugin->name(), module);
    if (success) {
        logos_api->getTokenManager()->saveToken(std::string("core"), authToken);
        logos_api->getTokenManager()->saveToken(std::string("capability_module"), authToken);
    } else {
        spdlog::critical("Failed to register module for remote access: {}", basePlugin->name().toStdString());
        delete module;
        delete logos_api;
        return nullptr;
    }

    return logos_api;
}
