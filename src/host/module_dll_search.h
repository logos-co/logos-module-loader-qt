#ifndef MODULE_DLL_SEARCH_H
#define MODULE_DLL_SEARCH_H

#include <string>

namespace ModuleDllSearch {

// Configure the dedicated host before loading its plugin. modulePath is the
// absolute UTF-8 path supplied to the plugin loader. Returns an error message
// on failure; the host must then abort loading. No-op outside Windows.
//
// This is process-wide and lasts until process exit, including module teardown.
// It must not be used by a shared-process loader for multiple modules.
std::string configure(const std::string& modulePath);

} // namespace ModuleDllSearch

#endif
