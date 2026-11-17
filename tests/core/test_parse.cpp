#include <doctest/doctest.h>

#include <string_view>

#include "crowdsim/core/Parse.hpp"

using crowdsim::core::parseFloat;

TEST_CASE("parseFloat reads a number prefix like from_chars") {
    const std::string_view text = "0.25,3";
    float v = -1.0f;
    const auto r = parseFloat(text.data(), text.data() + text.size(), v);
    CHECK(r.ok);
    CHECK(v == doctest::Approx(0.25f));
    CHECK(r.ptr == text.data() + 4);  // stops at the comma
}

TEST_CASE("parseFloat leaves the value alone when there is no number") {
    const std::string_view text = "fast";
    float v = 7.0f;
    const auto r = parseFloat(text.data(), text.data() + text.size(), v);
    CHECK_FALSE(r.ok);
    CHECK(v == 7.0f);
    CHECK(r.ptr == text.data());
}

TEST_CASE("parseFloat does not read past the end of the range") {
    const std::string_view text = "1.5e3";
    float v = 0.0f;
    const auto r = parseFloat(text.data(), text.data() + 3, v);  // "1.5" only
    CHECK(r.ok);
    CHECK(v == doctest::Approx(1.5f));
    CHECK(r.ptr == text.data() + 3);
}
