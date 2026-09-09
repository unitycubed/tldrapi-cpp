# tldrapi-cpp — C++ SDK for TLDRapi

Header-only C++17 client for the
[TLDRapi](https://unitycubed.dev/TLDRapi/) text-summarization API.

- **Single header** — `#include <tldrapi/tldrapi.hpp>` and go
- **One link-time dependency**: libcurl (macOS ships it; Linux
  `libcurl4-openssl-dev`; Windows `vcpkg install curl`)
- **Zero third-party headers** — parses TLDRapi's JSON responses with
  a minimal built-in extractor. Callers wanting full JSON access can
  hand `raw_body` to nlohmann::json or rapidjson

## Install

Copy `include/tldrapi/tldrapi.hpp` into your project's include path, or
add this repo as a git submodule.

CMake users:

```cmake
target_include_directories(your_target PRIVATE path/to/tldrapi-cpp/include)
target_link_libraries(your_target PRIVATE curl)
```

## Compile

macOS (via xcrun):

```sh
xcrun --sdk macosx clang++ -std=c++17 -Iinclude examples/basic.cpp -lcurl -o basic
```

Linux:

```sh
c++ -std=c++17 -Iinclude examples/basic.cpp -lcurl -o basic
```

## Usage

```cpp
#include <tldrapi/tldrapi.hpp>
#include <iostream>

int main() {
    tldrapi::client c({.rapidapi_key = std::getenv("TLDRAPI_RAPIDAPI_KEY")});

    auto res = c.summarize(
        "Long text goes here.",
        {.tier = tldrapi::tier::quick});

    std::cout << res.summary << "\n";
    std::cout << "cost: $" << res.usage.total_cost << "\n";
}
```

## Error handling

```cpp
try {
    auto res = c.summarize(text, {.tier = tldrapi::tier::deep});
    // ...
} catch (const tldrapi::rate_limit_error& e) {
    std::this_thread::sleep_for(std::chrono::seconds(e.retry_after_seconds));
} catch (const tldrapi::insufficient_credits_error& e) {
    std::cerr << "top up: " << e.response_body << "\n";
} catch (const tldrapi::authentication_error& e) {
    std::cerr << "check your RapidAPI key\n";
} catch (const tldrapi::api_error& e) {
    std::cerr << "API error [" << e.status_code << "]: " << e.what() << "\n";
}
```

All exceptions inherit from `tldrapi::api_error` which inherits from
`std::runtime_error`. Catch broadly or narrowly as you prefer.

## Retries

5xx and CURL transport errors auto-retry 3× with exponential backoff.
4xx (including 429) is **never** auto-retried. Configure via
`client_options.retries`.

## Thread safety

`tldrapi::client` is safe to share across threads — each call uses a
fresh CURL easy handle. The client itself is stateless after construction.

## License

MIT — see [LICENSE](./LICENSE).
