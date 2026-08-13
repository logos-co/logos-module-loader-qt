#include "command_line_parser.h"
#include "module_initializer.h"
#include "qt/qt_app.h"
#include "token_source.h"
#include "logos_api.h"
#include "interface.h"

#include <QtGlobal>          // qInstallMessageHandler, QtMsgType
#include <QMessageLogContext>
#include <QString>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <thread>

#include <unistd.h>
#ifndef _WIN32
#include <execinfo.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace {

#ifndef _WIN32

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

// Send Qt's own logging to stderr, always.
//
// Without a handler installed, Qt picks a backend for us, and a Qt built with
// journald support routes qDebug/qInfo/qWarning there instead of stderr once
// stderr is not a terminal -- which is exactly the case for a module host: the
// daemon spawns it with its stdio inherited from a pipe or a file.
//
// The effect is that everything a module logs vanishes from the daemon's log.
// Measured on a logosctl session: 34 module lines went to journald and 0 to
// the daemon's log file, and forcing stderr took the module lines in one
// install from 3 to 18. Every frontend (logoscore, logosctl, basecamp)
// installs a handler and so keeps its own output; the host was the only
// process in the tree that did not, and it is the one whose output an operator
// most wants when a module misbehaves.
//
// Nothing here decides WHAT is worth logging -- the frontends filter by
// verbosity at their end, and the daemon captures this stream. The host's job
// is only to make sure the stream reaches its parent at all.
void hostMessageHandler(QtMsgType type, const QMessageLogContext &context,
                        const QString &msg)
{
    const QByteArray text = msg.toLocal8Bit();
    const char *level = "Info";
    switch (type) {
    case QtDebugMsg:    level = "Debug";    break;
    case QtInfoMsg:     level = "Info";     break;
    case QtWarningMsg:  level = "Warning";  break;
    case QtCriticalMsg: level = "Critical"; break;
    case QtFatalMsg:    level = "Fatal";    break;
    }
    if (type == QtCriticalMsg || type == QtFatalMsg) {
        // Only the serious ones are worth the file/line noise.
        // %d, not %u: QMessageLogContext::line is an int.
        fprintf(stderr, "%s: %s (%s:%d)\n", level, text.constData(),
                context.file ? context.file : "", context.line);
    } else {
        fprintf(stderr, "%s: %s\n", level, text.constData());
    }
    fflush(stderr);
    if (type == QtFatalMsg) {
        abort();
    }
}

} // namespace

int main(int argc, char *argv[])
{
#ifndef _WIN32
    // Isolate this module subprocess into its own session/process group, up
    // front. The host is spawned in the daemon's process group, which is in turn
    // the group of whatever launched the daemon (a shell, a script). Leaving the
    // module tree in that shared group means tearing it down on shutdown — or any
    // process-group signal aimed at the daemon — leaks into the launcher and can
    // kill the shell driving it (a script's teardown step dies with exit -15 on
    // Linux). The daemon itself deliberately stays in the foreground / its
    // launcher's group so process managers (systemd, Docker) and shells keep
    // managing it normally; only its workers detach. A freshly forked child is
    // never a group leader, so setsid() succeeds; on the off chance it doesn't,
    // setpgid() still gives us an isolated group.
    if (::setsid() == -1 && errno != EPERM) {
        ::setpgid(0, 0);
    }
    // Tie this worker's lifetime to the daemon's. If the daemon (our parent) dies
    // WITHOUT cleaning us up — i.e. it crashes — make sure we don't linger as an
    // orphan. setsid() above detached us from the controlling terminal, removing
    // the incidental SIGHUP that used to reap orphans, so we replace it: graceful
    // shutdown still kills us per-PID from the daemon, and now a daemon *crash*
    // cleans us up too.
    {
        const pid_t daemon_pid = ::getppid();
#ifdef __linux__
        // Kernel-level + immediate. PR_SET_PDEATHSIG fires when the thread that
        // forked us exits; that is the daemon's long-lived event-loop/io thread,
        // so it only fires on real daemon death.
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
        // Race guard: if the daemon already died between fork() and now, our
        // parent has changed — exit. Compare against the daemon's actual pid (not
        // pid 1) so a daemon that is itself PID 1 (a container) is handled right.
        if (::getppid() != daemon_pid) {
            _exit(0);
        }
        // Portable watchdog (covers platforms without PR_SET_PDEATHSIG, e.g.
        // macOS, and backs it up elsewhere): exit if our parent changes.
        std::thread([daemon_pid] {
            while (::getppid() == daemon_pid) {
                ::sleep(1);
            }
            _exit(0);
        }).detach();
    }
#endif

    ModuleArgs args = parseCommandLineArgs(argc, argv);
    if (!args.valid) {
        return 1;
    }

    installCrashHandler(args.name.c_str());

    // Before QtApp::init, so that anything Qt says while starting up lands on
    // stderr too rather than in whichever backend it would have chosen.
    qInstallMessageHandler(hostMessageHandler);

    QtApp::init(argc, argv);

    // Quit cleanly on SIGTERM/SIGINT (the signals the subprocess container and
    // an operator send at shutdown) so exec() returns and `delete logos_api`
    // below runs its destructor chain — which is what unlinks this module's
    // QtRO local socket. Without it the host dies mid-exec() by default signal
    // disposition and leaks /tmp/logos_<name>_<instance> on every shutdown.
    QtApp::installSignalHandlers();

    // Read the auth token from the channel our container designated via
    // --token-source (default: stdin). The host is deliberately agnostic to
    // which container spawned it — it just reads bytes from an OS handle, so it
    // depends on no container implementation. The subprocess container writes
    // the token to our stdin; a Docker/sandbox container could pass fd:<n> or
    // file:<path> instead, with no change here.
    std::string authToken = TokenSource::read(args.tokenSource);
    if (authToken.empty()) {
        return 1;
    }

    // Runtime concern: load the Qt plugin and initialize LogosAPI.
    ModuleLib::LogosModule module = loadModule(args.path, args.name);
    if (!module.isValid()) {
        return 1;
    }

    PluginInterface* basePlugin = module.as<PluginInterface>();
    LogosAPI* logos_api = initializeLogosAPI(args.name, module.instance(),
                                             basePlugin, authToken, args.hostServices,
                                             args.path,
                                             args.instancePersistencePath,
                                             args.transportSetJson);
    module.release();

    if (!logos_api) {
        return 1;
    }

    int result = QtApp::exec();
    delete logos_api;
    QtApp::cleanup();

    return result;
}
