#include "module_dll_search.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message + " (Windows error " +
                                 std::to_string(GetLastError()) + ")");
}

fs::path executableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(length > 0 && length < path.size(), "GetModuleFileNameW");
    path.resize(length);
    return fs::path(path).parent_path();
}

struct TempDirectory {
    fs::path previous = fs::current_path();
    fs::path path = fs::temp_directory_path() /
        ("logos-dll-search-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    TempDirectory() { require(fs::create_directory(path), "create test directory"); }
    ~TempDirectory()
    {
        std::error_code ignored;
        fs::current_path(previous, ignored);
        fs::remove_all(path, ignored);
    }
};

void checkLibrary(const wchar_t* name, bool expected)
{
    const HMODULE library = LoadLibraryW(name);
    const bool found = library != nullptr;
    if (library)
        FreeLibrary(library);
    require(found == expected, "unexpected search result for " + fs::path(name).u8string());
}

} // namespace

// Run each mode in a fresh process: SetDefaultDllDirectories cannot be undone.
int main(int argc, char** argv)
{
    try {
        require(argc == 2, "expected control or configured");
        const std::string mode = argv[1];
        require(mode == "control" || mode == "configured", "unknown mode");
        const bool configured = mode == "configured";
        const fs::path fixtures = executableDirectory() / "dll-search-fixtures";
        TempDirectory temp;
        // Exercise UTF-8 -> Win32 wide paths, spaces, and a module outside the
        // executable directory. Neither CWD nor PATH points at this directory.
        const fs::path moduleDir = temp.path / L"module \u03bb with spaces";
        fs::copy(fixtures, moduleDir, fs::copy_options::recursive);
        const fs::path cwd = temp.path / "cwd";
        const fs::path pathDir = temp.path / "path";
        fs::create_directory(cwd);
        fs::create_directory(pathDir);
        fs::copy_file(moduleDir / "logos_dll_search_leaf.dll", cwd / "logos_dll_search_cwd.dll");
        fs::copy_file(moduleDir / "logos_dll_search_leaf.dll", pathDir / "logos_dll_search_path.dll");
        fs::current_path(cwd);
        require(SetEnvironmentVariableW(L"PATH", pathDir.c_str()), "set test PATH");

        const fs::path pluginPath = moduleDir / "logos_dll_search_plugin.dll";
        require(!ModuleDllSearch::configure("relative.dll").empty(), "relative path must be rejected");
        if (configured) {
            const std::string error = ModuleDllSearch::configure(pluginPath.u8string());
            require(error.empty(), error);
        }

        // The same initial-load flags as logos-module. Even the control must
        // load the plugin; these flags alone must not resolve its later load.
        HMODULE plugin = LoadLibraryExW(pluginPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(plugin != nullptr, "initial plugin load");
        const auto load = reinterpret_cast<int (*)()>(GetProcAddress(plugin, "loadLateDependency"));
        require(load != nullptr, "find plugin entry point");
        const int result = load();
        FreeLibrary(plugin);
        require(result == (configured ? 42 : 0), "late dependency and its transitive import");

        checkLibrary(L"logos_dll_search_host.dll", true);
        checkLibrary(L"version.dll", true); // System32 remains searchable.
        checkLibrary(L"logos_dll_search_cwd.dll", !configured);
        checkLibrary(L"logos_dll_search_path.dll", !configured);
        std::printf("PASS %s: late load=%d; host/System32 available; CWD/PATH %s\n",
                    mode.c_str(), result, configured ? "excluded" : "available");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
