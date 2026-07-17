#include "qt_app.h"
#include <QCoreApplication>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace {
    QCoreApplication* s_app = nullptr;

    // Self-pipe trick: the signal handler must be async-signal-safe, so it does
    // nothing but write() one byte to a pipe. A QSocketNotifier on the read end
    // wakes the Qt event loop on its own thread and quits the app cleanly.
    int s_sigPipe[2] = { -1, -1 };

    // async-signal-safe: write(2) only.
    extern "C" void onTerminationSignal(int /*sig*/) {
        if (s_sigPipe[1] < 0) return;
        const char byte = 1;
        ssize_t rc;
        do {
            rc = ::write(s_sigPipe[1], &byte, 1);
        } while (rc < 0 && errno == EINTR);
        (void)rc;
    }
}

namespace QtApp {

    void init(int argc, char* argv[]) {
        s_app = new QCoreApplication(argc, argv);
    }

    // Make SIGTERM / SIGINT quit the event loop so exec() returns and the
    // caller's `delete logos_api` runs. Without this the module subprocess dies
    // by the default signal disposition mid-exec(): the destructor chain never
    // runs, so QtRO's QLocalServer never unlinks its socket — every module
    // leaks its /tmp/logos_<name>_<instance> file on every clean shutdown. Must
    // be called after init() (needs the QCoreApplication for the notifier).
    void installSignalHandlers() {
        if (!s_app) return;
        if (::pipe(s_sigPipe) != 0) return;
        for (int fd : s_sigPipe) {
            ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        }

        auto* notifier = new QSocketNotifier(s_sigPipe[0], QSocketNotifier::Read, s_app);
        QObject::connect(notifier, &QSocketNotifier::activated, s_app, []() {
            char buf[16];
            while (::read(s_sigPipe[0], buf, sizeof(buf)) > 0) { /* drain */ }
            QCoreApplication::quit();
        });

        struct sigaction sa{};
        sa.sa_handler = &onTerminationSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        ::sigaction(SIGTERM, &sa, nullptr);
        ::sigaction(SIGINT, &sa, nullptr);
    }

    int exec() {
        if (!s_app) return -1;
        return s_app->exec();
    }

    void cleanup() {
        delete s_app;
        s_app = nullptr;
    }

}
