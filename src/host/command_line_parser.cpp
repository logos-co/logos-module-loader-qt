#include "command_line_parser.h"
#include "module_path.h"
#include <CLI/CLI.hpp>

ModuleArgs parseCommandLineArgs(int argc, char *argv[])
{
    ModuleArgs result;
    result.valid = false;

    CLI::App app{"Logos host for loading modules in separate processes"};
    app.set_version_flag("-v,--version", "1.0");

    app.add_option("-n,--name", result.name, "Name of the module to load")
        ->required();
    app.add_option("-p,--path", result.path,
        "Path to the module file; relative paths are taken from the working "
        "directory")
        ->required();
    app.add_option("--instance-persistence-path", result.instancePersistencePath,
        "Instance persistence directory for the module");
    app.add_option("--transport-set", result.transportSetJson,
        "Per-module transport set, base64-encoded JSON (logos-cpp-sdk shape); raw "
        "JSON is still accepted; empty = global default");
    app.add_option("--token-source", result.tokenSource,
        "Where to read the auth token from: stdin (default), fd:<n>, or file:<path>");
    app.add_option("--host-services", result.hostServices,
        "Privileged host services granted to this module, as a bare "
        "comma-separated list (e.g. token_registry,token_delivery); "
        "empty (the default) means none");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        app.exit(e);
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
