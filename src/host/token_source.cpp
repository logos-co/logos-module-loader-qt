#include "token_source.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

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
