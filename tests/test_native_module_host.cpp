// logos_native_module_host run in-process: the fixture module is loaded into this
// test and served over inproc, as a runtime host will serve its trusted modules.
#include "native_module_host.h"

#include <logos_protocol.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using logos::native_host::Module;
using logos::native_host::Options;
using logos::native_host::Teardown;

fs::path executableDir()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(std::wstring(buffer, length)).parent_path();
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof buffer;
    return _NSGetExecutablePath(buffer, &size) == 0 ? fs::path(buffer).parent_path() : fs::path();
#else
    return fs::read_symlink("/proc/self/exe").parent_path();
#endif
}

// Beside this test on Windows, in ../lib elsewhere: the build tree and the package agree.
std::string fixturePath()
{
    const fs::path dir = executableDir();
    for (const fs::path& candidate : {dir / "plain_host_fixture.dll",
                                      dir / ".." / "lib" / "libplain_host_fixture.so",
                                      dir / ".." / "lib" / "libplain_host_fixture.dylib"}) {
        if (fs::exists(candidate)) return fs::absolute(candidate).lexically_normal().u8string();
    }
    return {};
}

void useInstance(const std::string& prefix)
{
#ifdef _WIN32
    ASSERT_EQ(::_putenv_s("LOGOS_INSTANCE_ID", (prefix + std::to_string(::_getpid())).c_str()), 0);
#else
    ASSERT_EQ(::setenv("LOGOS_INSTANCE_ID", (prefix + std::to_string(::getpid())).c_str(), 1), 0);
#endif
}

Options inprocOptions()
{
    Options options;
    options.name = "plain_host_fixture";
    options.path = fixturePath();
    options.transportSet = R"([{"protocol":"inproc"}])";
    options.credential = "fixture-credential";
    return options;
}

// The engine's principal reaches an in-process provider without a token.
int call(const char* method, std::string* value = nullptr, int timeoutMs = 2000)
{
    lp_client* client = lp_client_create("plain_host_fixture", "@runtime", nullptr, nullptr);
    if (!client) return LP_ERR_UNAVAILABLE;
    char* result = nullptr;
    char* error = nullptr;
    const int status = lp_invoke(client, method, "[]", timeoutMs, &result, &error);
    if (value && result) *value = result;
    lp_string_free(result);
    lp_string_free(error);
    lp_client_destroy(client);
    return status;
}

TEST(NativeModuleHost, ServesAModuleInProcessAndWithdrawsIt)
{
    useInstance("native_host_serve_");
    const Options options = inprocOptions();
    ASSERT_FALSE(options.path.empty()) << "plain_host_fixture not found near " << executableDir();
    Module module;
    std::string error;
    ASSERT_TRUE(module.start(options, error)) << error;

    std::string value;
    ASSERT_EQ(call("ready", &value), LP_OK);
    EXPECT_EQ(value, "true");
    EXPECT_TRUE(module.stop(std::chrono::steady_clock::now() + std::chrono::seconds(3),
                            Teardown::InProcess));
    EXPECT_NE(call("ready", nullptr, 500), LP_OK);
}

TEST(NativeModuleHost, AnAbandonedLoadPublishesNothing)
{
    useInstance("native_host_abandoned_");
    Options options = inprocOptions();
    options.stillWanted = [] { return false; };
    Module module;
    std::string error;
    EXPECT_FALSE(module.start(options, error));
    EXPECT_EQ(error, "load abandoned");
    EXPECT_NE(call("ready", nullptr, 500), LP_OK);
}

// An image without logos_module_set_runtime_delegate cannot run as its own identity.
TEST(NativeModuleHost, ADelegateNeedsTheModulesExport)
{
    useInstance("native_host_delegate_");
    Options options = inprocOptions();
    lp_runtime_delegate_v1* unused = reinterpret_cast<lp_runtime_delegate_v1*>(&options);
    options.delegate = unused;
    Module module;
    std::string error;
    EXPECT_FALSE(module.start(options, error));
    EXPECT_EQ(error, "module has no runtime delegate export; it cannot run in-process");
}

// A call that outlives the deadline keeps everything it may touch, and the host moves on.
TEST(NativeModuleHost, AStuckCallIsLeftRunningNotFreed)
{
    useInstance("native_host_stuck_");
    auto* module = new Module;
    std::string error;
    ASSERT_TRUE(module->start(inprocOptions(), error)) << error;
    std::thread slow([] { (void)call("slow", nullptr, 5000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(module->stop(std::chrono::steady_clock::now() + std::chrono::milliseconds(100),
                              Teardown::InProcess));
    slow.join();
    delete module;
}

} // namespace
