#ifndef MODULE_INITIALIZER_H
#define MODULE_INITIALIZER_H

#include <string>
#include "module_lib.h"

class PluginInterface;
class LogosAPI;
class QObject;

// `error`, when given, receives why the load failed -- the loader's own
// message where there is one. main() puts it on the load-status line so the
// daemon reports the cause instead of an exit code.
ModuleLib::LogosModule loadModule(const std::string& modulePath,
                                  const std::string& expectedName,
                                  std::string* error = nullptr);

LogosAPI* initializeLogosAPI(const std::string& moduleName, QObject* module,
                              PluginInterface* basePlugin, const std::string& authToken,
                              const std::string& hostServices,
                              const std::string& modulePath,
                              const std::string& instancePersistencePath = {},
                              const std::string& transportSetJson = {});

#endif // MODULE_INITIALIZER_H
