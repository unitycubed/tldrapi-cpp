# tldrapi-cpp — C++ SDK for TLDRapi

Header-only C++17 client for [TLDRapi](https://tldrapi.com) — turn any
content into a clean summary in one API call.

- **Free tier** — 100 credits per month, no card, no trial expiry
- **20+ input formats** — text, HTML, Markdown, PDF (with OCR), .docx,
  .doc, .odt, .rtf, .epub, JSON, YAML, CSV, transcripts
- **5 quality tiers** — pick latency vs. depth per call
- **Custom voice styles** — 20+ built-in voices; paid tiers can define
  their own with plain-English instructions
- **Multi-provider routing** — automatic failover across Anthropic,
  OpenAI, Groq, Gemini, and OpenRouter
- **Refunds you don't have to ask for** — every summary is judge-scored
  and mis-summaries are auto-refunded
- **Single header, one link-time dep** (libcurl); zero third-party
  headers — parses TLDRapi's JSON with a built-in extractor. Callers
  wanting full JSON access can hand `raw_body` to nlohmann::json /
  rapidjson

## Install

Copy `include/tldrapi/tldrapi.hpp` into your project's include path, or
add this repo as a git submodule.

CMake users:

```cmake
target_include_directories(your_target PRIVATE path/to/tldrapi-cpp/include)
target_link_libraries(your_target PRIVATE curl)
```

Prereqs: macOS ships libcurl; Linux `apt install libcurl4-openssl-dev`;
Windows `vcpkg install curl`.

## Compile

macOS (via xcrun):

```sh
xcrun --sdk macosx clang++ -std=c++17 -Iinclude examples/basic.cpp -lcurl -o basic
```

Linux:

```sh
c++ -std=c++17 -Iinclude examples/basic.cpp -lcurl -o basic
```

## Table of contents

- [Getting your free key](#getting-your-free-key)
- [Hello world](#hello-world)
- [Examples gallery](#examples-gallery)
  - [Summarize an article by URL](#summarize-an-article-by-url)
  - [Pin a session across many summaries](#pin-a-session-across-many-summaries)
  - [Handle a rate-limit with backoff](#handle-a-rate-limit-with-backoff)
  - [Show live credit balance to your user](#show-live-credit-balance-to-your-user)
  - [Advanced quality controls — 3 axes, 30 named presets](#advanced-quality-controls)
- [Quality tiers](#quality-tiers)
- [Async submit + poll](#async-submit--poll)
- [Error handling](#error-handling)
- [Retries + timeouts](#retries--timeouts)
- [Thread safety](#thread-safety)
- [License](#license)

## Getting your free key

1. Sign in at [rapidapi.com](https://rapidapi.com)
2. Subscribe to the [TLDRapi Summarizer](https://rapidapi.com/thunderAPIs256/api/tldrapi-summarizer)
   listing — choose **BASIC (Free)**
3. Open the listing → **Console** → **Applications** → **Add App**
4. In the App → **Authorizations** tab → copy the Authorization Key

Pass it to `tldrapi::client` as `rapidapi_key`. Everything on the free
tier works exactly like the paid tiers — same endpoints, same response
shape, same SDK — just with a 100-credit monthly cap.

## Hello world

```cpp
#include <tldrapi/tldrapi.hpp>
#include <iostream>

int main() {
    tldrapi::client c({.rapidapi_key = std::getenv("TLDRAPI_RAPIDAPI_KEY")});

    auto res = c.summarize("Some long article body here...");

    std::cout << res.summary << "\n";
    std::cout << "credits remaining: " << res.credits.remaining << "\n";
}
```

## Examples gallery

### Summarize an article by URL

TLDRapi accepts URLs directly — the server fetches, extracts main
content, strips nav/ads, and summarizes.

```cpp
auto r = c.summarize("https://arxiv.org/abs/1706.03762",
                     {.tier = tldrapi::tier::deep});
std::cout << r.summary << "\n";
```

Works with HTML pages, news sites, GitHub READMEs, blog posts, and
academic PDFs served over HTTP.

### Pin a session across many summaries

```cpp
auto r1 = c.summarize("Doc 1");

tldrapi::SummarizeOptions o2;
o2.session_id = r1.session_id;
auto r2 = c.summarize("Doc 2", o2);

tldrapi::SummarizeOptions o3;
o3.session_id = r1.session_id;
auto r3 = c.summarize("Doc 3", o3);
```

Useful when you want consistent voice across a run — chapters of the same book, articles in a series, tickets in the same support thread

### Handle a rate-limit with backoff

```cpp
for (int attempt = 0; attempt < 3; ++attempt) {
    try {
        auto r = c.summarize(text, {.tier = tldrapi::tier::deep});
        std::cout << r.summary << "\n";
        break;
    } catch (const tldrapi::rate_limit_error& e) {
        int secs = e.retry_after_seconds > 0 ? e.retry_after_seconds : 60;
        std::this_thread::sleep_for(std::chrono::seconds(secs));
    }
}
```

### Show live credit balance to your user

```cpp
auto u = c.usage();
std::cout << "You have " << u.credits_remaining
          << " credits left (" << u.plan << ")\n";

auto r = c.summarize(text);
std::cout << "That call cost " << r.credits.charged
          << " credits. Remaining: " << r.credits.remaining << "\n";
```

### Advanced quality controls

Every summarize call has three orthogonal knobs. You can send zero of
them (defaults are fine), or a named preset via `tier`, or set 1-3
optional axes via `extra_headers`, or combine — axes override the
preset and the server returns `X-Quality-Warning`.

**30 named presets** arranged as a 1D spectrum across the underlying 3D
quality space (LLM x retention x strategy). The 5 bolded rows are the
main anchors; each also accepts a short alias equal to its LLM tier
name (`quick` / `standard` / `deep` / `premium` / `ultra`).

| #  | Preset                | What it delivers                                                                                       |
|---:|-----------------------|--------------------------------------------------------------------------------------------------------|
|  1 | `minimal-quick`       | Cheapest and fastest. Headline-length blurb from a small chunk. Title-level takeaway.                  |
|  2 | **`brief-quick`**     | 3-sentence recap with the fastest LLM. Previews and low-latency feed cards.                            |
|  3 | `minimal-standard`    | Headline blurb with the mid-tier LLM's fluency; still very cheap.                                      |
|  4 | `balanced-quick`      | 3-5 sentences from the fast LLM; slightly deeper than `brief-quick`.                                   |
|  5 | `brief-standard`      | 3-sentence recap with smoother phrasing than `brief-quick`.                                            |
|  6 | **`balanced-standard`** | Balanced coverage without run-ons. The general default for most articles.                            |
|  7 | `thorough-quick`      | Paragraph-length from the fast LLM; retains the top 2-3 supporting facts.                              |
|  8 | `minimal-deep`        | Headline output with the deeper LLM's coherence; frugal way to buy fluency without length.             |
|  9 | `brief-deep`          | 3-sentence recap with deeper-model reasoning.                                                          |
| 10 | **`thorough-deep`**   | Preserves specific dates, names, secondary facts. Research papers, meeting transcripts, long articles. |
| 11 | `detailed-quick`      | Longer paragraph from the fast LLM; more supporting facts, still light on nuance.                      |
| 12 | `thorough-standard`   | Retains dates and names on Standard-class content; great for meeting-transcript recaps.                |
| 13 | `complete-quick`      | Maximum retention the Quick LLM can produce; nearing Standard breadth but Quick tone.                  |
| 14 | `balanced-deep`       | 4-6 sentences with deep-model narrative flow.                                                          |
| 15 | `detailed-standard`   | Full-paragraph, entity-preserving; approaches Deep on retention.                                       |
| 16 | `complete-standard`   | Maximum Standard retention; substantial output length.                                                 |
| 17 | `detailed-deep`       | Heavy retention with deep-model reasoning; picks up minor arguments.                                   |
| 18 | `complete-deep`       | Maximum Deep retention; edging into Premium coverage.                                                  |
| 19 | `minimal-premium`     | Very short output with premium-model tone; premium quality at bargain length.                          |
| 20 | `brief-premium`       | 3-sentence recap with high-fidelity entity handling.                                                   |
| 21 | **`detailed-premium`** | Entity preservation, edge cases, atmospheric detail. Substantial documents and long-form reports.     |
| 22 | `balanced-premium`    | Moderate-length premium coverage; smoother than Deep, more concise than `detailed-premium`.            |
| 23 | `thorough-premium`    | Heavy retention with premium reasoning.                                                                |
| 24 | `minimal-ultra`       | Single-shot on the full document, minimum output length. Ultra fidelity, tiny output.                  |
| 25 | `brief-ultra`         | Full-context single-shot, 3-sentence output. Ideal for research-grade preview blurbs.                  |
| 26 | **`complete-ultra`**  | Single-shot on the full document, maximum retention, no chunking artifacts. Book-length manuscripts, long-form technical documentation. |
| 27 | `complete-premium`    | Maximum Premium retention; almost every noteworthy fact.                                               |
| 28 | `balanced-ultra`      | Full-context single-shot, balanced-length output.                                                      |
| 29 | `thorough-ultra`      | Full-context, retains most secondary facts.                                                            |
| 30 | `detailed-ultra`      | Full-context, near-maximum retention; the top rung of the spectrum.                                    |


**Three optional axis overrides**, sent as headers via `extra_headers`
(a `std::map<std::string, std::string>`):

- `X-Optional-Quality` — LLM tier: `quick | standard | deep | premium | ultra`
- `X-Optional-Extractive-Lvl` — retention level: `minimal | brief | balanced | thorough | detailed | complete`
- `X-Optional-Strategy` — inference strategy: `contextual-compression | premium-single-shot | hierarchical-merge`
- `X-Allow-Downgrade` — set `"true"` to opt into permissive paid-tier downgrade
- `X-Async` — set `"true"` to submit async; response is HTTP 202 with `X-Paid-Request-Id`

```cpp
// named preset via short enum
auto r = c.summarize(text, {.tier = tldrapi::tier::premium});

// compound preset via header
tldrapi::SummarizeOptions opts;
opts.extra_headers["X-Quality"] = "thorough-standard";
r = c.summarize(text, opts);

// preset + one axis override (axes win, warning header returned)
opts = {.tier = tldrapi::tier::premium};
opts.extra_headers["X-Optional-Extractive-Lvl"] = "brief";
r = c.summarize(text, opts);

// all three axes, no preset
opts = {};
opts.extra_headers["X-Optional-Quality"] = "ultra";
opts.extra_headers["X-Optional-Extractive-Lvl"] = "complete";
opts.extra_headers["X-Optional-Strategy"] = "premium-single-shot";
r = c.summarize(text, opts);

// permissive downgrade on paid-tier
opts = {.tier = tldrapi::tier::premium};
opts.extra_headers["X-Allow-Downgrade"] = "true";
r = c.summarize(text, opts);
```

Native `submit_async` / `get_result` / `wait_for_result` methods land
in the next SDK release; today, use libcurl or your own HTTP client
against `/paid/result/{id}` to poll.

## Quality tiers

| Tier      | Reads at once   | Best for                          |
|-----------|----------------:|-----------------------------------|
| quick     |     4K tokens   | Short texts, previews             |
| standard  |    16K tokens   | Default — most articles           |
| deep      |    32K tokens   | Longer content, deeper reasoning  |
| premium   |    64K tokens   | Substantial documents             |
| ultra     |   100K tokens   | Long-form / research-grade        |

Full pricing detail (methodology, formula, dynamic-pricing audit trail): [tldrapi.com/pricing](https://tldrapi.com/pricing). Live rates via `/rates` or the SDK's `rates()` method.rates()`.

### Paid-tier quality guarantees

Default = strict wait for the tier's canonical primary model. Opt into
permissive fallback with `extra_headers["X-Allow-Downgrade"] = "true"`
— the worker walks DOWN the ladder (premium → deep → standard →
quick) and returns whichever tier's primary is available. Response
carries `X-Quality-Actual` and `X-Original-Tier` when a downgrade
happened, and the credit-cost delta is automatically refunded.

## Async submit + poll

Reachable today via `extra_headers` + a manual poll:

```
POST /summarize   with X-Async: true      → 202 + X-Paid-Request-Id
GET  /paid/result/{id}
   200 → SummarizeResult in body
   202 → still queued
   410 → expired (past the 1h cache TTL)
```

Credits are deducted at submit time and refunded on failure exactly
like sync. Native helpers land in the next SDK release.

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
`std::runtime_error`. Catch broadly or narrowly. Every exception
carries `.status_code`, `.request_id` (attach when reporting bugs),
and `.response_body`.

## Retries + timeouts

5xx and libcurl transport errors auto-retry 3× with exponential
backoff + jitter. 4xx (including 429) is **never** auto-retried —
that would burn credits or worsen a throttle. Configure via
`client_options.retries` (set to 0 to disable).

Per-request timeout defaults to 60s. Override via
`client_options.timeout_seconds` globally, or per call via
`SummarizeOptions::timeout_seconds`.

## Thread safety

`tldrapi::client` is safe to share across threads — each call uses a
fresh CURL easy handle. The client itself is stateless after
construction.

## License

MIT — see [LICENSE](./LICENSE).

Copyright (c) 2026 Ehren Biglari / Unity Cubed.
