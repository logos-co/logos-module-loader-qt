#include "parent_lifetime.h"

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <thread>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

void isolateAndFollowParent()
{
#ifndef _WIN32
    // Its own group, so signals aimed at the daemon's group (and whoever
    // launched the daemon) never reach module hosts.
    if (::setsid() == -1 && errno != EPERM) ::setpgid(0, 0);

    const pid_t daemon = ::getppid();
#ifdef __linux__
    // Fires when the spawning THREAD exits; the container spawns from a
    // thread that lives as long as the daemon.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    // The daemon may have died before PDEATHSIG was armed.
    if (::getppid() != daemon) ::_exit(0);
    // Everywhere else (macOS), and as a backup: exit once we are reparented.
    std::thread([daemon] {
        while (::getppid() == daemon) ::sleep(1);
        ::_exit(0);
    }).detach();
#endif
}
