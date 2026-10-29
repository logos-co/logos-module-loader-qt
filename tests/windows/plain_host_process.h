#pragma once

// logos_host_plain.exe run the way the subprocess container runs it: token
// on stdin, load status on stdout, WM_QUIT to its main thread to stop it.

#include <windows.h>

#include <string>

namespace plain_host_test {

inline std::wstring besideThisExe(const wchar_t* name)
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1) + name;
}

inline std::wstring widen(const char* text)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), n);
    return out;
}

inline std::wstring quoted(const std::wstring& arg) { return L"\"" + arg + L"\""; }

class PlainHost {
public:
    ~PlainHost()
    {
        if (m_process.hProcess) {
            if (WaitForSingleObject(m_process.hProcess, 0) != WAIT_OBJECT_0)
                TerminateProcess(m_process.hProcess, 1);
            CloseHandle(m_process.hThread);
            CloseHandle(m_process.hProcess);
        }
        if (m_input) CloseHandle(m_input);
        if (m_output) CloseHandle(m_output);
    }

    bool start(std::wstring commandLine, const wchar_t* workingDirectory = nullptr)
    {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE inRead, outWrite;
        if (!CreatePipe(&inRead, &m_input, &inherit, 0)
            || !CreatePipe(&m_output, &outWrite, &inherit, 0))
            return false;
        SetHandleInformation(m_input, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(m_output, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = inRead;
        startup.hStdOutput = outWrite;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        const BOOL started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                            0, nullptr, workingDirectory, &startup, &m_process);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        if (!started) return false;
        DWORD written = 0;
        return WriteFile(m_input, "secret\n", 7, &written, nullptr) != 0;
    }

    // Collects stdout until `needle` appears or the deadline passes.
    bool readUntil(const char* needle, DWORD timeoutMs)
    {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (m_printed.find(needle) == std::string::npos) {
            DWORD available = 0;
            if (!PeekNamedPipe(m_output, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (available == 0) {
                if (GetTickCount64() >= deadline) return false;
                Sleep(10);
                continue;
            }
            char buffer[4096];
            DWORD got = 0;
            if (!ReadFile(m_output, buffer, sizeof(buffer), &got, nullptr)) return false;
            m_printed.append(buffer, got);
        }
        return true;
    }

    bool postQuit() const { return PostThreadMessageW(m_process.dwThreadId, WM_QUIT, 0, 0) != 0; }

    // True once exited within the timeout; *exitCode is its status.
    bool waitForExit(DWORD timeoutMs, DWORD* exitCode)
    {
        if (WaitForSingleObject(m_process.hProcess, timeoutMs) != WAIT_OBJECT_0) return false;
        GetExitCodeProcess(m_process.hProcess, exitCode);
        readUntil("\x01", 0);  // drain whatever is left
        return true;
    }

    const std::string& printed() const { return m_printed; }

private:
    PROCESS_INFORMATION m_process{};
    HANDLE m_input = nullptr;
    HANDLE m_output = nullptr;
    std::string m_printed;
};

} // namespace plain_host_test
