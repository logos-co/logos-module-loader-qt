#ifndef TOKEN_SOURCE_H
#define TOKEN_SOURCE_H

#include <string>

// Container-agnostic auth-token reader for the module host.
//
// The host (logos_host) needs its auth token before it initializes LogosAPI,
// but it must not know *which* container spawned it. Each container delivers
// the token over whatever channel fits its isolation model and names that
// channel via the --token-source CLI argument; the host just reads bytes from
// the designated handle. This keeps the host free of any dependency on a
// specific container implementation (no logos-container-subprocess, not even
// logos-container) — it is plain libc.
//
// Supported sources (extensible — a Docker or sandbox container can pick a
// different one without changing the host):
//   stdin        read the token from fd 0 (the default; used by the subprocess
//                container, which writes it to the child's stdin pipe)
//   fd:<n>       read the token from an inherited file descriptor <n>
//   file:<path>  read the token from a file (e.g. a Docker secret mount)
//
// The token is the bytes up to the first newline (or end-of-stream); a single
// trailing '\n' / '\r' is stripped. Reads are bounded by a timeout so a child
// never blocks forever if the token is never delivered.
namespace TokenSource {

// Read the token from the channel described by `source` (the value of
// --token-source; empty means "stdin"). Returns the token, or an empty string
// on error/timeout. `timeout_ms` bounds how long to wait for data to arrive.
std::string read(const std::string& source, int timeout_ms = 10000);

}  // namespace TokenSource

#endif  // TOKEN_SOURCE_H
