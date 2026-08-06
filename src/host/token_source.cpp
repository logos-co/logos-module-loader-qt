#include "token_source.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <unistd.h>
#ifndef _WIN32
#include <poll.h>
#else
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#endif

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <string>

namespace TokenSource {

namespace {

// Reduce raw bytes to the token: take everything up to the first newline (the
// token is one line — a container may leave trailing bytes on the channel),
// then drop a trailing '\r' from a CRLF framing.
void firstLine(std::string& s) {
    const std::size_t nl = s.find('\n');
    if (nl != std::string::npos) s.resize(nl);
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

#ifdef _WIN32

// mingw-w64 ships <unistd.h> (read/close) but NOT <poll.h>, and Win32 has no
// single readiness wait that covers all three things stdin can be here: an
// anonymous pipe from the subprocess container, a redirected file, or a
// console. (WaitForSingleObject does not signal readability on a pipe handle;
// PeekNamedPipe does not work on files or consoles.)
//
// So do the blocking read on a detached thread and bound the WAIT instead of
// the read. That behaves identically for all three channel kinds.
//
// The shared state is a shared_ptr so a thread that wakes up after we have
// already timed out writes into memory that is still alive. Detaching is safe
// here specifically because the token is required at startup: a timeout means
// the caller returns {} and the host exits immediately afterwards.
//
// Slight departure from this file's "plain libc" note in the header: it now
// also uses <thread>/<mutex>, which is still standard C++ and adds no library
// dependency.
std::string readFdUntilNewlineOrEof(int fd, int timeout_ms) {
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        std::string out;
        bool done = false;
    };
    auto sh = std::make_shared<Shared>();

    std::thread([sh, fd] {
        std::string local;
        char buf[256];
        for (;;) {
            const int n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                local.append(buf, static_cast<std::size_t>(n));
                if (local.find('\n') != std::string::npos) break;  // full line
                continue;
            }
            break;  // EOF (0) or error (<0): token is whatever arrived
        }
        {
            std::lock_guard<std::mutex> lk(sh->m);
            sh->out = std::move(local);
            sh->done = true;
        }
        sh->cv.notify_one();
    }).detach();

    std::unique_lock<std::mutex> lk(sh->m);
    const auto ready = [&] { return sh->done; };
    if (timeout_ms < 0) {          // mirror poll()'s "negative means forever"
        sh->cv.wait(lk, ready);
    } else if (!sh->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), ready)) {
        spdlog::critical("Timed out waiting for auth token on fd {}", fd);
        return {};
    }
    return sh->out;
}

#else

// Read from `fd` until the first newline, EOF, or the deadline. Returns the
// bytes read (newline included if seen); caller reduces to the first line.
// Bounded by poll() so a never-delivered token cannot hang the child forever.
std::string readFdUntilNewlineOrEof(int fd, int timeout_ms) {
    std::string out;
    char buf[256];
    for (;;) {
        struct pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr == 0) {
            spdlog::critical("Timed out waiting for auth token on fd {}", fd);
            return {};
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            spdlog::critical("poll() failed reading auth token: {}", std::strerror(errno));
            return {};
        }
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            if (out.find('\n') != std::string::npos) return out;  // got a full line
            continue;
        }
        if (n == 0) return out;                  // EOF: token is everything read
        if (errno == EINTR) continue;
        spdlog::critical("read() failed reading auth token: {}", std::strerror(errno));
        return {};
    }
}

#endif  // _WIN32

std::string readFromFile(const std::string& path, int timeout_ms) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        spdlog::critical("Failed to open token file {}: {}", path, std::strerror(errno));
        return {};
    }
    std::string out = readFdUntilNewlineOrEof(fd, timeout_ms);
    ::close(fd);
    return out;
}

}  // namespace

std::string read(const std::string& source, int timeout_ms) {
    std::string token;

    if (source.empty() || source == "stdin") {
        token = readFdUntilNewlineOrEof(STDIN_FILENO, timeout_ms);
    } else if (source.rfind("fd:", 0) == 0) {
        const std::string n = source.substr(3);
        char* end = nullptr;
        const long fd = std::strtol(n.c_str(), &end, 10);
        if (n.empty() || end == n.c_str() || *end != '\0' || fd < 0) {
            spdlog::critical("Invalid --token-source fd spec: {}", source);
            return {};
        }
        token = readFdUntilNewlineOrEof(static_cast<int>(fd), timeout_ms);
    } else if (source.rfind("file:", 0) == 0) {
        token = readFromFile(source.substr(5), timeout_ms);
    } else {
        spdlog::critical("Unknown --token-source: {} "
                         "(expected stdin, fd:<n>, or file:<path>)", source);
        return {};
    }

    firstLine(token);
    if (token.empty())
        spdlog::critical("No auth token received from source: {}",
                         source.empty() ? "stdin" : source.c_str());
    return token;
}

}  // namespace TokenSource
