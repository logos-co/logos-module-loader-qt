// =============================================================================
// Tests for the host-side TokenSource reader.
//
// The module host reads its auth token from a channel its container designates
// via --token-source (stdin | fd:<n> | file:<path>), with no dependency on any
// container implementation. These tests exercise each source over real pipes
// and a real temp file, plus the timeout and error paths.
// =============================================================================
#include <gtest/gtest.h>
#include "token_source.h"

#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>       // _pipe / _write on mingw
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

// mingw has no POSIX pipe(2): the CRT spells it _pipe and additionally wants a
// buffer size and a text/binary mode. _O_BINARY matters -- the token is
// compared byte for byte, and text mode would translate the "\r\n" the CRLF
// test deliberately writes.
#ifdef _WIN32
inline int makePipe(int fds[2]) { return ::_pipe(fds, 4096, _O_BINARY); }
inline int writeFd(int fd, const void* buf, std::size_t n) {
    return ::_write(fd, buf, static_cast<unsigned int>(n));
}
#else
inline int makePipe(int fds[2]) { return ::pipe(fds); }
inline ssize_t writeFd(int fd, const void* buf, std::size_t n) {
    return ::write(fd, buf, n);
}
#endif

// A temp file open for writing, plus its path. mingw has neither mkstemp nor
// a /tmp, so the Windows branch asks the OS for both. Returns -1 on failure.
#ifdef _WIN32
inline int makeTempFile(std::string& pathOut) {
    char dir[MAX_PATH]; char file[MAX_PATH];
    if (::GetTempPathA(MAX_PATH, dir) == 0) return -1;
    if (::GetTempFileNameA(dir, "lts", 0, file) == 0) return -1;
    pathOut = file;
    int fd = -1;
    if (::_sopen_s(&fd, file, _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
        return -1;
    return fd;
}
#else
inline int makeTempFile(std::string& pathOut) {
    char path[] = "/tmp/logos_token_src_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd >= 0) pathOut = path;
    return fd;
}
#endif

// Write `data` to a fresh pipe and return the read fd (caller closes it). The
// write end is closed after writing so the reader sees EOF.
int pipeWith(const std::string& data) {
    int fds[2];
    if (makePipe(fds) != 0) return -1;
    ssize_t off = 0;
    while (off < static_cast<ssize_t>(data.size())) {
        auto n = writeFd(fds[1], data.data() + off, data.size() - off);
        if (n <= 0) break;
        off += n;
    }
    ::close(fds[1]);   // EOF for the reader
    return fds[0];
}

} // namespace

TEST(TokenSource, ReadsFromFdStripsNewline) {
    const int rfd = pipeWith("my-secret-token\n");
    ASSERT_GE(rfd, 0);
    const std::string tok = TokenSource::read("fd:" + std::to_string(rfd));
    ::close(rfd);
    EXPECT_EQ(tok, "my-secret-token");
}

TEST(TokenSource, ReadsFromFdWithoutNewlineAtEof) {
    const int rfd = pipeWith("no-newline-token");
    ASSERT_GE(rfd, 0);
    const std::string tok = TokenSource::read("fd:" + std::to_string(rfd));
    ::close(rfd);
    EXPECT_EQ(tok, "no-newline-token");
}

TEST(TokenSource, ReadsOnlyFirstLine) {
    const int rfd = pipeWith("first-line\nsecond-line\n");
    ASSERT_GE(rfd, 0);
    const std::string tok = TokenSource::read("fd:" + std::to_string(rfd));
    ::close(rfd);
    EXPECT_EQ(tok, "first-line");
}

TEST(TokenSource, ReadsFromFile) {
    std::string path;
    const int fd = makeTempFile(path);
    ASSERT_GE(fd, 0);
    const char* contents = "file-token\n";
    ASSERT_GT(writeFd(fd, contents, std::strlen(contents)), 0);
    ::close(fd);

    const std::string tok = TokenSource::read(std::string("file:") + path);
    std::remove(path.c_str());
    EXPECT_EQ(tok, "file-token");
}

TEST(TokenSource, UnknownSourceReturnsEmpty) {
    EXPECT_TRUE(TokenSource::read("carrier-pigeon").empty());
}

TEST(TokenSource, InvalidFdSpecReturnsEmpty) {
    EXPECT_TRUE(TokenSource::read("fd:notanumber").empty());
}

TEST(TokenSource, MissingFileReturnsEmpty) {
    EXPECT_TRUE(TokenSource::read("file:/no/such/path/logos_token").empty());
}

TEST(TokenSource, TimesOutWhenNoDataArrives) {
    // A pipe whose write end stays open and silent: the read must hit the
    // timeout and return empty rather than block forever.
    int fds[2];
    ASSERT_EQ(makePipe(fds), 0);
    const auto t0 = std::chrono::steady_clock::now();
    const std::string tok = TokenSource::read("fd:" + std::to_string(fds[0]),
                                              /*timeout_ms=*/200);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    ::close(fds[0]);
    ::close(fds[1]);
    EXPECT_TRUE(tok.empty());
    EXPECT_GE(elapsed, 150) << "should wait out the timeout, not return early";
    EXPECT_LT(elapsed, 1500) << "should not block well past the timeout";
}
