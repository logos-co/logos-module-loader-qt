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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

// Write `data` to a fresh pipe and return the read fd (caller closes it). The
// write end is closed after writing so the reader sees EOF.
int pipeWith(const std::string& data) {
    int fds[2];
    if (::pipe(fds) != 0) return -1;
    ssize_t off = 0;
    while (off < static_cast<ssize_t>(data.size())) {
        ssize_t n = ::write(fds[1], data.data() + off, data.size() - off);
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
    char path[] = "/tmp/logos_token_src_XXXXXX";
    const int fd = ::mkstemp(path);
    ASSERT_GE(fd, 0);
    const char* contents = "file-token\n";
    ASSERT_GT(::write(fd, contents, std::strlen(contents)), 0);
    ::close(fd);

    const std::string tok = TokenSource::read(std::string("file:") + path);
    ::unlink(path);
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
    ASSERT_EQ(::pipe(fds), 0);
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
