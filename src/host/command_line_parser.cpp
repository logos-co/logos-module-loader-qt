#include "command_line_parser.h"
#include "module_path.h"
#include <CLI/CLI.hpp>

#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

ModuleArgs parseCommandLineArgs(int argc, char *argv[])
{
    ModuleArgs result;
    result.valid = false;

    CLI::App app{"Logos host for loading modules in separate processes"};
    app.set_version_flag("-v,--version", "1.0");

    app.add_option("--inspect", result.inspectPath,
        "Print a Qt plugin's embedded Logos metadata as JSON and exit");
    app.add_option("-n,--name", result.name, "Name of the module to load");
    app.add_option("-p,--path", result.path,
        "Path to the module file; relative paths are taken from the working "
        "directory");
    app.add_option("--instance-persistence-path", result.instancePersistencePath,
        "Instance persistence directory for the module");
    app.add_option("--transport-set", result.transportSetJson,
        "Per-module transport set, base64-encoded JSON (logos-cpp-sdk shape); raw "
        "JSON is still accepted; empty = global default");
    app.add_option("--token-source", result.tokenSource,
        "Where to read the auth token from: stdin (default), fd:<n>, or file:<path>");
    app.add_option("--host-services", result.hostServices,
        "Privileged host services granted to this module, as a bare "
        "comma-separated list (e.g. token_delivery); "
        "empty (the default) means none");
    app.add_option("--concurrency", result.concurrency,
        "Native module dispatch mode: single (default) or multi");
    app.add_option("--max-workers", result.maxWorkers,
        "Maximum concurrent native dispatches in multi mode; zero selects a "
        "bounded hardware-derived default");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        app.exit(e);
        return result;
    }

    if (!result.inspectPath.empty()) {
        if (!result.name.empty() || !result.path.empty()) {
            return result;
        }
        result.inspectPath = ModulePath::resolve(result.inspectPath);
        result.valid = true;
        return result;
    }
    if (result.name.empty() || result.path.empty()) {
        return result;
    }
    if ((result.concurrency != "single" && result.concurrency != "multi")
        || result.maxWorkers < 0
        || (result.concurrency != "multi" && result.maxWorkers != 0)) {
        return result;
    }

    // Resolve --path HERE, so nothing downstream ever holds a relative one.
    // Qt would not resolve it the way the caller means: QPluginLoader searches
    // the Qt PLUGIN path for a relative name, never the working directory, and
    // answers a miss with "The shared library was not found." — see
    // module_path.h. The daemon always passes an absolute path and is
    // unaffected; this is for a human running the host by hand.
    result.path = ModulePath::resolve(result.path);

    result.valid = true;
    return result;
}

ModuleArgs parseProcessArguments(int argc, char *argv[])
{
#ifdef _WIN32
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) return parseCommandLineArgs(argc, argv);
    std::vector<std::string> arguments;
    for (int i = 0; i < count; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8.data(), n, nullptr, nullptr);
        arguments.push_back(std::move(utf8));
    }
    LocalFree(wide);
    std::vector<char*> pointers;
    for (std::string& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return parseCommandLineArgs(count, pointers.data());
#else
    return parseCommandLineArgs(argc, argv);
#endif
}
