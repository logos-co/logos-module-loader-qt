#include "module_initializer.h"
#include <QObject>
#include <spdlog/spdlog.h>
#include <filesystem>
#include "interface.h"
#include "logos_api.h"
#include "logos_api_provider.h"
#include "logos_transport_config.h"
#include "logos_transport_config_json.h"
#include "token_manager.h"
#include "module_lib.h"

namespace fs = std::filesystem;

using namespace ModuleLib;

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
            logos::transportSetFromJsonString(transportSetJson);
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
