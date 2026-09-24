#ifndef LOGOS_HOST_CRASH_HANDLER_H
#define LOGOS_HOST_CRASH_HANDLER_H

// On a fatal signal: "FATAL: module '<name>' crashed" and a raw-address
// backtrace on stderr, then death by the same signal. A no-op on Windows.
void installCrashHandler(const char* moduleName);

#endif
