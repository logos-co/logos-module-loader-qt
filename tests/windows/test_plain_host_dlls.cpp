// A plain module shipped the way modules wrapping a C library ship: in its
// own directory, importing a DLL beside it and loading another by bare name
// at runtime. Once under an ASCII path and once under a non-ASCII one, which
// the token file beside it shares.
//
// Usage: logos_host_plain_dll_tests [host.exe]
// (default: next to this executable; fixtures in dll-search-fixtures/)

#include "plain_host_process.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace plain_host_test;
namespace fs = std::filesystem;

namespace {

bool runCase(const std::wstring& host, const fs::path& root, const wchar_t* directory)
{
    const fs::path fixtures = fs::path(besideThisExe(L"dll-search-fixtures"));
    const fs::path moduleDir = root / directory;
    fs::create_directories(moduleDir);
    for (const wchar_t* dll : {L"plain_host_fixture_deps.dll", L"logos_dll_search_leaf.dll",
                               L"logos_dll_search_late.dll"})
        fs::copy_file(fixtures / dll, moduleDir / dll);
    const fs::path token = moduleDir / L"token.txt";
    std::ofstream(token) << "secret\n";

    const std::string label = moduleDir.filename().u8string();
    PlainHost process;
    // The working directory is not the module's: nothing may be found there.
    const std::wstring command = quoted(host) + L" --name plain_host_fixture --path "
        + quoted((moduleDir / L"plain_host_fixture_deps.dll").native())
        + L" --token-source " + quoted(L"file:" + token.native());
    if (!process.start(command, root.c_str())) {
        std::fprintf(stderr, "FAIL [%s]: CreateProcess error %lu\n", label.c_str(), GetLastError());
        return false;
    }
    const bool loaded = process.readUntil("@logos-load-status ok", 20000);
    const bool deps = process.printed().find("DEPS leaf=41 late=42") != std::string::npos;
    process.postQuit();
    DWORD code = 1;
    process.waitForExit(5000, &code);
    if (!loaded || !deps) {
        std::fprintf(stderr, "FAIL [%s]: %s\n--- host stdout ---\n%s\n", label.c_str(),
                     loaded ? "a dependency did not resolve" : "the module did not load",
                     process.printed().c_str());
        return false;
    }
    std::printf("PASS [%s]: import from its directory and late bare-name load\n", label.c_str());
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const std::wstring host = argc > 1 ? widen(argv[1]) : besideThisExe(L"logos_host_plain.exe");
    SetEnvironmentVariableW(L"LOGOS_INSTANCE_ID",
                            (L"plain_host_dlls_" + std::to_wstring(GetCurrentProcessId())).c_str());
    const fs::path root = fs::temp_directory_path()
        / (L"logos-plain-dlls-" + std::to_wstring(GetCurrentProcessId()) + L"-"
           + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);

    bool ok = runCase(host, root, L"module with spaces");
    ok = runCase(host, root, L"module λ with spaces") && ok;

    std::error_code ignored;
    fs::remove_all(root, ignored);
    return ok ? 0 : 1;
}
