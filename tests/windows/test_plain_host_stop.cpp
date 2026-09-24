// Stops logos_host_plain the way the subprocess container does on Windows:
// WM_QUIT posted to the host's main thread. The module's aboutToUnload must
// run and the host exit 0, not wait out the container's 5 s and be killed.
//
// Usage: logos_host_plain_stop_tests [host.exe fixture.dll]
// (default: both next to this executable)

#include "plain_host_process.h"

#include <cstdio>

using namespace plain_host_test;

namespace {

int fail(const char* what, const std::string& output)
{
    std::fprintf(stderr, "FAIL: %s\n--- host stdout ---\n%s\n", what, output.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    const std::wstring host = argc > 2 ? widen(argv[1]) : besideThisExe(L"logos_host_plain.exe");
    const std::wstring fixture = argc > 2 ? widen(argv[2]) : besideThisExe(L"plain_host_fixture.dll");
    SetEnvironmentVariableW(L"LOGOS_INSTANCE_ID",
                            (L"plain_host_stop_" + std::to_wstring(GetCurrentProcessId())).c_str());

    PlainHost process;
    if (!process.start(quoted(host) + L" --name plain_host_fixture --path " + quoted(fixture)))
        return fail("CreateProcess", {});
    if (!process.readUntil("@logos-load-status ok", 20000))
        return fail("the host did not report a successful load", process.printed());
    if (!process.postQuit()) {
        std::fprintf(stderr, "PostThreadMessage error %lu\n", GetLastError());
        return fail("WM_QUIT could not be posted to the host's main thread", process.printed());
    }
    DWORD code = 1;
    if (!process.waitForExit(3000, &code))
        return fail("the host ignored WM_QUIT for 3 s", process.printed());
    if (code != 0) return fail("the host exited non-zero", process.printed());
    if (process.printed().find("ABOUT_TO_UNLOAD") == std::string::npos)
        return fail("the module's aboutToUnload never ran", process.printed());
    std::puts("plain host: WM_QUIT stops it gracefully");
    return 0;
}
