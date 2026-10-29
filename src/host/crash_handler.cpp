// Shared by logos_host_qt and logos_host_plain, so a module crash reads the
// same in the daemon log whichever host ran it.
#include "crash_handler.h"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>

#ifndef _WIN32
#include <execinfo.h>
#include <unistd.h>

namespace {

// Fatal signals that indicate the loaded module faulted.
constexpr int kFatalSignals[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };

// Set once before handlers are installed; only ever read afterwards, so it is
// safe to touch from an async-signal context. Points into main()'s `args`,
// which outlives every point at which a fatal signal can be delivered.
const char* g_crashModuleName = "unknown";

// Alternate stack so a stack-overflow SIGSEGV (a common module crash) can still
// run the handler instead of immediately re-faulting.
char g_altStack[64 * 1024];

// async-signal-safe: write(2) only. Inlines NUL scan and the EINTR /
// partial-write loop so the handler never touches libc symbols whose
// async-signal-safety POSIX does not guarantee — std::strlen, strerror,
// stdio. Best-effort: on persistent write failure we just return; we
// are already in a fatal signal handler about to re-raise.
void safeWrite(const char* s)
{
    if (!s) return;
    std::size_t n = 0;
    while (s[n] != '\0') ++n;
    while (n > 0) {
        const ssize_t w = ::write(STDERR_FILENO, s, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return;
        }
        s += w;
        n -= static_cast<std::size_t>(w);
    }
}

// async-signal-safe hex emit of a pointer value. Avoids snprintf (not on
// the POSIX async-signal-safe list) and any libc string formatting.
void safeWriteHex(const void* p)
{
    char hex[2 + 16 + 1];  // "0x" + 16 hex digits + NUL
    hex[sizeof(hex) - 1] = '\0';
    int idx = static_cast<int>(sizeof(hex)) - 1;
    std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
    if (v == 0) {
        hex[--idx] = '0';
    } else {
        while (v > 0 && idx > 2) {
            const int d = static_cast<int>(v & 0xF);
            hex[--idx] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
            v >>= 4;
        }
    }
    hex[--idx] = 'x';
    hex[--idx] = '0';
    safeWrite(&hex[idx]);
}

// Everything here is async-signal-safe: write(2), backtrace(3) (writes
// only into our own stack array — no libc string allocation), signal(2)
// / raise(3). Deliberately NOT calling backtrace_symbols_fd: it invokes
// dladdr() to resolve symbols, which takes the loader lock. A crash that
// happens while the crashing thread already holds that lock (e.g. inside
// dlopen) would deadlock the handler here. Frames are emitted as raw
// addresses instead — decode after the fact with `atos -p <pid> <addr>`
// (macOS) or `addr2line -e <bin> <addr>` (Linux). Same trade-off
// Chromium / Firefox / Breakpad make for the same reason.
extern "C" void fatalSignalHandler(int sig)
{
    // "FATAL:" makes SubprocessContainer's onOutput classifier log this as
    // critical, so a module crash is unmistakable in the Basecamp log.
    safeWrite("\nFATAL: module '");
    safeWrite(g_crashModuleName);
    safeWrite("' crashed (signal ");

    char numbuf[16];
    int idx = static_cast<int>(sizeof(numbuf));
    numbuf[--idx] = '\0';
    int v = sig;
    if (v == 0) {
        numbuf[--idx] = '0';
    } else {
        while (v > 0 && idx > 0) { numbuf[--idx] = static_cast<char>('0' + v % 10); v /= 10; }
    }
    safeWrite(&numbuf[idx]);
    safeWrite("). Backtrace (raw addresses; decode with atos/addr2line):\n");

    void* frames[64];
    const int n = ::backtrace(frames, 64);
    for (int i = 0; i < n; ++i) {
        safeWrite("  ");
        safeWriteHex(frames[i]);
        safeWrite("\n");
    }
    safeWrite("FATAL: end backtrace\n");

    // Re-raise with the default disposition so the process still dies from the
    // real signal: this keeps WIFSIGNALED true, so SubprocessContainer sees
    // crashed=true and marks the module unloaded (host stays up).
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

} // namespace

void installCrashHandler(const char* moduleName)
{
    g_crashModuleName = (moduleName && *moduleName) ? moduleName : "unknown";

    stack_t ss{};
    ss.ss_sp    = g_altStack;
    ss.ss_size  = sizeof(g_altStack);
    ss.ss_flags = 0;
    ::sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_handler = &fatalSignalHandler;
    sigemptyset(&sa.sa_mask);
    // SA_RESETHAND: if the handler itself faults, the next delivery uses the
    // default disposition and the process dies instead of looping.
    sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
    for (const int s : kFatalSignals) {
        ::sigaction(s, &sa, nullptr);
    }
}

#else  // _WIN32

// No crash handler on Windows, deliberately.
//
// This one is built entirely on POSIX signals + sigaltstack + backtrace(3),
// none of which mingw-w64 has (there is no <execinfo.h>, and SIGBUS does not
// exist). The obvious Win32 translation -- SetUnhandledExceptionFilter plus
// DbgHelp's SymFromAddr -- would REINTRODUCE the exact hazard the comment on
// safeWrite above exists to avoid: symbolisation takes the loader lock, and a
// crash handler that grabs the loader lock deadlocks precisely when it is
// needed most (a fault inside a module dlopen/LoadLibrary).
//
// If Windows backtraces are wanted later, the safe shape is
// CaptureStackBackTrace (lock-free) writing raw hex addresses, symbolised
// offline against the PDB -- mirroring what safeWriteHex already does here.
// Until then a module fault simply terminates the host, which the parent
// already detects and reports.
void installCrashHandler(const char* /*moduleName*/) {}

#endif  // _WIN32
