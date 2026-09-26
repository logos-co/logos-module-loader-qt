// =============================================================================
// Tests for QtPluginFormatLoader — the ModuleFormatLoader implementation that knows how
// to resolve logos_host_qt and build its CLI arguments.
//
// These are pure unit tests: no processes spawned, no filesystem side-effects.
// =============================================================================
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "qt_plugin_format_loader.h"
#include "command_line_parser.h"

class QtPluginFormatLoaderTest : public ::testing::Test {
protected:
    QtPluginFormatLoader loader;
};

// ---------------------------------------------------------------------------
// id
// ---------------------------------------------------------------------------

TEST_F(QtPluginFormatLoaderTest, Id_ReturnsQtPlugin) {
    EXPECT_EQ(loader.id(), "qt-plugin");
}

// ---------------------------------------------------------------------------
// canHandle
// ---------------------------------------------------------------------------

TEST_F(QtPluginFormatLoaderTest, CanHandle_AcceptsQtPluginFormat) {
    LogosCore::ModuleDescriptor desc;
    desc.format = "qt-plugin";
    EXPECT_TRUE(loader.canHandle(desc));
}

TEST_F(QtPluginFormatLoaderTest, CanHandle_AcceptsNativeCdylibFormat) {
    LogosCore::ModuleDescriptor desc;
    desc.format = "native-cdylib";
    EXPECT_TRUE(loader.canHandle(desc));
}

TEST_F(QtPluginFormatLoaderTest, CanHandle_AcceptsEmptyFormat) {
    LogosCore::ModuleDescriptor desc;
    desc.format = "";
    EXPECT_TRUE(loader.canHandle(desc));
}

TEST_F(QtPluginFormatLoaderTest, CanHandle_AcceptsPeerFacadeFormat) {
    LogosCore::ModuleDescriptor desc;
    desc.format = "peer-facade";
    EXPECT_TRUE(loader.canHandle(desc));
}

TEST_F(QtPluginFormatLoaderTest, CanHandle_RejectsWasmFormat) {
    LogosCore::ModuleDescriptor desc;
    desc.format = "wasm";
    EXPECT_FALSE(loader.canHandle(desc));
}

TEST_F(QtPluginFormatLoaderTest, CanHandle_RejectsArbitraryFormat) {
    LogosCore::ModuleDescriptor desc;
    desc.format = "extism";
    EXPECT_FALSE(loader.canHandle(desc));
}

// ---------------------------------------------------------------------------
// buildArguments
// ---------------------------------------------------------------------------

TEST_F(QtPluginFormatLoaderTest, BuildArguments_IncludesNameAndPath) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "my_module";
    desc.path = "/opt/modules/my_module_plugin.so";

    auto args = loader.buildArguments(desc);

    ASSERT_GE(args.size(), 4u);
    EXPECT_EQ(args[0], "--name");
    EXPECT_EQ(args[1], "my_module");
    EXPECT_EQ(args[2], "--path");
    EXPECT_EQ(args[3], "/opt/modules/my_module_plugin.so");
}

TEST_F(QtPluginFormatLoaderTest, BuildArguments_IncludesInstancePersistencePath) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "persisted";
    desc.path = "/lib/persisted.so";
    desc.instancePersistencePath = "/var/logos/instances/abc123";

    auto args = loader.buildArguments(desc);

    ASSERT_EQ(args.size(), 6u);
    EXPECT_EQ(args[4], "--instance-persistence-path");
    EXPECT_EQ(args[5], "/var/logos/instances/abc123");
}

TEST_F(QtPluginFormatLoaderTest, BuildArguments_OmitsInstancePersistenceWhenEmpty) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "simple";
    desc.path = "/lib/simple.so";

    auto args = loader.buildArguments(desc);

    EXPECT_EQ(args.size(), 4u);
}

// ---------------------------------------------------------------------------
// buildArguments — the argv encoding contract
//
// These guard the defect fixed in "stop carrying JSON across the command line":
// Windows' CommandLineToArgvW treats `"` as a quoting delimiter and CONSUMES
// it, so any argv element containing a quote arrives at the child mangled.
// POSIX exec() passes argv through untouched, which is exactly why the bug was
// invisible to this suite's platforms and had to be found on real Windows.
//
// So the load-bearing assertion is not "the value is correct" but "the value
// contains no character the command line can eat". A round-trip test on POSIX
// would have stayed green through the entire outage.
// ---------------------------------------------------------------------------

namespace {

// Returns the value following `flag`, or std::nullopt if the flag is absent.
std::optional<std::string> valueOf(const std::vector<std::string>& args,
                                   const std::string& flag) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag)
            return args[i + 1];
    }
    return std::nullopt;
}

// The characters a Windows command line reconstructs destructively.
void ExpectCommandLineSafe(const std::string& value) {
    EXPECT_EQ(value.find('"'), std::string::npos) << "quote survives in: " << value;
    EXPECT_EQ(value.find('\\'), std::string::npos) << "backslash survives in: " << value;
    EXPECT_EQ(value.find(' '), std::string::npos) << "space survives in: " << value;
}

} // namespace

TEST_F(QtPluginFormatLoaderTest, BuildArguments_GrantsHostServicesToCapabilityModule) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "capability_module";
    desc.path = "/lib/capability_module_plugin.so";

    auto args = loader.buildArguments(desc);

    auto services = valueOf(args, "--host-services");
    ASSERT_TRUE(services.has_value()) << "the trust root must receive its grant";
    // A bare comma-separated list, NOT a JSON array. module_initializer
    // re-serialises it to JSON before stamping the `hostServices` property.
    EXPECT_EQ(*services, "token_delivery");
    ExpectCommandLineSafe(*services);
}

TEST_F(QtPluginFormatLoaderTest, BuildArguments_OmitsHostServicesForOrdinaryModule) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "chat_module";
    desc.path = "/lib/chat_module_plugin.so";

    auto args = loader.buildArguments(desc);

    // Absence of the flag is what keeps an ordinary module fail-closed: no
    // flag means no property, which means no lp_grant_host_services call.
    EXPECT_FALSE(valueOf(args, "--host-services").has_value());
}

TEST_F(QtPluginFormatLoaderTest, BuildArguments_DoesNotGrantDynamicCalls) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "capability_module";
    desc.path = "/lib/capability_module_plugin.so";

    auto args = loader.buildArguments(desc);

    auto services = valueOf(args, "--host-services");
    ASSERT_TRUE(services.has_value());
    // dynamic_calls is elevated but not a trust root; it belongs with the
    // daemon's per-module access policy, not this table.
    EXPECT_EQ(services->find("dynamic_calls"), std::string::npos);
}

TEST_F(QtPluginFormatLoaderTest, BuildArguments_Base64EncodesTransportSet) {
    // Known-answer vectors covering all three padding cases, so a hand-rolled
    // encoder cannot regress on the tail bytes.
    const std::vector<std::pair<std::string, std::string>> vectors = {
        {R"({"t":"qtro"})", "eyJ0IjoicXRybyJ9"},   // len % 3 == 0, no padding
        {R"({"a":1})",      "eyJhIjoxfQ=="},       // len % 3 == 1, "==" padding
        {R"({"ab":1})",     "eyJhYiI6MX0="},       // len % 3 == 2, "=" padding
    };

    for (const auto& [json, expected] : vectors) {
        LogosCore::ModuleDescriptor desc;
        desc.name = "some_module";
        desc.path = "/lib/some_module_plugin.so";
        desc.transportSetJson = json;

        auto args = loader.buildArguments(desc);

        auto encoded = valueOf(args, "--transport-set");
        ASSERT_TRUE(encoded.has_value()) << "for input " << json;
        EXPECT_EQ(*encoded, expected) << "for input " << json;
        // The whole point: the raw JSON never reaches argv.
        EXPECT_NE(*encoded, json);
        ExpectCommandLineSafe(*encoded);
    }
}

TEST_F(QtPluginFormatLoaderTest, BuildArguments_OmitsTransportSetWhenEmpty) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "some_module";
    desc.path = "/lib/some_module_plugin.so";

    auto args = loader.buildArguments(desc);

    EXPECT_FALSE(valueOf(args, "--transport-set").has_value());
}

TEST_F(QtPluginFormatLoaderTest, BuildArguments_ForwardsNativeMultiDispatchPolicy) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "worker";
    desc.path = "/lib/worker.so";
    desc.format = "native-cdylib";
    desc.rawMetadata = {{"concurrency", "multi"}, {"max_workers", 3}};

    const auto args = loader.buildArguments(desc);

    ASSERT_EQ(valueOf(args, "--concurrency"), std::optional<std::string>("multi"));
    ASSERT_EQ(valueOf(args, "--max-workers"), std::optional<std::string>("3"));
}

// A facade has no image to point at, and waits upstream rather than here.
TEST_F(QtPluginFormatLoaderTest, BuildArguments_AFacadeHasNoPathAndTakesManyCalls) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "monerod_module";
    desc.format = "peer-facade";
    desc.transportSetJson = R"([{"protocol":"qt_remote_plain"}])";

    const auto args = loader.buildArguments(desc);

    EXPECT_EQ(valueOf(args, "--name"), std::optional<std::string>("monerod_module"));
    EXPECT_FALSE(valueOf(args, "--path").has_value());
    EXPECT_EQ(valueOf(args, "--concurrency"), std::optional<std::string>("multi"));
    EXPECT_TRUE(valueOf(args, "--transport-set").has_value());
}

TEST_F(QtPluginFormatLoaderTest, BuildArguments_KeepsSingleDispatchAsHostDefault) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "worker";
    desc.path = "/lib/worker.so";
    desc.format = "native-cdylib";
    desc.rawMetadata = {{"concurrency", "single"}, {"max_workers", 3}};

    const auto args = loader.buildArguments(desc);

    EXPECT_FALSE(valueOf(args, "--concurrency").has_value());
    EXPECT_FALSE(valueOf(args, "--max-workers").has_value());
}

TEST(CommandLineParserTest, AcceptsBoundedMultiDispatchPolicy) {
    std::vector<std::string> values = {"logos_host_plain", "--name", "worker", "--path",
                                       "/tmp/worker.so", "--concurrency", "multi",
                                       "--max-workers", "3"};
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    const ModuleArgs args = parseCommandLineArgs(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(args.valid);
    EXPECT_EQ(args.concurrency, "multi");
    EXPECT_EQ(args.maxWorkers, 3);
}

TEST(CommandLineParserTest, RejectsUnknownDispatchPolicy) {
    std::vector<std::string> values = {"logos_host_plain", "--name", "worker", "--path",
                                       "/tmp/worker.so", "--concurrency", "unbounded"};
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    EXPECT_FALSE(parseCommandLineArgs(static_cast<int>(argv.size()), argv.data()).valid);
}

TEST(CommandLineParserTest, RejectsWorkerCapOutsideMultiMode) {
    std::vector<std::string> values = {"logos_host_plain", "--name", "worker", "--path",
                                       "/tmp/worker.so", "--max-workers", "3"};
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    EXPECT_FALSE(parseCommandLineArgs(static_cast<int>(argv.size()), argv.data()).valid);
}
