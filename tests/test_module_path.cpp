// =============================================================================
// Tests for how the host turns a --path argument into a plugin file.
//
// The regression pinned here: logos_host_qt refused every RELATIVE --path, and
// lied about why. Qt is the mechanism — QPluginLoader::setFileName() resolves a
// relative file name against QCoreApplication::libraryPaths(), the Qt PLUGIN
// search path, and never against the process working directory — but the
// message it produces is "The shared library was not found.", which reads as a
// missing transitive dependency of a plugin that WAS found. The file is right
// there and the host says it is not. That sentence cost two wrong diagnoses in
// one session.
//
// The daemon always passes an absolute path (ModuleRegistry::modulePath), so no
// shipped frontend hit this; anyone driving the host by hand did.
//
// Two halves, and both are tested, because either alone leaves the trap:
//   1. parseCommandLineArgs() resolves a relative --path against the working
//      directory, so the loader is only ever handed an absolute one.
//   2. ModulePath::fileProblem() separates "there is no such file" from "the
//      file is there and the loader refused it", so the host says which even
//      when a path arrives that it cannot resolve.
// =============================================================================
#include <gtest/gtest.h>

#include "command_line_parser.h"
#include "module_path.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Restores the working directory on the way out. These tests chdir() to give
// "relative" a fixed meaning, and the rest of this binary must not inherit it
// (test_token_source's Windows branch makes its temp files relative to the cwd).
class ScopedCwd {
public:
    explicit ScopedCwd(const fs::path& dir) : m_previous(fs::current_path()) {
        fs::current_path(dir);
    }
    ~ScopedCwd() {
        std::error_code ec;
        fs::current_path(m_previous, ec);
    }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;

private:
    fs::path m_previous;
};

// A private directory under the system temp dir, removed at the end of the
// test. Named per test so a leftover from a crashed run cannot make the next
// one pass or fail for the wrong reason.
class TempDir {
public:
    explicit TempDir(const std::string& label)
        : m_path(fs::temp_directory_path() / ("logos_module_path_" + label)) {
        std::error_code ec;
        fs::remove_all(m_path, ec);
        fs::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

// Stands in for a plugin. Nothing here loads it — these tests are about
// locating a file, which is the half that was broken.
void writeFile(const fs::path& path, const std::string& contents) {
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

// parseCommandLineArgs() takes the real (int, char*[]) an OS hands main(), so
// build one. argv[0] is the program name, which CLI11 consumes as such.
ModuleArgs parseArgs(const std::vector<std::string>& args) {
    std::vector<std::string> owned{"logos_host_qt"};
    owned.insert(owned.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(owned.size());
    for (std::string& arg : owned)
        argv.push_back(arg.data());  // C++17: data() is char*, and NUL-terminated

    return parseCommandLineArgs(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

// -----------------------------------------------------------------------------
// ModulePath::resolve
// -----------------------------------------------------------------------------

TEST(ModulePathResolve, MakesARelativePathAbsolute) {
    TempDir dir("resolve_relative");
    ScopedCwd cwd(dir.path());
    writeFile("modules/demo/demo_plugin.so", "not really a plugin");

    const std::string resolved = ModulePath::resolve("./modules/demo/demo_plugin.so");

    EXPECT_TRUE(fs::path(resolved).is_absolute()) << resolved;
    ASSERT_TRUE(fs::exists(resolved)) << resolved;
    EXPECT_TRUE(fs::equivalent(resolved, dir.path() / "modules/demo/demo_plugin.so"));
}

TEST(ModulePathResolve, LeavesAnAbsolutePathByteIdentical) {
    // The daemon's own paths go through here. Resolution must add the working
    // directory and nothing else -- no collapsing "..", no following symlinks --
    // or an absolute path that works today could come out naming a different file.
    const std::string given =
        (fs::temp_directory_path() / "logos" / ".." / "logos" / "demo_plugin.so").string();

    EXPECT_EQ(given, ModulePath::resolve(given));
}

TEST(ModulePathResolve, ResolvesAPathWhoseFileIsNotThere) {
    // The resolved path is what the "no plugin file at ..." message names, so it
    // has to be produced whether or not the file exists.
    TempDir dir("resolve_missing");
    ScopedCwd cwd(dir.path());

    const std::string resolved = ModulePath::resolve("nowhere/absent_plugin.so");

    EXPECT_TRUE(fs::path(resolved).is_absolute()) << resolved;
    EXPECT_FALSE(fs::exists(resolved)) << resolved;
}

TEST(ModulePathResolve, DropsRedundantDotComponents) {
    // The resolved path is what the host echoes back when it cannot load the
    // file. A stray "/./" in it is one more thing to reconcile against `ls`
    // output, and this bug already cost enough reading of error messages.
    TempDir dir("resolve_dot");
    ScopedCwd cwd(dir.path());
    writeFile("modules/demo_plugin.so", "not really a plugin");

    const std::string resolved = ModulePath::resolve("./modules/demo_plugin.so");

    EXPECT_EQ(std::string::npos, resolved.find("/./")) << resolved;
    ASSERT_TRUE(fs::exists(resolved)) << resolved;
    EXPECT_TRUE(fs::equivalent(resolved, dir.path() / "modules/demo_plugin.so"));
}

TEST(ModulePathResolve, KeepsDotDotRatherThanCollapsingIt) {
    // The other half of the same rule, and the reason it is not just
    // lexically_normal(): collapsing ".." names a DIFFERENT file whenever a
    // symlink sits above it. Adding the working directory is this function's
    // whole job; deciding which file is the loader's.
    TempDir dir("resolve_dotdot");
    fs::create_directories(dir.path() / "sub");
    writeFile(dir.path() / "demo_plugin.so", "not really a plugin");
    ScopedCwd cwd(dir.path() / "sub");

    const std::string resolved = ModulePath::resolve("../demo_plugin.so");

    EXPECT_NE(std::string::npos, resolved.find("..")) << resolved;
    ASSERT_TRUE(fs::exists(resolved)) << resolved;
    EXPECT_TRUE(fs::equivalent(resolved, dir.path() / "demo_plugin.so"));
}

TEST(ModulePathResolve, LeavesAnEmptyPathEmpty) {
    // "" is not a working directory reference; turning it into one would report
    // the cwd as the plugin path in the error message.
    EXPECT_EQ("", ModulePath::resolve(""));
}

// -----------------------------------------------------------------------------
// ModulePath::fileProblem — the honest half of the failure
// -----------------------------------------------------------------------------

TEST(ModulePathFileProblem, IsSilentWhenTheFileIsThere) {
    // Silence is the contract: it means the loader's verdict is the one to
    // report, not this one. Note the file is NOT a plugin -- deciding that is
    // the loader's job, and claiming it here would re-merge the two answers.
    TempDir dir("problem_present");
    writeFile(dir.path() / "demo_plugin.so", "not really a plugin");

    EXPECT_EQ("", ModulePath::fileProblem((dir.path() / "demo_plugin.so").string()));
}

TEST(ModulePathFileProblem, SaysAMissingFileIsMissing) {
    TempDir dir("problem_missing");
    const std::string missing = (dir.path() / "absent_plugin.so").string();

    const std::string problem = ModulePath::fileProblem(missing);

    EXPECT_NE(std::string::npos, problem.find("no plugin file")) << problem;
    EXPECT_NE(std::string::npos, problem.find(missing)) << problem;
    // The whole point. Qt's wording for an unresolvable path blames a *library*
    // the plugin needs; this one has to blame the missing file, or the reader
    // goes hunting for a dependency that was never the problem.
    EXPECT_EQ(std::string::npos, problem.find("shared library")) << problem;
}

TEST(ModulePathFileProblem, RefusesADirectory) {
    TempDir dir("problem_directory");
    fs::create_directories(dir.path() / "modules_state");

    const std::string problem = ModulePath::fileProblem((dir.path() / "modules_state").string());

    EXPECT_NE("", problem);
    EXPECT_NE(std::string::npos, problem.find("directory")) << problem;
}

// -----------------------------------------------------------------------------
// parseCommandLineArgs — the regression itself, on the production path
// -----------------------------------------------------------------------------

TEST(CommandLineParserPath, ResolvesARelativeModulePathAgainstTheWorkingDirectory) {
    TempDir dir("parse_relative");
    ScopedCwd cwd(dir.path());
    writeFile("modules/demo/demo_plugin.so", "not really a plugin");

    const ModuleArgs args =
        parseArgs({"--name", "demo", "--path", "./modules/demo/demo_plugin.so"});

    ASSERT_TRUE(args.valid);
    EXPECT_EQ("demo", args.name);
    EXPECT_TRUE(fs::path(args.path).is_absolute()) << args.path;
    ASSERT_TRUE(fs::exists(args.path)) << args.path;
    EXPECT_TRUE(fs::equivalent(args.path, dir.path() / "modules/demo/demo_plugin.so"));
}

TEST(CommandLineParserPath, ResolvesABareFileNameAgainstTheWorkingDirectory) {
    // No "./" to hint at it. This is the form Qt is most likely to find
    // SOMEWHERE on its plugin path, which would be worse than failing: a
    // different file than the one the caller pointed at.
    TempDir dir("parse_bare_name");
    ScopedCwd cwd(dir.path());
    writeFile("demo_plugin.so", "not really a plugin");

    const ModuleArgs args = parseArgs({"--name", "demo", "--path", "demo_plugin.so"});

    ASSERT_TRUE(args.valid);
    EXPECT_TRUE(fs::path(args.path).is_absolute()) << args.path;
    ASSERT_TRUE(fs::exists(args.path)) << args.path;
    EXPECT_TRUE(fs::equivalent(args.path, dir.path() / "demo_plugin.so"));
}

TEST(CommandLineParserPath, LeavesAnAbsoluteModulePathAlone) {
    TempDir dir("parse_absolute");
    const std::string given = (dir.path() / "demo_plugin.so").string();

    const ModuleArgs args = parseArgs({"--name", "demo", "--path", given});

    ASSERT_TRUE(args.valid);
    EXPECT_EQ(given, args.path);
}

TEST(CommandLineParserPath, ResolutionDoesNotDisturbTheOtherArguments) {
    TempDir dir("parse_other_args");
    ScopedCwd cwd(dir.path());
    writeFile("demo_plugin.so", "not really a plugin");

    const ModuleArgs args = parseArgs({"--name", "capability_module",
                                       "--path", "demo_plugin.so",
                                       "--token-source", "fd:7",
                                       "--host-services", "token_registry,token_delivery",
                                       "--instance-persistence-path", "/var/lib/logos/inst-1"});

    ASSERT_TRUE(args.valid);
    EXPECT_EQ("capability_module", args.name);
    EXPECT_EQ("fd:7", args.tokenSource);
    EXPECT_EQ("token_registry,token_delivery", args.hostServices);
    EXPECT_EQ("/var/lib/logos/inst-1", args.instancePersistencePath);
    EXPECT_TRUE(fs::path(args.path).is_absolute()) << args.path;
}
