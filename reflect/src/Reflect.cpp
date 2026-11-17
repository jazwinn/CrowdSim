#include "crowdsim/reflect/Reflect.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace crowdsim::reflect {

double roundFloat(float v) {
    if (!std::isfinite(v)) {
        return static_cast<double>(v);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    return std::strtod(buf, nullptr);
}

std::string colorString(std::uint32_t abgr) {
    return std::format("#{:02x}{:02x}{:02x}", abgr & 0xffu, (abgr >> 8) & 0xffu, (abgr >> 16) & 0xffu);
}

std::uint32_t colorFromString(const std::string& s) {
    std::uint32_t rgb = 0;
    const char* first = s.data() + 1;
    const char* last = s.data() + s.size();
    const auto [ptr, ec] = s.size() == 7 && s[0] == '#' ? std::from_chars(first, last, rgb, 16)
                                                        : std::from_chars_result{first, std::errc::invalid_argument};
    if (ec != std::errc{} || ptr != last) {
        throw FieldError({}, "expected colour \"#rrggbb\", got \"" + s + "\"");
    }
    const std::uint32_t r = (rgb >> 16) & 0xffu;
    const std::uint32_t g = (rgb >> 8) & 0xffu;
    const std::uint32_t b = rgb & 0xffu;
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

std::string checkBounds(double shown, const Meta& m) {
    auto num = [](double v) { return std::format("{:g}", v); };
    const bool tooLow = m.minExclusive ? !(shown > m.min) : shown < m.min;
    if (tooLow || shown > m.max) {
        if (std::isfinite(m.min) && std::isfinite(m.max)) {
            return std::format("must be {} {} and at most {}", m.minExclusive ? "above" : "at least", num(m.min),
                               num(m.max));
        }
        if (std::isfinite(m.min)) {
            return std::format("must be {} {}", m.minExclusive ? "above" : "at least", num(m.min));
        }
        return "must be at most " + num(m.max);
    }
    return {};
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool sameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

}  // namespace crowdsim::reflect
