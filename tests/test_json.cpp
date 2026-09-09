// Zero-dep unit tests for the JSON helpers in tldrapi.hpp.
// Compile:
//   xcrun --sdk macosx clang++ -std=c++17 -Wall -Wextra \
//     -Iinclude tests/test_json.cpp -lcurl -o /tmp/tldrapi_test
//   /tmp/tldrapi_test
//
// We exercise the detail::* helpers directly rather than the full
// client — a real integration test would need a mock HTTPS server,
// which we defer to real prod-canary hits after publish.

#include <cassert>
#include <cstdio>
#include <string>
#include <tldrapi/tldrapi.hpp>

using namespace tldrapi;

static int failures = 0;
#define EXPECT_EQ(a, b) do {                                                                  \
    if ((a) != (b)) {                                                                          \
        std::fprintf(stderr, "FAIL %s:%d  got=%s want=%s\n", __FILE__, __LINE__,               \
                     std::to_string(a).c_str(), std::to_string(b).c_str());                    \
        ++failures;                                                                            \
    }                                                                                          \
} while (0)
#define EXPECT_STREQ(a, b) do {                                                                \
    std::string _a = (a); std::string _b = (b);                                                \
    if (_a != _b) {                                                                            \
        std::fprintf(stderr, "FAIL %s:%d  got=\"%s\" want=\"%s\"\n", __FILE__, __LINE__,       \
                     _a.c_str(), _b.c_str());                                                  \
        ++failures;                                                                            \
    }                                                                                          \
} while (0)

int main() {
    // get_string basics
    EXPECT_STREQ(detail::get_string(R"({"summary":"hi"})", "summary"), "hi");
    EXPECT_STREQ(detail::get_string(R"({"summary":"hi there"})", "summary"), "hi there");
    EXPECT_STREQ(detail::get_string(R"({"x":"a","summary":"b"})", "summary"), "b");
    EXPECT_STREQ(detail::get_string(R"({"missing":"x"})", "summary"), "");

    // get_string with escaped chars
    EXPECT_STREQ(detail::get_string(R"({"s":"he said \"hi\""})", "s"), "he said \"hi\"");
    EXPECT_STREQ(detail::get_string(R"({"s":"line1\nline2"})", "s"), "line1\nline2");
    EXPECT_STREQ(detail::get_string(R"({"s":"tab\there"})", "s"), "tab\there");

    // get_number
    EXPECT_EQ(static_cast<int>(detail::get_number(R"({"n":42})", "n")), 42);
    EXPECT_EQ(static_cast<int>(detail::get_number(R"({"n":-7})", "n")), -7);
    EXPECT_EQ(static_cast<int>(detail::get_number(R"({"x":1,"n":99})", "n")), 99);
    EXPECT_EQ(static_cast<int>(detail::get_number(R"({"n":0.5})", "n")), 0);
    EXPECT_EQ(static_cast<int>(detail::get_number(R"({"none":"x"})", "n")), 0);

    // get_object — nested { ... } captured whole
    std::string obj = detail::get_object(R"({"usage":{"input":5,"output":3}})", "usage");
    EXPECT_STREQ(obj, R"({"input":5,"output":3})");
    // Nested strings with braces don't confuse it
    std::string tricky = detail::get_object(R"({"u":{"msg":"has } brace"}})", "u");
    EXPECT_STREQ(tricky, R"({"msg":"has } brace"})");

    // Round-trip: server response shape
    std::string resp = R"({
        "summary": "The fox is brown.",
        "session_id": "sess-1",
        "usage": {"input_tokens": 12, "output_tokens": 5, "total_cost": 0.0001, "model_used": "openrouter-llama-3.1-8b"}
    })";
    EXPECT_STREQ(detail::get_string(resp, "summary"), "The fox is brown.");
    EXPECT_STREQ(detail::get_string(resp, "session_id"), "sess-1");
    auto usage = detail::get_object(resp, "usage");
    EXPECT_EQ(static_cast<int>(detail::get_number(usage, "input_tokens")), 12);
    EXPECT_STREQ(detail::get_string(usage, "model_used"), "openrouter-llama-3.1-8b");

    // escape_json — inverse-ish
    EXPECT_STREQ(detail::escape_json("hi"), "hi");
    EXPECT_STREQ(detail::escape_json("he said \"hi\""), R"(he said \"hi\")");
    EXPECT_STREQ(detail::escape_json("a\nb"), "a\\nb");
    EXPECT_STREQ(detail::escape_json("back\\slash"), "back\\\\slash");

    // Tier enum stringifier
    EXPECT_STREQ(tier_to_string(tier::quick), "quick");
    EXPECT_STREQ(tier_to_string(tier::ultra), "ultra");
    EXPECT_STREQ(tier_to_string(tier::none), "");

    // client constructor rejects empty key
    try {
        client c({});
        std::fprintf(stderr, "FAIL: expected exception for empty key\n");
        ++failures;
    } catch (const std::invalid_argument&) { /* expected */ }

    // client constructor accepts valid key + fills defaults
    client c({.rapidapi_key = "test"});
    (void)c;  // just want to prove it doesn't throw

    if (failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d test(s) FAILED\n", failures);
    return 1;
}
