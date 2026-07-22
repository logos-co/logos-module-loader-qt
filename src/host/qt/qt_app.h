#ifndef QT_APP_H
#define QT_APP_H

namespace QtApp {
    void init(int argc, char* argv[]);
    // Install SIGTERM/SIGINT handlers that quit the event loop so exec()
    // returns and the module's teardown (which unlinks its local socket) runs.
    // Call after init().
    void installSignalHandlers();
    int exec();
    void cleanup();
}

#endif // QT_APP_H
