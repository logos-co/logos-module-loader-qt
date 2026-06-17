#ifndef MODULE_INITIALIZER_H
#define MODULE_INITIALIZER_H

#include <string>
#include "module_lib.h"

class PluginInterface;
class LogosAPI;
class QObject;

ModuleLib::LogosModule loadModule(const std::string& modulePath, const std::string& expectedName);

LogosAPI* initializeLogosAPI(const std::string& moduleName, QObject* module,
                              PluginInterface* basePlugin, const std::string& authToken,
                              const std::string& modulePath,
                              const std::string& instancePersistencePath = {},
                              const std::string& transportSetJson = {});

#endif // MODULE_INITIALIZER_H
