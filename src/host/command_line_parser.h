#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <string>

struct ModuleArgs {
    std::string name;
    std::string path;
    std::string instancePersistencePath;
    // Per-module transport set, serialized as JSON (the format defined
    // by logos-cpp-sdk's logos_transport_config_json.h). Empty means
    // "use the global default (LocalSocket only)" — the back-compat
    // behaviour for modules the daemon hasn't configured.
    std::string transportSetJson;
    // Where to read the auth token from, set by the container that spawned us
    // (see TokenSource). Empty means "stdin" — the default the subprocess
    // container uses. A different container could pass "fd:<n>" or
    // "file:<path>". The host stays agnostic to which container it runs under.
    std::string tokenSource;
    // Privileged host services this module has been granted, as a JSON array
    // from the closed set logos-protocol documents ("token_registry",
    // "token_delivery", "dynamic_calls"). Empty — the case for every ordinary
    // module — means no grant at all, and the module stays fail-closed.
    //
    // Passed at LAUNCH rather than delivered later because the grant has to be
    // in place before the plugin's provider init() runs: that is where a
    // cdylib module forwards it across the module-impl C ABI into its OWN
    // image, which is the only image whose gates it can open.
    std::string hostServices;
    bool valid;
};

ModuleArgs parseCommandLineArgs(int argc, char *argv[]);

#endif // COMMAND_LINE_PARSER_H
