#include "host_process.h"

#include "parent_lifetime.h"

#include <logos_container/load_status.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

namespace logos::host_process {
namespace {

std::atomic<bool> gStop{false};

#ifdef _WIN32
BOOL WINAPI consoleHandler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT
        || event == CTRL_SHUTDOWN_EVENT) {
        gStop = true;
        return TRUE;
    }
    return FALSE;
}
#else
extern "C" void stopHandler(int) { gStop = true; }
#endif

} // namespace

void prepare()
{
    isolateAndFollowParent();
#ifdef _WIN32
    // The container stops a host by posting WM_QUIT to its main thread, which
    // fails unless that thread already has a message queue.
    MSG msg;
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
#endif
}

void reportLoadStatus(bool ok, std::string reason)
{
    for (char& c : reason)
        if (c == '\n' || c == '\r') c = ' ';
    if (ok)
        std::fprintf(stdout, "\n%s %s\n", LogosCore::kLoadStatusPrefix, LogosCore::kLoadStatusOk);
    else
        std::fprintf(stdout, "\n%s %s %s\n", LogosCore::kLoadStatusPrefix,
                     LogosCore::kLoadStatusFailed, reason.c_str());
    std::fflush(stdout);
}

void installStopHandlers()
{
#ifdef _WIN32
    SetConsoleCtrlHandler(&consoleHandler, TRUE);
#else
    struct sigaction action{};
    action.sa_handler = &stopHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
#endif
}

void waitForStop()
{
#ifdef _WIN32
    MSG msg;
    while (!gStop.load()) {
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 25, QS_ALLPOSTMESSAGE);
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            if (msg.message == WM_QUIT) gStop = true;
    }
#else
    while (!gStop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(25));
#endif
}

} // namespace logos::host_process
