#pragma once

/// @file Parse.hpp
/// Number parsing that works on every standard library we build with.

#include <cstdlib>
#include <string>

namespace crowdsim::core {

struct FloatParse {
    const char* ptr = nullptr;  ///< first character not parsed
    bool ok = false;
};

/// Parse a float at the start of [first, last), like std::from_chars. Floating-point
/// from_chars is deleted in Apple's libc++, so this goes through strtof.
inline FloatParse parseFloat(const char* first, const char* last, float& out) {
    const std::string copy(first, last);
    char* end = nullptr;
    const float v = std::strtof(copy.c_str(), &end);
    if (end == copy.c_str()) {
        return {first, false};
    }
    out = v;
    return {first + (end - copy.c_str()), true};
}

}  // namespace crowdsim::core
