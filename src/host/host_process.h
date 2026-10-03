#ifndef LOGOS_HOST_PROCESS_H
#define LOGOS_HOST_PROCESS_H

// The process side of a module host, shared by logos_host_plain and other
// Qt-free hosts (logos_host_remote): following the parent, the load-status
// line the container reads, and the stop the container sends.

#include <string>

namespace logos::host_process {

// First thing in main(): its own session, exit with the parent, and on
// Windows the message queue the container's WM_QUIT needs.
void prepare();

// "ok", or "failed <reason>", as one status line on stdout.
void reportLoadStatus(bool ok, std::string reason = {});

// SIGTERM/SIGINT (console events or WM_QUIT on Windows) end waitForStop().
void installStopHandlers();
void waitForStop();

} // namespace logos::host_process

#endif
