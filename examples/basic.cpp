// Minimal example. Compile:
//   c++ -std=c++17 -Iinclude examples/basic.cpp -lcurl -o basic
// Run:
//   TLDRAPI_RAPIDAPI_KEY=your_key ./basic

#include <cstdlib>
#include <iostream>
#include <tldrapi/tldrapi.hpp>

int main() {
    const char* key = std::getenv("TLDRAPI_RAPIDAPI_KEY");
    if (!key || !*key) {
        std::cerr << "set TLDRAPI_RAPIDAPI_KEY\n";
        return 1;
    }
    try {
        tldrapi::client c({.rapidapi_key = key});
        auto res = c.summarize(
            "The quick brown fox jumps over the lazy dog. "
            "Then it jumps back. It repeats this several times per day.",
            {.tier = tldrapi::tier::quick});
        std::cout << "summary: " << res.summary << "\n";
        std::cout << "cost:    $" << res.usage.total_cost << "\n";
        std::cout << "model:   " << res.usage.model_used << "\n";
        std::cout << "remain:  " << res.credits.remaining << "\n";
    } catch (const tldrapi::rate_limit_error& e) {
        std::cerr << "rate limited; retry after " << e.retry_after_seconds << "s\n";
        return 2;
    } catch (const tldrapi::api_error& e) {
        std::cerr << "API error [" << e.status_code << "]: " << e.what() << "\n";
        return 3;
    }
    return 0;
}
