#include "transport_set_arg.h"

namespace {

int sextet(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Whole quartets only, padding only at the end.
bool base64Decode(const std::string& in, std::string& out)
{
    if (in.empty() || in.size() % 4 != 0) return false;
    const std::size_t padding = in.back() != '=' ? 0 : in[in.size() - 2] != '=' ? 1 : 2;
    out.clear();
    unsigned buffer = 0;
    int bits = 0;
    for (std::size_t i = 0; i < in.size() - padding; ++i) {
        const int value = sextet(in[i]);
        if (value < 0) return false;
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return true;
}

} // namespace

std::string decodeTransportSetArg(const std::string& arg)
{
    if (arg.empty() || arg.front() == '{' || arg.front() == '[') return arg;
    std::string decoded;
    return base64Decode(arg, decoded) ? decoded : arg;
}
