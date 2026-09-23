// Stops logos_host_plain the way the subprocess container does on Windows:
// WM_QUIT posted to the host's main thread. The module's aboutToUnload must
// run and the host exit 0, not wait out the container's 5 s and be killed.
//
// Usage: logos_host_plain_stop_tests [host.exe fixture.dll]
// (default: both next to this executable)

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

std::wstring besideThisExe(const wchar_t* name)
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1) + name;
}

std::wstring widen(const char* text)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), n);
    return out;
}

// Reads what the host printed so far, up to the deadline or until `needle`.
bool readUntil(HANDLE pipe, std::string& output, const char* needle, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (output.find(needle) == std::string::npos) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available == 0) {
            if (GetTickCount64() >= deadline) return false;
            Sleep(10);
            continue;
        }
        char buffer[4096];
        DWORD got = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr)) return false;
        output.append(buffer, got);
    }
    return true;
}

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

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE inRead, inWrite, outRead, outWrite;
    if (!CreatePipe(&inRead, &inWrite, &inherit, 0) || !CreatePipe(&outRead, &outWrite, &inherit, 0))
        return fail("CreatePipe", {});
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inRead;
    startup.hStdOutput = outWrite;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    std::wstring command = L"\"" + host + L"\" --name plain_host_fixture --path \"" + fixture + L"\"";
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                        &startup, &process))
        return fail("CreateProcess", {});
    CloseHandle(inRead);
    CloseHandle(outWrite);

    DWORD written = 0;
    WriteFile(inWrite, "secret\n", 7, &written, nullptr);

    std::string output;
    int result = 0;
    if (!readUntil(outRead, output, "@logos-load-status ok", 20000)) {
        result = fail("the host did not report a successful load", output);
    } else if (!PostThreadMessageW(process.dwThreadId, WM_QUIT, 0, 0)) {
        std::fprintf(stderr, "PostThreadMessage error %lu\n", GetLastError());
        result = fail("WM_QUIT could not be posted to the host's main thread", output);
    } else if (WaitForSingleObject(process.hProcess, 3000) != WAIT_OBJECT_0) {
        result = fail("the host ignored WM_QUIT for 3 s", output);
    } else {
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        readUntil(outRead, output, "ABOUT_TO_UNLOAD", 0);
        if (code != 0)
            result = fail("the host exited non-zero", output);
        else if (output.find("ABOUT_TO_UNLOAD") == std::string::npos)
            result = fail("the module's aboutToUnload never ran", output);
    }
    if (result != 0) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(inWrite);
    CloseHandle(outRead);
    if (result == 0) std::puts("plain host: WM_QUIT stops it gracefully");
    return result;
}
