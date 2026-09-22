#include "module_dll_search.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <exception>
#endif

namespace ModuleDllSearch {

std::string configure(const std::string& modulePath)
{
#ifdef _WIN32
    // Keep windows.h out of the host's other translation units: its interface
    // macro and TokenSource enum collide with SDK and host identifiers.
    try {
        const auto plugin = std::filesystem::u8path(modulePath);
        if (!plugin.is_absolute() || plugin.filename().empty())
            return "DLL search setup requires an absolute plugin file path: " + modulePath;

        const auto directory = plugin.parent_path();
        if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
            const auto error = GetLastError();
            return "SetDefaultDllDirectories failed (Windows error " +
                   std::to_string(error) + ")";
        }
        if (!AddDllDirectory(directory.c_str())) {
            const auto error = GetLastError();
            return "AddDllDirectory failed for " + directory.u8string() +
                   " (Windows error " + std::to_string(error) + ")";
        }

        // DEFAULT_DIRS keeps the executable directory and System32, enables
        // this user directory for later bare-name LoadLibrary calls, and omits
        // CWD and PATH. The existing per-load DLL_LOAD_DIR flag only covers
        // imports resolved while mapping the plugin, not later runtime loads.
        // This host loads one module and keeps it mapped until process exit,
        // so intentionally keep the directory registered for the same lifetime.
    } catch (const std::exception& error) {
        return "cannot configure the module DLL search path: " + std::string(error.what());
    }
#else
    (void)modulePath;
#endif
    return {};
}

} // namespace ModuleDllSearch
