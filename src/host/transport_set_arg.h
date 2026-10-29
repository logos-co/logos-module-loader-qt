#pragma once

#include <string>

// --transport-set arrives base64-encoded (qt_plugin_format_loader.cpp's
// base64Encode): Windows' CommandLineToArgvW eats the quotes of raw JSON.
// Raw JSON from an older emitter starts with '{' or '[', outside the base64
// alphabet, and passes through; so does anything that is not valid base64,
// for the transport-set parser to reject.
std::string decodeTransportSetArg(const std::string& arg);
