// Which host binary a module is started with. Unlike the other loader tests
// these touch the filesystem: the candidates must exist to be chosen.
#include <gtest/gtest.h>

#include "qt_plugin_format_loader.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kExe = ".exe";
#else
constexpr const char* kExe = "";
#endif

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) ::setenv(name, value, 1);
    else ::unsetenv(name);
#endif
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::optional<std::string>& value) : m_name(name)
    {
        if (const char* current = std::getenv(name)) m_previous = current;
        setEnv(name, value ? value->c_str() : nullptr);
    }
    ~ScopedEnv() { setEnv(m_name, m_previous ? m_previous->c_str() : nullptr); }

private:
    const char* m_name;
    std::optional<std::string> m_previous;
};

class HostDirectory {
public:
    HostDirectory()
        : m_path(fs::temp_directory_path()
                 / ("logos_host_resolution_"
                    + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        fs::create_directories(m_path);
        for (const char* name : {"logos_host_qt", "logos_host_plain"})
            std::ofstream(m_path / (std::string(name) + kExe)) << "";
    }
    ~HostDirectory()
    {
        std::error_code ignored;
        fs::remove_all(m_path, ignored);
    }
    fs::path host(const char* name) const { return m_path / (std::string(name) + kExe); }

private:
    fs::path m_path;
};

} // namespace

// Detector: a bundle that sets LOGOS_HOST_PATH ships logos_host_plain beside
// it, and plain modules still failed with "logos_host_plain not found".
TEST(HostResolution, APlainModuleUsesThePlainHostBesideLogosHostPath)
{
    HostDirectory hosts;
    ScopedEnv qt("LOGOS_HOST_PATH", hosts.host("logos_host_qt").string());
    ScopedEnv plain("LOGOS_HOST_PLAIN_PATH", std::nullopt);
    LogosCore::ModuleDescriptor desc;
    desc.format = "native-cdylib";
    desc.modulesDirs = {(fs::temp_directory_path() / "logos_no_such_modules").string()};
    EXPECT_EQ(fs::path(QtPluginFormatLoader().resolveHostBinary(desc)),
              hosts.host("logos_host_plain"));
}

TEST(HostResolution, LogosHostPlainPathStillWins)
{
    HostDirectory configured;
    HostDirectory beside;
    ScopedEnv qt("LOGOS_HOST_PATH", beside.host("logos_host_qt").string());
    ScopedEnv plain("LOGOS_HOST_PLAIN_PATH", configured.host("logos_host_plain").string());
    LogosCore::ModuleDescriptor desc;
    desc.format = "native-cdylib";
    EXPECT_EQ(fs::path(QtPluginFormatLoader().resolveHostBinary(desc)),
              configured.host("logos_host_plain"));
}
