#pragma once

#include <Arduino.h>
#include <cmath>
#include <functional>
#include <sstream>
#include <string>

#define BoolToString(x) ((x) ? "true" : "false")

// Escape a string into a JSON double-quoted literal. Matches the previous
// SimpleJSON StringifyString behavior (0x00-0x1F and 0x7F -> \u00xx lowercase,
// escapes " \ / \b \f \n \r \t, UTF-8 passes through unchanged).
inline std::string jsonEscape(const std::string &str)
{
    std::string out = "\"";
    for (size_t i = 0; i < str.size(); ++i) {
        char chr = str[i];
        if (chr == '"' || chr == '\\' || chr == '/') {
            out += '\\';
            out += chr;
        } else if (chr == '\b') {
            out += "\\b";
        } else if (chr == '\f') {
            out += "\\f";
        } else if (chr == '\n') {
            out += "\\n";
        } else if (chr == '\r') {
            out += "\\r";
        } else if (chr == '\t') {
            out += "\\t";
        } else if ((unsigned char)chr < 0x20 || chr == 0x7F) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)chr);
            out += buf;
        } else {
            out += chr;
        }
    }
    out += "\"";
    return out;
}

// Format a numeric value the way the previous SimpleJSON serializer did
// (std::stringstream with precision 15, NaN/Inf -> "null").
inline std::string jsonNum(double v)
{
    if (std::isinf(v) || std::isnan(v))
        return "null";
    std::ostringstream ss;
    ss.precision(15);
    ss << v;
    return ss.str();
}
