// =============================================================================
// Tests for QtPluginFormatLoader — the ModuleFormatLoader implementation that knows how
// to resolve logos_host_qt and build its CLI arguments.
//
// These are pure unit tests: no processes spawned, no filesystem side-effects.
// =============================================================================
#include <gtest/gtest.h>
#include "qt_plugin_format_loader.h"

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

TEST_F(QtPluginFormatLoaderTest, CanHandle_AcceptsEmptyFormat) {
    LogosCore::ModuleDescriptor desc;
    desc.format = "";
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
