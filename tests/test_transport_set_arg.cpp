#include <gtest/gtest.h>

#include "transport_set_arg.h"

// What qt_plugin_format_loader.cpp's base64Encode emits for these sets.
TEST(TransportSetArg, DecodesTheLoadersBase64)
{
    EXPECT_EQ(decodeTransportSetArg("W3sicHJvdG9jb2wiOiJsb2NhbCJ9XQ=="),
              R"([{"protocol":"local"}])");
    EXPECT_EQ(decodeTransportSetArg("W3sicHJvdG9jb2wiOiJ0Y3AiLCJwb3J0Ijo2MDAxfV0="),
              R"([{"protocol":"tcp","port":6001}])");
}

TEST(TransportSetArg, PassesRawJsonThrough)
{
    EXPECT_EQ(decodeTransportSetArg(R"([{"protocol":"local"}])"), R"([{"protocol":"local"}])");
    EXPECT_EQ(decodeTransportSetArg("{}"), "{}");
    EXPECT_EQ(decodeTransportSetArg(""), "");
}

// Left for the transport-set parser to reject, with the text as given.
TEST(TransportSetArg, PassesInvalidBase64Through)
{
    for (const char* invalid : {"abc", "a===", "ab=c", "W3s!", "not base64 at all"})
        EXPECT_EQ(decodeTransportSetArg(invalid), invalid);
}
