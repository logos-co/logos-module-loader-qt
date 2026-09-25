#ifndef LOGOS_NATIVE_MODULE_HOST_H
#define LOGOS_NATIVE_MODULE_HOST_H

// Hosts one native (module-impl C ABI) module: loads and verifies its image,
// publishes it as a plain provider and runs its unload handshake. logos_host_plain
// runs it in a process of its own; a runtime host runs it in-process, with a delegate.

#include <chrono>
#include <functional>
#include <memory>
#include <string>

struct lp_provider;
struct lp_runtime_delegate_v1;

namespace logos::native_host {

struct Options {
    std::string name;         // the name the image must answer to
    std::string path;         // the image
    std::string transportSet; // the provider's transport set, as JSON
    std::string credential;   // the module's credential
    std::string anchor = "core"; // the principal that presents it to the module
    std::string hostServices; // a JSON array granted to the image; empty grants nothing
    unsigned maxCalls = 1;    // calls the provider runs at once; 1 is "single"
    std::string instancePersistencePath;
    // In-process hosts: installed through logos_module_set_runtime_delegate before
    // the module does anything else. An image without that export is refused.
    const lp_runtime_delegate_v1* delegate = nullptr;
    // Asked just before the module is published; false abandons the load.
    std::function<bool()> stillWanted;
};

// "multi" runs maxWorkers calls at once (the hardware's count when 0); else 1.
unsigned maxCallsFor(const std::string& concurrency, int maxWorkers);

enum class Teardown {
    Process,  // the process exits next: always withdraw the provider and close the image
    InProcess // the host lives on: never unmap, and keep everything if a call outlives the deadline
};

class Module {
public:
    Module();
    ~Module();
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    // False, with `error`, when the module cannot serve; nothing stays published.
    bool start(const Options& options, std::string& error);

    // Refuses new calls, waits for running ones, runs the module's unload and
    // withdraws the provider, all by `deadline`. False when a call or the unload
    // outlived it.
    bool stop(std::chrono::steady_clock::time_point deadline, Teardown teardown);

    lp_provider* provider() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace logos::native_host

#endif
