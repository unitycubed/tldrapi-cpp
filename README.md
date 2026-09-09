# tldrapi-cpp — C++ SDK for TLDRapi

Header-only C++17 client for the
[TLDRapi](https://tldrapi-summarization.p.rapidapi.com/) text-summarization API.

- **Single header** — `#include <tldrapi/tldrapi.hpp>` and go
- **One link-time dependency**: libcurl (macOS ships it; Linux
  `libcurl4-openssl-dev`; Windows `vcpkg install curl`)
- **Zero third-party headers** — parses TLDRapi's JSON responses with
  a minimal built-in extractor. Callers wanting full JSON access can
  hand `raw_body` to nlohmann::json or rapidjson

## Get your app's RapidAPI key

1. Sign in at [rapidapi.com](https://rapidapi.com)
2. Subscribe to the [TLDRapi Summarizer](https://rapidapi.com/thunderAPIs256/api/tldrapi-summarizer) listing (start with **BASIC** — free)
3. Go to **Console** (top nav) → **Applications** → **Add App** (or open an existing one)
4. In the App → **Authorizations** tab → click the copy icon next to your Authorization Key

That's the app's `X-RapidAPI-Key`. Pass it to the SDK constructor.

*Legacy path (deprecated): upper-right (?) → Legacy Developer Dashboard → Add New App → Authorization tab. The new Console path above is simpler.*

The Authorization Key field is the same value in both places — RapidAPI just labels it differently depending on which interface you use:

**New Console:**

![RapidAPI Console — Authorization Method labeled "RAPIDAPI"](https://raw.githubusercontent.com/unitycubed/tldrapi-docs/main/img/rapidapi-key-label-console.png)

**Legacy Developer Dashboard:**

![RapidAPI Legacy Developer Dashboard — Authorization Method labeled "API key"](https://raw.githubusercontent.com/unitycubed/tldrapi-docs/main/img/rapidapi-key-label-legacy.png)



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

Released under the MIT License — see [LICENSE](LICENSE).

Copyright (c) 2026 Ehren Biglari / Unity Cubed.
